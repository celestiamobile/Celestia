// Copyright (C) 2026, Celestia Development Team
// Licensed under the GNU General Public License, version 2 or later.

#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT BOOL CelestiaAcquirePictureInPictureAudioSession(NSError * _Nullable * _Nullable error);
// Consumes the lease even if restoring the audio session fails.
FOUNDATION_EXPORT BOOL CelestiaReleasePictureInPictureAudioSession(NSError * _Nullable * _Nullable error);

NS_ASSUME_NONNULL_END
