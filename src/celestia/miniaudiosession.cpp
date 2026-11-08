#include "miniaudiosession.h"

#include <memory>

#include <celutil/fsutils.h>
#include <celutil/logger.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

using namespace celestia::util;

namespace celestia
{

namespace
{

// RAII-managed shared ma_context + ma_engine. A single instance is shared
// across every live MiniAudioSession via shared_ptr; the weak_ptr at file
// scope below lets new sessions latch onto the existing instance, and the
// last session out lets the shared_ptr's refcount destroy it.
//
// Sharing also prevents per-channel teardown from releasing the iOS
// audio session while another channel still needs it. In managed iOS
// builds, this backend holds one lease alongside any PiP owners.
struct AudioBackend
{
    ma_context context;
    ma_engine  engine;
    bool       valid = false;
#ifdef CELESTIA_MANAGE_IOS_AUDIO_SESSION
    bool       audioSessionAcquired = false;
#endif

    AudioBackend()
    {
        auto config = ma_context_config_init();
#ifdef CELESTIA_MANAGE_IOS_AUDIO_SESSION
        if (!acquireScriptAudioSession())
            return;
        audioSessionAcquired = true;
        config.coreaudio.sessionCategory = ma_ios_session_category_none;
        config.coreaudio.noAudioSessionActivate = MA_TRUE;
        config.coreaudio.noAudioSessionDeactivate = MA_TRUE;
#else
        // on iOS, explicitly set the correct category for correct routing
        config.coreaudio.sessionCategory = ma_ios_session_category_playback;
#endif
        if (ma_context_init(nullptr, 0, &config, &context) != MA_SUCCESS)
        {
            GetLogger()->error("Failed to init miniaudio context");
            return;
        }
        auto engineConfig = ma_engine_config_init();
        engineConfig.pContext = &context;
        if (ma_engine_init(&engineConfig, &engine) != MA_SUCCESS)
        {
            ma_context_uninit(&context);
            GetLogger()->error("Failed to start miniaudio engine");
            return;
        }
        valid = true;
    }

    ~AudioBackend()
    {
        if (valid)
        {
            ma_engine_uninit(&engine);
            ma_context_uninit(&context);
        }
#ifdef CELESTIA_MANAGE_IOS_AUDIO_SESSION
        if (audioSessionAcquired)
            releaseScriptAudioSession();
#endif
    }

    AudioBackend(const AudioBackend&)            = delete;
    AudioBackend(AudioBackend&&)                 = delete;
    AudioBackend &operator=(const AudioBackend&) = delete;
    AudioBackend &operator=(AudioBackend&&)      = delete;
};

std::weak_ptr<AudioBackend> g_audioBackend; //NOSONAR

// Returns the live shared backend, creating it on demand. The caller
// pins it for the lifetime of its session by holding the returned
// shared_ptr; when the last holder lets go, ~AudioBackend uninits the
// engine and context.
std::shared_ptr<AudioBackend>
acquireAudioBackend()
{
    auto backend = g_audioBackend.lock();
    if (backend)
        return backend;

    backend = std::make_shared<AudioBackend>();
    if (!backend->valid)
        return nullptr;
    g_audioBackend = backend;
    return backend;
}

} // namespace

class MiniAudioSessionPrivate
{
 public:
    enum class State : int
    {
        NotInitialized      = 0,
        EngineStarted       = 1,
        SoundInitialzied    = 2,
        Playing             = 3,
    };

    MiniAudioSessionPrivate() = default;
    ~MiniAudioSessionPrivate();

    MiniAudioSessionPrivate(const MiniAudioSessionPrivate&) = delete;
    MiniAudioSessionPrivate(MiniAudioSessionPrivate&&)      = delete;
    MiniAudioSessionPrivate &operator=(const MiniAudioSessionPrivate&) = delete;
    MiniAudioSessionPrivate &operator=(MiniAudioSessionPrivate&&) = delete;

