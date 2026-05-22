// steamintegration.cpp
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "steamintegration.h"

#ifdef USE_STEAM
#include <algorithm>
#include <optional>
#include <system_error>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include <steam/steam_api.h>
#endif

namespace celestia::qt
{

#ifdef USE_STEAM
namespace
{
bool g_steamInitialized = false;

// Validate a Workshop item's install path and return the path of the
// content directory if the layout matches `wantType`. A valid Workshop
// item must contain a description.json at its root with:
//   * "id"   — a non-empty string, also the name of a sibling directory
//   * "type" — equal to wantType (currently "addon" or "script")
// Items whose description.json is missing, malformed, or whose type
// doesn't match are skipped.
std::optional<std::filesystem::path>
extractTypedDir(const std::filesystem::path& installPath, QStringView wantType)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(installPath, ec) || ec)
        return std::nullopt;

    const auto descPath = installPath / "description.json";
    if (!std::filesystem::is_regular_file(descPath, ec) || ec)
        return std::nullopt;

    QFile descFile(QString::fromStdString(descPath.string()));
    if (!descFile.open(QIODevice::ReadOnly))
        return std::nullopt;

    const QByteArray bytes = descFile.readAll();
    descFile.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    const QJsonObject obj = doc.object();
    const QJsonValue idValue = obj.value(QStringLiteral("id"));
    const QJsonValue typeValue = obj.value(QStringLiteral("type"));
    if (!idValue.isString() || !typeValue.isString())
        return std::nullopt;

    if (typeValue.toString() != wantType)
        return std::nullopt;

    const QString id = idValue.toString();
    if (id.isEmpty())
        return std::nullopt;

    std::filesystem::path candidate = installPath / id.toStdString();
    if (!std::filesystem::is_directory(candidate, ec) || ec)
        return std::nullopt;

    return candidate;
}

// Walk the user's subscribed Workshop items and return the local content
// directories whose description.json declares the given `wantType`,
// sorted ascending by ID for deterministic order.
std::vector<std::filesystem::path>
collectWorkshopDirs(QStringView wantType)
{
    std::vector<std::filesystem::path> dirs;
    if (!g_steamInitialized)
        return dirs;

    auto* ugc = SteamUGC();
    if (ugc == nullptr)
        return dirs;

    const uint32 count = ugc->GetNumSubscribedItems();
    if (count == 0)
        return dirs;

    std::vector<PublishedFileId_t> items(count);
    const uint32 fetched = ugc->GetSubscribedItems(items.data(), count);

    char installPath[1024];
    for (uint32 i = 0; i < fetched; ++i)
    {
        uint64 sizeOnDisk = 0;
        uint32 timestamp = 0;
        if (!ugc->GetItemInstallInfo(items[i],
                                     &sizeOnDisk,
                                     installPath,
                                     sizeof(installPath),
                                     &timestamp))
        {
            continue;
        }

        if (auto contentDir = extractTypedDir(std::filesystem::path{installPath}, wantType);
            contentDir.has_value())
        {
            dirs.push_back(std::move(*contentDir));
        }
    }

    std::sort(dirs.begin(), dirs.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b)
              {
                  return a.filename() < b.filename();
              });
    return dirs;
}
} // anonymous namespace
#endif

bool
initializeSteam()
{
#ifdef USE_STEAM
    g_steamInitialized = SteamAPI_Init();
    return g_steamInitialized;
#else
    return false;
#endif
}

void
shutdownSteam()
{
#ifdef USE_STEAM
    if (g_steamInitialized)
    {
        SteamAPI_Shutdown();
        g_steamInitialized = false;
    }
#endif
}

std::vector<std::filesystem::path>
getSteamWorkshopAddonDirs()
{
#ifdef USE_STEAM
    return collectWorkshopDirs(QStringLiteral("addon"));
#else
    return {};
#endif
}

std::vector<std::filesystem::path>
getSteamWorkshopScriptDirs()
{
#ifdef USE_STEAM
    return collectWorkshopDirs(QStringLiteral("script"));
#else
    return {};
#endif
}

} // namespace celestia::qt
