// Copyright (C) 2026, the Celestia Development Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace celestia::tools
{

struct TabulatedDensity
{
    std::vector<double> altitudesM;
    std::vector<double> relativeDensity;

    double sample(double altitudeM) const;
};

struct TabulatedSpectrum
{
    std::vector<double> wavelengthsNm;
    std::vector<double> valuesPerM;

    double sample(double wavelengthNm) const;
};

struct TabulatedPhase
{
    std::vector<double> wavelengthsNm;
    std::vector<double> anglesRad;
    std::vector<double> values;

    double sample(double wavelengthNm, double angleRad) const;
};

struct TabulatedScatteringComponent
{
    TabulatedDensity density;
    TabulatedSpectrum scattering;
    TabulatedSpectrum absorption;
    TabulatedPhase phase;
};

struct TabulatedAbsorbingComponent
{
    TabulatedDensity density;
    TabulatedSpectrum absorption;
};

struct TabulatedAtmosphereInput
{
    double bottomRadiusM{ 0.0 };
    double topRadiusM{ 0.0 };
    TabulatedScatteringComponent molecules;
    TabulatedScatteringComponent aerosols;
    std::optional<TabulatedAbsorbingComponent> absorber;
};

bool ReadTabulatedDensityCsv(std::istream&, TabulatedDensity&, std::string& error);
bool ReadTabulatedSpectrumCsv(std::istream&, std::string_view valueColumn,
                              TabulatedSpectrum&, std::string& error);
bool ReadTabulatedPhaseCsv(std::istream&, TabulatedPhase&, std::string& error);

bool ValidateTabulatedAtmosphereInput(const TabulatedAtmosphereInput&,
                                      std::string& error);
bool LoadTabulatedAtmosphereInput(const std::filesystem::path& manifest,
                                  TabulatedAtmosphereInput&,
                                  std::string& error);

} // namespace celestia::tools
