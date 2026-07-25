// Copyright (C) 2026, the Celestia Development Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <celcompat/charconv.h>
#include <celengine/brunetonatmospherefile.h>

#include "brunetonbaker.h"

namespace
{

void
printUsage(std::ostream& output)
{
    output
        << "Usage: brunetonprecompute [options] OUTPUT.atm\n"
        << "Bake the analytic physical-Earth Bruneton atmosphere on the CPU.\n\n"
        << "Options:\n"
        << "  --bottom-radius-km VALUE  LUT sphere bottom radius (default 6378.1366)\n"
        << "  --top-radius-km VALUE     LUT sphere top radius (default 6478.1366)\n"
        << "  --orders VALUE            Scattering orders, 1..16 (default 4)\n"
        << "  --threads VALUE           Worker threads, 0..64; 0 selects hardware (default 0)\n"
        << "  --phase-samples VALUE     Samples per phase-function row (default 1024)\n"
        << "  --no-half                 Do not emulate binary16 intermediate scattering\n"
        << "  -h, --help                Show this help\n";
}

bool
parseInt(std::string_view text, int& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool
parseUnsigned(std::string_view text, std::uint32_t& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool
parseDouble(std::string_view text, double& value)
{
    const auto result =
        celestia::compat::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool
cleanupTemporaryOutput(const std::filesystem::path& file,
                       const std::filesystem::path& directory,
                       std::string& error)
{
    std::error_code removeError;
    std::filesystem::remove(file, removeError);
    if (removeError)
    {
        error = "could not remove temporary output: " + removeError.message();
        return false;
    }

    std::filesystem::remove(directory, removeError);
    if (removeError)
    {
        error = "could not remove temporary output directory: " +
                removeError.message();
        return false;
    }
    return true;
}

bool
createTemporaryOutput(const std::filesystem::path& outputPath,
                      std::filesystem::path& directory,
                      std::filesystem::path& file,
                      std::string& error)
{
    static std::atomic<std::uint64_t> sequence{ 0 };
    const auto parent = outputPath.has_parent_path()
        ? outputPath.parent_path()
        : std::filesystem::path{ "." };

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto timestamp = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
        const auto suffix = std::filesystem::path{
            ".tmp." + std::to_string(timestamp) + "." +
            std::to_string(sequence.fetch_add(1))
        };
        auto candidate = parent / outputPath.filename();
        candidate += suffix;

        std::error_code createError;
        if (std::filesystem::create_directory(candidate, createError))
        {
            directory = candidate;
            file = candidate / outputPath.filename();
            return true;
        }
        if (createError != std::errc::file_exists)
        {
            error = "could not create temporary output directory: " +
                    createError.message();
            return false;
        }
    }

    error = "could not allocate a unique temporary output directory";
    return false;
}

} // namespace

int
main(int argc, char** argv)
{
    celestia::tools::BrunetonBakeSettings settings;
    std::filesystem::path outputPath;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument{ argv[i] };
        if (argument == "-h" || argument == "--help")
        {
            printUsage(std::cout);
            return 0;
        }
        if (argument == "--no-half")
        {
            settings.emulateHalfPrecision = false;
            continue;
        }
        if (argument.size() >= 2 && argument.substr(0, 2) == "--")
        {
            if (++i == argc)
            {
                std::cerr << argument << " requires a value\n";
                return 2;
            }
            const std::string_view value{ argv[i] };
            bool parsed = false;
            if (argument == "--bottom-radius-km")
                parsed = parseDouble(value, settings.bottomRadiusKm);
            else if (argument == "--top-radius-km")
                parsed = parseDouble(value, settings.topRadiusKm);
            else if (argument == "--orders")
                parsed = parseInt(value, settings.scatteringOrders);
            else if (argument == "--threads")
                parsed = parseInt(value, settings.threadCount);
            else if (argument == "--phase-samples")
                parsed = parseUnsigned(value, settings.phaseSampleCount);
            else
            {
                std::cerr << "unknown option: " << argument << '\n';
                return 2;
            }
            if (!parsed)
            {
                std::cerr << "invalid value for " << argument << ": " << value << '\n';
                return 2;
            }
            continue;
        }
        if (!outputPath.empty())
        {
            std::cerr << "only one output path may be specified\n";
            return 2;
        }
        outputPath = argument;
    }

    if (outputPath.empty())
    {
        printUsage(std::cerr);
        return 2;
    }
    std::error_code existsError;
    const bool outputExists = std::filesystem::exists(outputPath, existsError);
    if (existsError)
    {
        std::cerr << "could not inspect output path: "
                  << existsError.message() << '\n';
        return 1;
    }
    if (outputExists)
    {
        std::cerr << "output file already exists: " << outputPath << '\n';
        return 1;
    }

    std::string error;
    if (!celestia::tools::ValidateBrunetonBakeSettings(settings, error))
    {
        std::cerr << error << '\n';
        return 2;
    }

    std::cout << "Baking physical Earth: radius " << settings.bottomRadiusKm
              << ".." << settings.topRadiusKm << " km, "
              << settings.scatteringOrders << " scattering orders, "
              << settings.phaseSampleCount << " phase samples\n";
    const auto start = std::chrono::steady_clock::now();
    celestia::engine::BrunetonAtmosphereData data;
    if (!celestia::tools::BakePhysicalEarthAtmosphere(settings, data, error))
    {
        std::cerr << "precomputation failed: " << error << '\n';
        return 1;
    }

    std::filesystem::path temporaryDirectory;
    std::filesystem::path temporaryPath;
    if (!createTemporaryOutput(outputPath, temporaryDirectory, temporaryPath, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    {
        std::ofstream output(temporaryPath, std::ios::binary);
        if (!output)
        {
            std::cerr << "could not create temporary output: " << temporaryPath << '\n';
            std::string cleanupError;
            if (!cleanupTemporaryOutput(temporaryPath, temporaryDirectory, cleanupError))
                std::cerr << cleanupError << '\n';
            return 1;
        }
        if (!celestia::engine::SaveBrunetonAtmosphere(output, data, error))
        {
            std::cerr << "could not encode atmosphere: " << error << '\n';
            output.close();
            std::string cleanupError;
            if (!cleanupTemporaryOutput(temporaryPath, temporaryDirectory, cleanupError))
                std::cerr << cleanupError << '\n';
            return 1;
        }
        output.flush();
        if (!output)
        {
            std::cerr << "could not flush temporary output\n";
            output.close();
            std::string cleanupError;
            if (!cleanupTemporaryOutput(temporaryPath, temporaryDirectory, cleanupError))
                std::cerr << cleanupError << '\n';
            return 1;
        }
        output.close();
        if (output.fail())
        {
            std::cerr << "could not close temporary output\n";
            std::string cleanupError;
            if (!cleanupTemporaryOutput(temporaryPath, temporaryDirectory, cleanupError))
                std::cerr << cleanupError << '\n';
            return 1;
        }
    }

    std::error_code publishError;
    std::filesystem::create_hard_link(temporaryPath, outputPath, publishError);
    if (publishError)
    {
        std::string cleanupError;
        if (!cleanupTemporaryOutput(temporaryPath, temporaryDirectory, cleanupError))
            std::cerr << cleanupError << '\n';
        std::cerr << "could not publish output without replacing an existing file: "
                  << publishError.message() << '\n';
        return 1;
    }
    std::string cleanupError;
    if (!cleanupTemporaryOutput(temporaryPath, temporaryDirectory, cleanupError))
    {
        std::cerr << "output was published, but cleanup failed: "
                  << cleanupError << '\n';
        return 1;
    }

    std::error_code sizeError;
    const auto outputSize = std::filesystem::file_size(outputPath, sizeError);
    if (sizeError)
    {
        std::cerr << "output was written, but its size could not be read: "
                  << sizeError.message() << '\n';
        return 1;
    }

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start);
    std::cout << "Wrote " << outputPath << " (" << outputSize
              << " bytes) in " << elapsed.count() << " seconds\n";
    return 0;
}