    // backend must outlive `sound` since ma_sound is bound to the
    // backend's ma_engine. Declare it first so it's destroyed last
    // (member destruction order is the reverse of declaration).
    std::shared_ptr<AudioBackend> backend;
    ma_sound sound;
    State state         { State::NotInitialized };
};

MiniAudioSessionPrivate::~MiniAudioSessionPrivate()
{
    switch (state)
    {
    case State::Playing:
        if (ma_sound_is_playing(&sound) == MA_TRUE)
            ma_sound_stop(&sound);
    case State::SoundInitialzied:
        ma_sound_uninit(&sound);
    case State::EngineStarted:
    case State::NotInitialized:
        // `backend` (declared above `sound`) is released after `sound`
        // is destroyed. If we hold the last reference, ~AudioBackend
        // uninits the engine + context; otherwise other live sessions
        // keep them alive.
        break;
    }
}

MiniAudioSession::MiniAudioSession(const std::filesystem::path &path, float volume, float pan, bool loop, bool nopause) :
    AudioSession(path, volume, pan, loop, nopause),
    p(std::make_unique<MiniAudioSessionPrivate>())
{
}

MiniAudioSession::~MiniAudioSession() = default;

bool MiniAudioSession::play(double startTime)
{
    ma_result result;
    switch (p->state)
    {
    case MiniAudioSessionPrivate::State::NotInitialized:
        // ensure the shared engine is up
        if (!startEngine())
            return false;
        p->state = MiniAudioSessionPrivate::State::EngineStarted;

    case MiniAudioSessionPrivate::State::EngineStarted:
        // load sound file from disk into our own ma_sound, but bound to
        // the shared engine
        result = ma_sound_init_from_file(&p->backend->engine, celestia::util::PathToString(path()).c_str(), MA_SOUND_FLAG_ASYNC, nullptr, nullptr, &p->sound);
        if (result != MA_SUCCESS)
        {
            GetLogger()->error("Failed to load sound file {}", path());
            return false;
        }
        ma_sound_set_volume(&p->sound, volume());
        ma_sound_set_pan(&p->sound, pan());
        ma_sound_set_looping(&p->sound, loop() ? MA_TRUE : MA_FALSE);
        p->state = MiniAudioSessionPrivate::State::SoundInitialzied;

    case MiniAudioSessionPrivate::State::SoundInitialzied:
        // start playing, seek if needed
        if (startTime >= 0 && !seek(startTime))
            return false;

        result = ma_sound_start(&p->sound);
        if (result != MA_SUCCESS)
        {
            GetLogger()->error("Failed to start playing sound file {}", path());
            return false;
        }

        p->state = MiniAudioSessionPrivate::State::Playing;
        return true;

    case MiniAudioSessionPrivate::State::Playing:
        if (!isPlaying())
        {
            // not playing, start playing and seek if needed
            if (startTime >= 0 && !seek(startTime))
                return false;

            result = ma_sound_start(&p->sound);
            if (result != MA_SUCCESS)
            {
                GetLogger()->error("Failed to start playing sound file {}", path());
                return false;
            }
            p->state = MiniAudioSessionPrivate::State::Playing;
            return true;
        }
        // already playing, seek if needed
        if (startTime >= 0 && !seek(startTime))
            return false;
        return true;

    default:
        GetLogger()->error("Unknown miniaudio session state {}", static_cast<int>(p->state));
        return false;
    }
}

bool MiniAudioSession::isPlaying() const
{
    if (p->state != MiniAudioSessionPrivate::State::Playing)
        return false;

    if (ma_sound_is_playing(&p->sound) == MA_FALSE)
    {
        // Not actually playing, reset state
        p->state = MiniAudioSessionPrivate::State::SoundInitialzied;
        return false;
    }
    return true;
}

void MiniAudioSession::stop()
{
    if (isPlaying())
    {
        ma_sound_stop(&p->sound);
        p->state = MiniAudioSessionPrivate::State::SoundInitialzied;
    }
}

bool MiniAudioSession::seek(double seconds)
{
    if (p->state >= MiniAudioSessionPrivate::State::SoundInitialzied)
    {
        ma_result result = ma_sound_seek_to_pcm_frame(&p->sound, static_cast<ma_uint64>(seconds * ma_engine_get_sample_rate(&p->backend->engine)));
        if (result != MA_SUCCESS)
        {
            GetLogger()->error("Failed to seek to {}", seconds);
            return false;
        }
    }
    return true;
}

void MiniAudioSession::updateVolume()
{
    if (p->state >= MiniAudioSessionPrivate::State::SoundInitialzied)
        ma_sound_set_volume(&p->sound, volume());
}

void MiniAudioSession::updatePan()
{
    if (p->state >= MiniAudioSessionPrivate::State::SoundInitialzied)
        ma_sound_set_pan(&p->sound, pan());
}

void MiniAudioSession::updateLoop()
{
    if (p->state >= MiniAudioSessionPrivate::State::SoundInitialzied)
        ma_sound_set_looping(&p->sound, loop() ? MA_TRUE : MA_FALSE);
}

bool MiniAudioSession::startEngine()
{
    if (p->state >= MiniAudioSessionPrivate::State::EngineStarted)
        return true;
    p->backend = acquireAudioBackend();
    return p->backend != nullptr;
}

}
