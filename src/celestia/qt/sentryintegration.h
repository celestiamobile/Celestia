// sentryintegration.h
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// Optional Sentry (sentry-native) crash reporting for the Qt6 front-end. When
// the build is configured with -DENABLE_SENTRY=ON, initializeSentry() starts
// the crashpad backend and shutdownSentry() flushes pending events. When the
// build is configured without USE_SENTRY, every function is a no-op and the
// Sentry SDK is not linked.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

namespace celestia::qt
{

// Initialises Sentry as early as possible in startup. Returns true if the SDK
// was initialised, false if it was disabled (no DSN baked in) or failed to
// start. Safe to call in builds without USE_SENTRY (returns false).
bool initializeSentry();

// Flushes and shuts down Sentry. Must run before the process exits, otherwise
// buffered events may be lost. Safe to call when initializeSentry() returned
// false or in builds without USE_SENTRY.
void shutdownSentry();

} // namespace celestia::qt
