// atmfile.cpp
//
// Copyright (C) 2026, the Celestia Development Team
//
// Loader for the .atm precomputed-atmosphere format declared in
// atmfile.h. Pure CPU; no GL state is touched here.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "atmfile.h"

#include <cstring>
#include <fstream>

#include <celutil/logger.h>

namespace celestia::engine
{

namespace
{

using celestia::util::GetLogger;

bool
readExact(std::ifstream& in, void* dst, std::size_t bytes)
{
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(bytes));
    return in.good() && static_cast<std::size_t>(in.gcount()) == bytes;
}

bool
readBlob(std::ifstream& in, std::vector<float>& dst, std::uint64_t bytes,
         const std::filesystem::path& path, const char* what)
{
    if (bytes % sizeof(float) != 0)
    {
        GetLogger()->error("{}: {} byte count {} is not a multiple of 4\n",
                           path.string(), what, bytes);
        return false;
    }
    dst.resize(bytes / sizeof(float));
    if (!readExact(in, dst.data(), bytes))
    {
        GetLogger()->error("{}: short read for {} LUT (expected {} bytes)\n",
                           path.string(), what, bytes);
        return false;
    }
    return true;
}

bool
validateHeader(const AtmFileHeader& h, const std::filesystem::path& path)
{
    if (h.magic != AtmFileMagic)
    {
        GetLogger()->error("{}: bad magic 0x{:08x} (expected 0x{:08x})\n",
                           path.string(), h.magic, AtmFileMagic);
        return false;
    }
    if (h.version != AtmFileVersion)
    {
        GetLogger()->error("{}: unsupported version {} (this build expects {})\n",
                           path.string(), h.version, AtmFileVersion);
        return false;
    }

    const struct
    {
        const char*   name;
        std::uint32_t got;
        std::uint32_t want;
    } dims[] = {
        { "transmittance_width",   h.transmittance_width,   AtmTransmittanceTextureWidth   },
        { "transmittance_height",  h.transmittance_height,  AtmTransmittanceTextureHeight  },
        { "scattering_r_size",     h.scattering_r_size,     AtmScatteringTextureRSize      },
        { "scattering_mu_size",    h.scattering_mu_size,    AtmScatteringTextureMuSize     },
        { "scattering_mu_s_size",  h.scattering_mu_s_size,  AtmScatteringTextureMuSSize    },
        { "scattering_nu_size",    h.scattering_nu_size,    AtmScatteringTextureNuSize     },
        { "irradiance_width",      h.irradiance_width,      AtmIrradianceTextureWidth      },
        { "irradiance_height",     h.irradiance_height,     AtmIrradianceTextureHeight     },
    };
    for (const auto& d : dims)
    {
        if (d.got != d.want)
        {
            GetLogger()->error("{}: {} mismatch (file has {}, shader expects {})\n",
                               path.string(), d.name, d.got, d.want);
            return false;
        }
    }
    return true;
}

} // namespace

bool
LoadAtmFile(const std::filesystem::path& path, AtmFileData& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        GetLogger()->error("Failed to open atmosphere LUT file {}\n", path.string());
        return false;
    }

    if (!readExact(in, &out.header, sizeof(AtmFileHeader)))
    {
        GetLogger()->error("{}: short read for AtmFileHeader\n", path.string());
        return false;
    }
    if (!validateHeader(out.header, path))
        return false;

    if (!readBlob(in, out.transmittance, AtmTransmittanceBytes, path, "transmittance"))
        return false;
    if (!readBlob(in, out.scattering, AtmScatteringBytes, path, "scattering"))
        return false;
    if (out.header.combined_scattering == 0)
    {
        if (!readBlob(in, out.single_mie_scattering, AtmScatteringBytes, path,
                      "single_mie_scattering"))
            return false;
    }
    else
    {
        out.single_mie_scattering.clear();
    }
    if (!readBlob(in, out.irradiance, AtmIrradianceBytes, path, "irradiance"))
        return false;

    // Tolerate trailing bytes silently (forward-compat with future
    // appended sections) but warn if the file is clearly broken.
    if (in.peek() != EOF)
    {
        GetLogger()->warn("{}: trailing bytes after irradiance LUT (ignored)\n",
                          path.string());
    }
    return true;
}

} // namespace celestia::engine
