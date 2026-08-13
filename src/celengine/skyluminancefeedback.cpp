#include "skyluminancefeedback.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <celengine/framebuffer.h>
#include <celengine/glsupport.h>
#include <celengine/observer.h>
#include <celutil/logger.h>

namespace celestia::engine
{
namespace
{

constexpr std::size_t SampleGridSize = 4;
constexpr std::size_t SampleCount = SampleGridSize * SampleGridSize;
constexpr std::size_t PixelSize = 4;
constexpr std::size_t BufferSize = SampleCount * PixelSize;
constexpr std::size_t ReadbackCount = 3;
constexpr double CaptureInterval = 1.0;

float
toLinear(float value)
{
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float
sampleLuminance(const std::uint8_t* pixels, bool linearSource)
{
    std::array<float, SampleCount> luminances;
    for (std::size_t i = 0; i < SampleCount; ++i)
    {
        const std::uint8_t* pixel = pixels + i * PixelSize;
        float red = static_cast<float>(pixel[0]) / 255.0f;
        float green = static_cast<float>(pixel[1]) / 255.0f;
        float blue = static_cast<float>(pixel[2]) / 255.0f;
        if (!linearSource)
        {
            red = toLinear(red);
            green = toLinear(green);
            blue = toLinear(blue);
        }
        luminances[i] =
            0.2126f * red + 0.7152f * green + 0.0722f * blue;
    }

    constexpr std::size_t percentile = SampleCount * 3 / 4;
    std::nth_element(luminances.begin(),
                     luminances.begin() + percentile,
                     luminances.end());
    return luminances[percentile];
}

} // end unnamed namespace

struct SkyLuminanceFeedback::Impl
{
    struct Readback
    {
        GLuint pbo{ 0 };
        GLsync fence{ nullptr };
        std::uint64_t observerId{ 0 };
        std::uint64_t generation{ 0 };
        bool linearSource{ false };
    };

    struct State
    {
        float luminance{ 0.0f };
        double lastCaptureTime{ -CaptureInterval };
        std::uint64_t generation{ 0 };
        bool insideAtmosphere{ false };
        bool valid{ false };
    };

    bool initialize()
    {
        if (m_readbacks.front().pbo != 0)
            return m_sampleFbo != nullptr;

        m_sampleFbo = std::make_unique<FramebufferObject>(
            SampleGridSize,
            SampleGridSize,
            FramebufferObject::Attachment::Color);
        if (!m_sampleFbo->isValid())
        {
            celestia::util::GetLogger()->error(
                "Unable to create sky luminance framebuffer.\n");
            m_sampleFbo = nullptr;
            return false;
        }

        std::array<GLuint, ReadbackCount> pbos;
        glGenBuffers(static_cast<GLsizei>(pbos.size()), pbos.data());

        GLint previousPbo;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPbo);
        for (std::size_t i = 0; i < m_readbacks.size(); ++i)
        {
            m_readbacks[i].pbo = pbos[i];
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, BufferSize, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPbo));
        return true;
    }

    ~Impl()
    {
        if (m_readbacks.front().pbo == 0)
            return;

        std::array<GLuint, ReadbackCount> pbos;
        for (std::size_t i = 0; i < m_readbacks.size(); ++i)
        {
            if (m_readbacks[i].fence != nullptr)
                glDeleteSync(m_readbacks[i].fence);
            pbos[i] = m_readbacks[i].pbo;
        }
        glDeleteBuffers(static_cast<GLsizei>(pbos.size()), pbos.data());
    }

    void consume()
    {
        if (m_readbacks.front().pbo == 0)
            return;
        if (std::none_of(m_readbacks.begin(), m_readbacks.end(),
                         [](const Readback& readback)
                         {
                             return readback.fence != nullptr;
                         }))
        {
            return;
        }

        GLint previousPbo;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPbo);

