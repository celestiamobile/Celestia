// sentryintegration.cpp
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "sentryintegration.h"

#ifdef USE_SENTRY
#include <filesystem>
#include <system_error>

#include <sentry.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
// Directory containing the running executable. The crashpad handler and the
// crash database are placed here so they are found regardless of the working
// directory Steam launches us from.
std::filesystem::path
executableDirectory()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
                                       static_cast<DWORD>(buffer.size()));
        if (len == 0)
            return std::filesystem::current_path();
        if (len < buffer.size())
        {
            buffer.resize(len);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code ec;
    if (auto exe = std::filesystem::read_symlink("/proc/self/exe", ec); !ec)
        return exe.parent_path();
    return std::filesystem::current_path();
#endif
}
} // end unnamed namespace
#endif

namespace celestia::qt
{

bool
initializeSentry()
{
#ifdef USE_SENTRY
    constexpr const char* dsn = CELESTIA_SENTRY_DSN;
    if (dsn == nullptr || dsn[0] == '\0')
        return false;

    const std::filesystem::path baseDir = executableDirectory();

    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, dsn);
    sentry_options_set_release(options, CELESTIA_SENTRY_RELEASE);
    sentry_options_set_debug(options, 0);

#ifdef _WIN32
    const std::filesystem::path dbPath = baseDir / L".sentry-native";
    const std::filesystem::path handlerPath = baseDir / L"crashpad_handler.exe";
    sentry_options_set_database_pathw(options, dbPath.wstring().c_str());
    sentry_options_set_handler_pathw(options, handlerPath.wstring().c_str());
#else
    const std::filesystem::path dbPath = baseDir / ".sentry-native";
    const std::filesystem::path handlerPath = baseDir / "crashpad_handler";
    sentry_options_set_database_path(options, dbPath.string().c_str());
    sentry_options_set_handler_path(options, handlerPath.string().c_str());
#endif

    return sentry_init(options) == 0;
#else
    return false;
#endif
}

void
shutdownSentry()
{
#ifdef USE_SENTRY
    sentry_close();
#endif
}

} // namespace celestia::qt
