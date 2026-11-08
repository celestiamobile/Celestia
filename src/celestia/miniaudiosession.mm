// miniaudiosession.mm
//
// Copyright (C) 2025, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <TargetConditionals.h>

#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
#define CELESTIA_MANAGE_IOS_AUDIO_SESSION

#import "iosaudiosession.h"
#import <AVFAudio/AVFAudio.h>

#include <mutex>
#include <optional>

namespace
{

struct AudioConfiguration
{
    AVAudioSessionCategory category;
    AVAudioSessionMode mode;
    AVAudioSessionCategoryOptions options;

    bool matches(AVAudioSession *session) const
    {
        return [category isEqualToString:session.category] &&
               [mode isEqualToString:session.mode] && options == session.categoryOptions;
    }
};

class IOSAudioSessionManager
{
public:
    BOOL acquire(bool pictureInPicture, NSError **error)
    {
        std::lock_guard lock(m_mutex);
        auto *session = AVAudioSession.sharedInstance;
        if (m_scriptUsers == 0 && m_pictureInPictureUsers == 0)
        {
            if (m_applied && !m_applied->matches(session))
            {
                m_original.reset();
                m_applied.reset();
                m_ownsActivation = false;
            }
            if (!m_original)
                m_original = AudioConfiguration{session.category, session.mode, session.categoryOptions};
        }

        auto &users = pictureInPicture ? m_pictureInPictureUsers : m_scriptUsers;
        ++users;
        if (update(session, true, error))
            return YES;

        --users;
        NSError *cleanupError = nil;
        if (!update(session, false, &cleanupError))
            NSLog(@"Could not restore audio after activation failure: %@", cleanupError);
        return NO;
    }

    BOOL release(bool pictureInPicture, NSError **error)
    {
        std::lock_guard lock(m_mutex);
        auto &users = pictureInPicture ? m_pictureInPictureUsers : m_scriptUsers;
        if (users == 0)
        {
            if (error != nullptr)
                *error = [NSError errorWithDomain:@"CelestiaAudioSession" code:1
                                        userInfo:@{NSLocalizedDescriptionKey: @"Audio session lease was already released."}];
            return NO;
        }
        --users;
        // Keep the surviving owner's playback running; restore policy only at the last release.
        if (m_scriptUsers != 0 || m_pictureInPictureUsers != 0)
            return YES;
        return update(AVAudioSession.sharedInstance, false, error);
    }

private:
    BOOL update(AVAudioSession *session, bool activate, NSError **error)
    {
        if (m_scriptUsers != 0 || m_pictureInPictureUsers != 0)
        {
            const AudioConfiguration configuration{
                AVAudioSessionCategoryPlayback,
                m_pictureInPictureUsers != 0 ? AVAudioSessionModeMoviePlayback : AVAudioSessionModeDefault,
                m_pictureInPictureUsers != 0 ? AVAudioSessionCategoryOptionMixWithOthers : 0,
            };
            if (!configuration.matches(session) &&
                ![session setCategory:configuration.category mode:configuration.mode
                              options:configuration.options error:error])
                return NO;
            m_applied = configuration;
            if (activate)
            {
                if (![session setActive:YES error:error])
                    return NO;
                m_ownsActivation = true;
            }
            return YES;
        }

        if (!m_original)
            return YES;
        if (m_applied && !m_applied->matches(session))
        {
            // An untracked client changed the session; do not overwrite its policy.
            m_original.reset();
            m_applied.reset();
            m_ownsActivation = false;
            return YES;
        }
        if (m_ownsActivation)
        {
            if (![session setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:error])
                return NO;
            m_ownsActivation = false;
        }
        // Restore a potentially nonmixing category only after our last owner deactivates.
        if (![session setCategory:m_original->category mode:m_original->mode
                          options:m_original->options error:error])
            return NO;
        m_original.reset();
        m_applied.reset();
        return YES;
    }

    std::mutex m_mutex;
    unsigned int m_scriptUsers{0};
    unsigned int m_pictureInPictureUsers{0};
    bool m_ownsActivation{false};
    std::optional<AudioConfiguration> m_original;
    std::optional<AudioConfiguration> m_applied;
};

IOSAudioSessionManager& audioSessionManager()
{
    static IOSAudioSessionManager manager;
    return manager;
}

bool acquireScriptAudioSession()
{
    NSError *error = nil;
    if (audioSessionManager().acquire(false, &error))
        return true;
    NSLog(@"Could not activate script audio: %@", error);
    return false;
}

void releaseScriptAudioSession()
{
    NSError *error = nil;
    if (!audioSessionManager().release(false, &error))
        NSLog(@"Could not restore audio after script playback: %@", error);
}

} // namespace

BOOL CelestiaAcquirePictureInPictureAudioSession(NSError **error)
{
    return audioSessionManager().acquire(true, error);
}

BOOL CelestiaReleasePictureInPictureAudioSession(NSError **error)
{
    return audioSessionManager().release(true, error);
}
#endif

#include "miniaudiosession.cpp"
