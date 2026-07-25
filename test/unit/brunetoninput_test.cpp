// Copyright (C) 2026, the Celestia Development Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <limits>
#include <sstream>
#include <string>

#include <doctest.h>

#include "../../src/tools/atmosphere/brunetoninput.h"

using celestia::tools::ReadTabulatedDensityCsv;
using celestia::tools::ReadTabulatedPhaseCsv;
using celestia::tools::ReadTabulatedSpectrumCsv;
using celestia::tools::TabulatedAtmosphereInput;
using celestia::tools::TabulatedDensity;
using celestia::tools::TabulatedPhase;
using celestia::tools::TabulatedScatteringComponent;
using celestia::tools::TabulatedSpectrum;
using celestia::tools::ValidateTabulatedAtmosphereInput;

namespace
{

constexpr double Pi = 3.14159265358979323846;

TabulatedScatteringComponent
makeIsotropicComponent()
{
    TabulatedScatteringComponent component;
    component.density.altitudesM = { 0.0, 100000.0 };
    component.density.relativeDensity = { 1.0, 0.0 };
    component.scattering.wavelengthsNm = { 440.0, 550.0, 680.0 };
    component.scattering.valuesPerM = { 1.0e-6, 2.0e-6, 3.0e-6 };
    component.absorption.wavelengthsNm =
        component.scattering.wavelengthsNm;
    component.absorption.valuesPerM = { 0.0, 0.0, 0.0 };
    component.phase.wavelengthsNm = component.scattering.wavelengthsNm;
    for (int i = 0; i <= 180; ++i)
        component.phase.anglesRad.push_back(i * Pi / 180.0);
    component.phase.values.assign(
        component.phase.wavelengthsNm.size() *
            component.phase.anglesRad.size(),
        1.0 / (4.0 * Pi));
    return component;
}

} // namespace

TEST_CASE("Tabulated atmosphere CSV readers require explicit units")
{
    std::string error;

    std::istringstream densityCsv{
        "altitude_m,relative_density\n"
        "0,1\n"
        "100000,0\n"
    };
    TabulatedDensity density;
    CHECK(ReadTabulatedDensityCsv(densityCsv, density, error));
    CHECK(density.sample(50000.0) == doctest::Approx(0.5));

    std::istringstream spectrumCsv{
        "wavelength_nm,scattering_per_m\n"
        "440,1e-6\n"
        "550,2e-6\n"
        "680,3e-6\n"
    };
    TabulatedSpectrum spectrum;
    CHECK(ReadTabulatedSpectrumCsv(
        spectrumCsv, "scattering_per_m", spectrum, error));
    CHECK(spectrum.sample(495.0) == doctest::Approx(1.5e-6));

    std::istringstream invalidCsv{
        "lambda,beta_sca\n"
        "4.4e-7,1e-6\n"
    };
    CHECK_FALSE(ReadTabulatedSpectrumCsv(
        invalidCsv, "scattering_per_m", spectrum, error));
    CHECK(error ==
          "spectrum header must be wavelength_nm,scattering_per_m");

    std::istringstream emptyDensityCsv{
        "altitude_m,relative_density\n"
    };
    CHECK_FALSE(ReadTabulatedDensityCsv(emptyDensityCsv, density, error));
    CHECK(error == "density table must contain at least two rows");
    CHECK(std::isnan(density.sample(
        std::numeric_limits<double>::quiet_NaN())));
}

TEST_CASE("Tabulated phase CSV preserves wavelength and angle axes")
{
    std::istringstream phaseCsv{
        "wavelength_nm,0,90,180\n"
        "440,0.1,0.2,0.3\n"
        "680,0.2,0.3,0.4\n"
    };
    TabulatedPhase phase;
    std::string error;
    REQUIRE(ReadTabulatedPhaseCsv(phaseCsv, phase, error));
    REQUIRE(phase.wavelengthsNm.size() == 2);
    REQUIRE(phase.anglesRad.size() == 3);
    CHECK(phase.sample(560.0, Pi / 4.0) == doctest::Approx(0.2));
    CHECK(std::isnan(phase.sample(
        std::numeric_limits<double>::quiet_NaN(), Pi / 4.0)));
}

TEST_CASE("Tabulated atmosphere validates spectral and spatial coverage")
{
    TabulatedAtmosphereInput input;
    input.bottomRadiusM = 6378136.6;
    input.topRadiusM = 6478136.6;
    input.molecules = makeIsotropicComponent();
    input.aerosols = makeIsotropicComponent();

    std::string error;
    CHECK(ValidateTabulatedAtmosphereInput(input, error));

    input.aerosols.phase.wavelengthsNm[1] = 551.0;
    CHECK_FALSE(ValidateTabulatedAtmosphereInput(input, error));
    CHECK(error == "aerosol wavelength axes do not match");

    input.aerosols = makeIsotropicComponent();
    input.aerosols.density.altitudesM.back() = 99999.0;
    CHECK_FALSE(ValidateTabulatedAtmosphereInput(input, error));
    CHECK(error.find("covering 0 through the atmosphere top") !=
          std::string::npos);

    input = {};
    input.bottomRadiusM = 6378136.6;
    input.topRadiusM = 6478136.6;
    input.molecules = makeIsotropicComponent();
    input.aerosols = makeIsotropicComponent();
    const auto rowSize = input.aerosols.phase.anglesRad.size();
    input.aerosols.phase.values.front() = 1000.0;
    input.aerosols.phase.values[rowSize] = 1000.0;
    input.aerosols.phase.values[rowSize * 2] = 1000.0;
    CHECK_FALSE(ValidateTabulatedAtmosphereInput(input, error));
    CHECK(error == "aerosol phase rows must integrate to one over solid angle");
}