        for (Readback& readback : m_readbacks)
        {
            if (readback.fence == nullptr)
                continue;

            GLenum result = glClientWaitSync(readback.fence, 0, 0);
            if (result == GL_WAIT_FAILED)
            {
                glDeleteSync(readback.fence);
                readback.fence = nullptr;
                celestia::util::GetLogger()->error(
                    "Unable to wait for sky luminance readback.\n");
                continue;
            }
            if (result != GL_ALREADY_SIGNALED &&
                result != GL_CONDITION_SATISFIED)
                continue;

            glDeleteSync(readback.fence);
            readback.fence = nullptr;

            glBindBuffer(GL_PIXEL_PACK_BUFFER, readback.pbo);
            const auto* pixels = static_cast<const std::uint8_t*>(
                glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, BufferSize,
                                 GL_MAP_READ_BIT));
            if (pixels != nullptr)
            {
                float value =
                    sampleLuminance(pixels, readback.linearSource);
                State& state = m_states[readback.observerId];
                if (readback.generation == state.generation)
                {
                    state.luminance = state.valid
                        ? state.luminance +
                            0.2f * (value - state.luminance)
                        : value;
                    state.valid = true;
                }
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            else
            {
                celestia::util::GetLogger()->error(
                    "Unable to map sky luminance readback.\n");
            }
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPbo));
    }

    void capture(const Observer& observer,
                 const std::array<int, 4>& viewport,
                 bool insideAtmosphere,
                 bool linearSource)
    {
        State& state = m_states[observer.getInstanceId()];
        if (!insideAtmosphere)
        {
            if (state.insideAtmosphere)
                ++state.generation;
            state.lastCaptureTime = -CaptureInterval;
            state.insideAtmosphere = false;
            state.valid = false;
            return;
        }
        if (!state.insideAtmosphere)
        {
            state.lastCaptureTime = -CaptureInterval;
            state.insideAtmosphere = true;
        }
        double captureTime = observer.getRealTime();
        if (captureTime >= state.lastCaptureTime &&
            captureTime - state.lastCaptureTime < CaptureInterval)
            return;
        if (viewport[2] <= 0 || viewport[3] <= 0)
            return;

        GLint readFramebuffer;
        GLint drawFramebuffer;
        GLint samples;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_SAMPLES, &samples);
        if (samples > 1)
            return;
        if (!initialize())
            return;

        Readback* readback = nullptr;
        for (std::size_t i = 0; i < m_readbacks.size(); ++i)
        {
            Readback& candidate =
                m_readbacks[(m_nextReadback + i) % m_readbacks.size()];
            if (candidate.fence == nullptr)
            {
                readback = &candidate;
                m_nextReadback =
                    (m_nextReadback + i + 1) % m_readbacks.size();
                break;
            }
        }
        if (readback == nullptr)
            return;

        GLint previousPbo;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPbo);

        GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        if (scissorEnabled)
            glDisable(GL_SCISSOR_TEST);

        m_sampleFbo->bind();
        GLint sampleFramebuffer;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &sampleFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(readFramebuffer));
        if (readFramebuffer != 0)
            glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          static_cast<GLuint>(sampleFramebuffer));
        glBlitFramebuffer(
            viewport[0],
            viewport[1],
            viewport[0] + viewport[2],
            viewport[1] + viewport[3],
            0,
            0,
            SampleGridSize,
            SampleGridSize,
            GL_COLOR_BUFFER_BIT,
            GL_LINEAR);

        glBindFramebuffer(GL_FRAMEBUFFER,
                          static_cast<GLuint>(sampleFramebuffer));
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, readback->pbo);
        glReadPixels(0, 0, SampleGridSize, SampleGridSize,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        readback->observerId = observer.getInstanceId();
        readback->generation = state.generation;
        readback->linearSource = linearSource;
        readback->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (readback->fence == nullptr)
        {
            celestia::util::GetLogger()->error(
                "Unable to create sky luminance readback fence.\n");
        }
        else
        {
            state.lastCaptureTime = captureTime;
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPbo));
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(readFramebuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          static_cast<GLuint>(drawFramebuffer));
        if (scissorEnabled)
            glEnable(GL_SCISSOR_TEST);
    }

    std::optional<float> luminance(const Observer& observer) const
    {
        auto it = m_states.find(observer.getInstanceId());
        if (it == m_states.end() || !it->second.valid)
            return std::nullopt;
        return it->second.luminance;
    }

    std::array<Readback, ReadbackCount> m_readbacks;
    std::unique_ptr<FramebufferObject> m_sampleFbo;
    std::size_t m_nextReadback{ 0 };
    std::unordered_map<std::uint64_t, State> m_states;
};

SkyLuminanceFeedback::SkyLuminanceFeedback() :
    m_impl(std::make_unique<Impl>())
{
}

SkyLuminanceFeedback::~SkyLuminanceFeedback() = default;

void
SkyLuminanceFeedback::consume()
{
    m_impl->consume();
}

void
SkyLuminanceFeedback::capture(const Observer& observer,
                              const std::array<int, 4>& viewport,
                              bool insideAtmosphere,
                              bool linearSource)
{
    m_impl->capture(observer, viewport, insideAtmosphere,
                    linearSource);
}

std::optional<float>
SkyLuminanceFeedback::luminance(const Observer& observer) const
{
    return m_impl->luminance(observer);
}

} // namespace celestia::engine
