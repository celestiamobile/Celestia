// Copyright (C) 2026, the Celestia Development Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "brunetoninput.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <istream>
#include <limits>
#include <string_view>

#include <celcompat/charconv.h>
#include <celutil/associativearray.h>
#include <celutil/parser.h>
#include <celutil/tokenizer.h>

namespace celestia::tools
{

namespace
{

constexpr double Pi = 3.14159265358979323846;

std::string_view
trim(std::string_view value)
{
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' ||
            value.front() == '\r'))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' ||
            value.back() == '\r'))
    {
        value.remove_suffix(1);
    }
    return value;
}

std::vector<std::string_view>
splitCsv(std::string_view line)
{
    std::vector<std::string_view> fields;
    for (;;)
    {
        const auto comma = line.find(',');
        fields.push_back(trim(line.substr(0, comma)));
        if (comma == std::string_view::npos)
            return fields;
        line.remove_prefix(comma + 1);
    }
}

bool
parseNumber(std::string_view text, double& value)
{
    const auto result =
        celestia::compat::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} &&
           result.ptr == text.data() + text.size() &&
           std::isfinite(value);
}

bool
readHeader(std::istream& input,
           std::vector<std::string_view>& fields,
           std::string& storage,
           std::string& error)
{
    if (!std::getline(input, storage))
    {
        error = "CSV input is empty";
        return false;
    }
    fields = splitCsv(storage);
    return true;
}

bool
strictlyIncreasing(const std::vector<double>& values)
{
    return values.size() >= 2 &&
           std::adjacent_find(values.begin(), values.end(),
                              [](double left, double right)
                              {
                                  return !std::isfinite(left) ||
                                         !std::isfinite(right) ||
                                         right <= left;
                              }) == values.end();
}

bool
nonnegative(const std::vector<double>& values)
{
    return std::all_of(values.begin(), values.end(),
                       [](double value)
                       {
                           return std::isfinite(value) && value >= 0.0;
                       });
}

bool
sameAxis(const std::vector<double>& left, const std::vector<double>& right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        const double scale = std::max({ 1.0, std::abs(left[i]), std::abs(right[i]) });
        if (std::abs(left[i] - right[i]) > 1.0e-9 * scale)
            return false;
    }
    return true;
}

bool
uniformAxis(const std::vector<double>& values)
{
    if (values.size() < 2)
        return false;
    const double step = (values.back() - values.front()) / (values.size() - 1);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const double expected = values.front() + step * i;
        if (std::abs(values[i] - expected) >
            1.0e-9 * std::max(1.0, std::abs(expected)))
        {
            return false;
        }
    }
    return true;
}

bool
validateDensity(const TabulatedDensity& table, std::string_view name,
                double atmosphereHeightM, std::string& error)
{
    if (table.altitudesM.size() != table.relativeDensity.size() ||
        !strictlyIncreasing(table.altitudesM) ||
        !nonnegative(table.relativeDensity) ||
        table.altitudesM.front() != 0.0 ||
        table.altitudesM.back() < atmosphereHeightM)
    {
        error = std::string(name) +
                " density must have matching finite, nonnegative samples on "
                "a strictly increasing altitude axis covering 0 through the "
                "atmosphere top";
        return false;
    }
    return true;
}

bool
validateSpectrum(const TabulatedSpectrum& table, std::string_view name,
                 std::string& error)
{
    if (table.wavelengthsNm.size() != table.valuesPerM.size() ||
        !strictlyIncreasing(table.wavelengthsNm) ||
        !nonnegative(table.valuesPerM))
    {
        error = std::string(name) +
                " spectrum must have matching finite, nonnegative samples "
                "on a strictly increasing wavelength axis";
        return false;
    }
    return true;
}

bool
validatePhase(const TabulatedPhase& table, std::string_view name,
              std::string& error)
{
    if (!strictlyIncreasing(table.wavelengthsNm) ||
        !strictlyIncreasing(table.anglesRad) ||
        table.values.size() !=
            table.wavelengthsNm.size() * table.anglesRad.size() ||
        !nonnegative(table.values) ||
        std::abs(table.anglesRad.front()) > 1.0e-12 ||
        std::abs(table.anglesRad.back() - Pi) > 1.0e-12)
    {
        error = std::string(name) +
                " phase table must be finite and nonnegative with rectangular "
                "wavelength-major samples spanning exactly 0 to 180 degrees";
        return false;
    }

    for (std::size_t wavelength = 0;
         wavelength < table.wavelengthsNm.size();
         ++wavelength)
    {
        double integral = 0.0;
        for (std::size_t angle = 1; angle < table.anglesRad.size(); ++angle)
        {
            const double theta0 = table.anglesRad[angle - 1];
            const double theta1 = table.anglesRad[angle];
            const auto offset = wavelength * table.anglesRad.size();
            const double value0 = table.values[offset + angle - 1];
            const double value1 = table.values[offset + angle];
            const double slope = (value1 - value0) / (theta1 - theta0);
            const double intercept = value0 - slope * theta0;
            const auto antiderivative =
                [=](double theta)
                {
                    return -intercept * std::cos(theta) +
                           slope * (-theta * std::cos(theta) +
                                    std::sin(theta));
                };
            integral += antiderivative(theta1) - antiderivative(theta0);
        }
        integral *= 2.0 * Pi;
        if (std::abs(integral - 1.0) > 0.02)
        {
            error = std::string(name) +
                    " phase rows must integrate to one over solid angle";
            return false;
        }
    }
    return true;
}

bool
validateWavelengthCount(const std::vector<double>& wavelengths,
                        std::string& error)
{
    if (wavelengths.size() == 3)
    {
        constexpr double RgbWavelengths[] = { 440.0, 550.0, 680.0 };
        if (std::equal(wavelengths.begin(), wavelengths.end(),
                       std::begin(RgbWavelengths),
                       [](double left, double right)
                       {
                           return std::abs(left - right) <= 1.0e-6;
                       }))
        {
            return true;
        }
    }
    else if (wavelengths.size() == 15)
    {
        constexpr double First = 360.0;
        constexpr double Last = 830.0;
        for (std::size_t i = 0; i < wavelengths.size(); ++i)
        {
            const double expected =
                First + (Last - First) * i / (wavelengths.size() - 1);
            if (std::abs(wavelengths[i] - expected) > 1.0e-5)
                break;
            if (i + 1 == wavelengths.size())
                return true;
        }
    }

    error = "wavelength axis must be 440, 550, 680 nm or the 15-sample "
            "uniform 360 through 830 nm grid";
    return false;
}

double
sampleLinear(const std::vector<double>& axis,
             const std::vector<double>& values,
             double coordinate)
{
    if (axis.size() < 2 || axis.size() != values.size() ||
        !std::isfinite(coordinate))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (coordinate <= axis.front())
        return values.front();
    if (coordinate >= axis.back())
        return values.back();
    const auto upper = std::upper_bound(axis.begin(), axis.end(), coordinate);
    const auto right = static_cast<std::size_t>(upper - axis.begin());
    const auto left = right - 1;
    const double t = (coordinate - axis[left]) / (axis[right] - axis[left]);
    return values[left] * (1.0 - t) + values[right] * t;
}

bool
loadDensity(const std::filesystem::path& path,
            TabulatedDensity& table,
            std::string& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "could not open density table: " + path.string();
        return false;
    }
    if (!ReadTabulatedDensityCsv(input, table, error))
        error = path.string() + ": " + error;
    return error.empty();
}

bool
loadSpectrum(const std::filesystem::path& path,
             std::string_view column,
             TabulatedSpectrum& table,
             std::string& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "could not open spectrum table: " + path.string();
        return false;
    }
    if (!ReadTabulatedSpectrumCsv(input, column, table, error))
        error = path.string() + ": " + error;
    return error.empty();
}

bool
loadPhase(const std::filesystem::path& path,
          TabulatedPhase& table,
          std::string& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "could not open phase table: " + path.string();
        return false;
    }
    if (!ReadTabulatedPhaseCsv(input, table, error))
        error = path.string() + ": " + error;
    return error.empty();
}

std::optional<std::filesystem::path>
requiredPath(const celestia::util::AssociativeArray& values,
             std::string_view key,
             const std::filesystem::path& base,
             std::string& error)
{
    auto path = values.getPath(key);
    if (!path)
    {
        error = "missing path: " + std::string(key);
        return std::nullopt;
    }
    if (path->is_relative())
        *path = base / *path;
    return path;
}

template<std::size_t N>
bool
validateKeys(const celestia::util::AssociativeArray& values,
             const std::array<std::string_view, N>& allowed,
             std::string_view context,
             std::string& error)
{
    if (values.hasDuplicateKeys())
    {
        error = std::string(context) + " contains duplicate properties";
        return false;
    }

    bool valid = true;
    auto check = [&](std::string_view key, const celestia::util::Value&)
    {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
        {
            error = std::string(context) + " contains unknown property: " +
                    std::string(key);
            valid = false;
        }
    };
    values.for_all(check);
    return valid;
}

bool
loadScatteringComponent(const celestia::util::AssociativeArray& manifest,
                        std::string_view key,
                        const std::filesystem::path& base,
                        TabulatedScatteringComponent& component,
                        std::string& error)
{
    const auto* value = manifest.getValue(key);
    const auto* values = value == nullptr ? nullptr : value->getHash();
    if (values == nullptr)
    {
        error = std::string(key) + " must be a hash";
        return false;
    }
    constexpr std::array Allowed{
        std::string_view{ "Density" },
        std::string_view{ "Scattering" },
        std::string_view{ "Absorption" },
        std::string_view{ "Phase" },
    };
    if (!validateKeys(*values, Allowed, key, error))
        return false;

    const auto density = requiredPath(*values, "Density", base, error);
    const auto scattering = requiredPath(*values, "Scattering", base, error);
    const auto absorption = requiredPath(*values, "Absorption", base, error);
    const auto phase = requiredPath(*values, "Phase", base, error);
    return density && scattering && absorption && phase &&
           loadDensity(*density, component.density, error) &&
           loadSpectrum(*scattering, "scattering_per_m",
                        component.scattering, error) &&
           loadSpectrum(*absorption, "absorption_per_m",
                        component.absorption, error) &&
           loadPhase(*phase, component.phase, error);
}

} // namespace

double
TabulatedDensity::sample(double altitudeM) const
{
    return sampleLinear(altitudesM, relativeDensity, altitudeM);
}

double
TabulatedSpectrum::sample(double wavelengthNm) const
{
    return sampleLinear(wavelengthsNm, valuesPerM, wavelengthNm);
}

double
TabulatedPhase::sample(double wavelengthNm, double angleRad) const
{
    if (wavelengthsNm.size() < 2 || anglesRad.size() < 2 ||
        values.size() != wavelengthsNm.size() * anglesRad.size() ||
        !std::isfinite(wavelengthNm) || !std::isfinite(angleRad))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto wavelengthUpper =
        std::upper_bound(wavelengthsNm.begin(), wavelengthsNm.end(), wavelengthNm);
    std::size_t right = static_cast<std::size_t>(
        wavelengthUpper - wavelengthsNm.begin());
    if (right == 0)
        right = 1;
    else if (right >= wavelengthsNm.size())
        right = wavelengthsNm.size() - 1;
    const std::size_t left = right - 1;
    const double wavelengthT =
        std::clamp((wavelengthNm - wavelengthsNm[left]) /
                       (wavelengthsNm[right] - wavelengthsNm[left]),
                   0.0, 1.0);

    const auto rowSize = anglesRad.size();
    const auto angleUpper =
        std::upper_bound(anglesRad.begin(), anglesRad.end(), angleRad);
    std::size_t angleRight =
        static_cast<std::size_t>(angleUpper - anglesRad.begin());
    if (angleRight == 0)
        angleRight = 1;
    else if (angleRight >= anglesRad.size())
        angleRight = anglesRad.size() - 1;
    const std::size_t angleLeft = angleRight - 1;
    const double angleT =
        std::clamp((angleRad - anglesRad[angleLeft]) /
                       (anglesRad[angleRight] - anglesRad[angleLeft]),
                   0.0, 1.0);
    const auto sampleRow =
        [&](std::size_t row)
        {
            const double value0 = values[row * rowSize + angleLeft];
            const double value1 = values[row * rowSize + angleRight];
            return value0 * (1.0 - angleT) + value1 * angleT;
        };
    const double leftValue = sampleRow(left);
    const double rightValue = sampleRow(right);
    return leftValue * (1.0 - wavelengthT) + rightValue * wavelengthT;
}

bool
ReadTabulatedDensityCsv(std::istream& input,
                        TabulatedDensity& table,
                        std::string& error)
{
    error.clear();
    std::string line;
    std::vector<std::string_view> fields;
    if (!readHeader(input, fields, line, error))
        return false;
    if (fields.size() != 2 || fields[0] != "altitude_m" ||
        fields[1] != "relative_density")
    {
        error = "density header must be altitude_m,relative_density";
        return false;
    }

    TabulatedDensity loaded;
    std::size_t lineNumber = 1;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (trim(line).empty())
            continue;
        fields = splitCsv(line);
        double altitude;
        double density;
        if (fields.size() != 2 ||
            !parseNumber(fields[0], altitude) ||
            !parseNumber(fields[1], density))
        {
            error = "invalid density row at line " + std::to_string(lineNumber);
            return false;
        }
        loaded.altitudesM.push_back(altitude);
        loaded.relativeDensity.push_back(density);
    }
    if (loaded.altitudesM.size() < 2)
    {
        error = "density table must contain at least two rows";
        return false;
    }
    table = std::move(loaded);
    return true;
}

bool
ReadTabulatedSpectrumCsv(std::istream& input,
                         std::string_view valueColumn,
                         TabulatedSpectrum& table,
                         std::string& error)
{
    error.clear();
    std::string line;
    std::vector<std::string_view> fields;
    if (!readHeader(input, fields, line, error))
        return false;
    if (fields.size() != 2 || fields[0] != "wavelength_nm" ||
        fields[1] != valueColumn)
    {
        error = "spectrum header must be wavelength_nm," +
                std::string(valueColumn);
        return false;
    }

    TabulatedSpectrum loaded;
    std::size_t lineNumber = 1;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (trim(line).empty())
            continue;
        fields = splitCsv(line);
        double wavelength;
        double value;
        if (fields.size() != 2 ||
            !parseNumber(fields[0], wavelength) ||
            !parseNumber(fields[1], value))
        {
            error = "invalid spectrum row at line " + std::to_string(lineNumber);
            return false;
        }
        loaded.wavelengthsNm.push_back(wavelength);
        loaded.valuesPerM.push_back(value);
    }
    if (loaded.wavelengthsNm.size() < 2)
    {
        error = "spectrum table must contain at least two rows";
        return false;
    }
    table = std::move(loaded);
    return true;
}

bool
ReadTabulatedPhaseCsv(std::istream& input,
                      TabulatedPhase& table,
                      std::string& error)
{
    error.clear();
    std::string line;
    std::vector<std::string_view> fields;
    if (!readHeader(input, fields, line, error))
        return false;
    if (fields.size() < 3 || fields[0] != "wavelength_nm")
    {
        error = "phase header must begin with wavelength_nm and at least two "
                "angle columns";
        return false;
    }

    TabulatedPhase loaded;
    for (std::size_t i = 1; i < fields.size(); ++i)
    {
        double angleDegrees;
        if (!parseNumber(fields[i], angleDegrees))
        {
            error = "invalid phase angle in header";
            return false;
        }
        loaded.anglesRad.push_back(angleDegrees * Pi / 180.0);
    }

    std::size_t lineNumber = 1;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (trim(line).empty())
            continue;
        fields = splitCsv(line);
        double wavelength;
        if (fields.size() != loaded.anglesRad.size() + 1 ||
            !parseNumber(fields[0], wavelength))
        {
            error = "invalid phase row at line " + std::to_string(lineNumber);
            return false;
        }
        loaded.wavelengthsNm.push_back(wavelength);
        for (std::size_t i = 1; i < fields.size(); ++i)
        {
            double value;
            if (!parseNumber(fields[i], value))
            {
                error = "invalid phase value at line " +
                        std::to_string(lineNumber);
                return false;
            }
            loaded.values.push_back(value);
        }
    }
    if (loaded.wavelengthsNm.size() < 2)
    {
        error = "phase table must contain at least two wavelength rows";
        return false;
    }
    table = std::move(loaded);
    return true;
}

bool
ValidateTabulatedAtmosphereInput(const TabulatedAtmosphereInput& input,
                                 std::string& error)
{
    error.clear();
    if (!std::isfinite(input.bottomRadiusM) ||
        !std::isfinite(input.topRadiusM) ||
        input.bottomRadiusM <= 0.0 ||
        input.topRadiusM <= input.bottomRadiusM)
    {
        error = "invalid tabulated atmosphere radii";
        return false;
    }
    const double height = input.topRadiusM - input.bottomRadiusM;

    const auto validateScattering =
        [&](const TabulatedScatteringComponent& component,
            std::string_view name) -> bool
        {
            return validateDensity(component.density, name, height, error) &&
                   validateSpectrum(component.scattering, name, error) &&
                   validateSpectrum(component.absorption, name, error) &&
                   validatePhase(component.phase, name, error) &&
                   sameAxis(component.scattering.wavelengthsNm,
                            component.absorption.wavelengthsNm) &&
                   sameAxis(component.scattering.wavelengthsNm,
                            component.phase.wavelengthsNm);
        };

    if (!validateScattering(input.molecules, "molecular"))
    {
        if (error.empty())
            error = "molecular wavelength axes do not match";
        return false;
    }
    if (std::any_of(input.molecules.phase.values.begin(),
                    input.molecules.phase.values.end(),
                    [](double value) { return value <= 0.0; }))
    {
        error = "molecular phase values must be positive";
        return false;
    }
    if (!validateScattering(input.aerosols, "aerosol"))
    {
        if (error.empty())
            error = "aerosol wavelength axes do not match";
        return false;
    }
    if (!sameAxis(input.molecules.scattering.wavelengthsNm,
                  input.aerosols.scattering.wavelengthsNm))
    {
        error = "molecular and aerosol wavelength axes do not match";
        return false;
    }
    if (!sameAxis(input.molecules.phase.anglesRad,
                  input.aerosols.phase.anglesRad) ||
        !uniformAxis(input.molecules.phase.anglesRad))
    {
        error = "molecular and aerosol phase angles must use the same "
                "uniform 0 to 180 degree grid";
        return false;
    }
    if (input.absorber)
    {
        if (!validateDensity(input.absorber->density, "absorber", height, error) ||
            !validateSpectrum(input.absorber->absorption, "absorber", error))
        {
            return false;
        }
        if (!sameAxis(input.molecules.scattering.wavelengthsNm,
                      input.absorber->absorption.wavelengthsNm))
        {
            error = "absorber wavelength axis does not match scattering components";
            return false;
        }
    }
    return validateWavelengthCount(input.molecules.scattering.wavelengthsNm,
                                   error);
}

bool
LoadTabulatedAtmosphereInput(const std::filesystem::path& manifestPath,
                             TabulatedAtmosphereInput& input,
                             std::string& error)
{
    error.clear();
    std::ifstream manifestFile(manifestPath);
    if (!manifestFile)
    {
        error = "could not open atmosphere input manifest: " +
                manifestPath.string();
        return false;
    }

    celestia::util::Tokenizer tokenizer(manifestFile);
    celestia::util::Parser parser(&tokenizer);
    tokenizer.nextToken();
    if (tokenizer.getNameValue() != "AtmosphereInput")
    {
        error = "AtmosphereInput expected";
        return false;
    }
    auto manifestValue = parser.readValue();
    const auto* manifest = manifestValue.getHash();
    if (manifest == nullptr)
    {
        error = "AtmosphereInput must be followed by a hash";
        return false;
    }
    constexpr std::array RootKeys{
        std::string_view{ "BottomRadiusM" },
        std::string_view{ "TopRadiusM" },
        std::string_view{ "Molecules" },
        std::string_view{ "Aerosols" },
        std::string_view{ "Absorber" },
    };
    if (!validateKeys(*manifest, RootKeys, "AtmosphereInput", error))
        return false;
    if (tokenizer.nextToken() != celestia::util::TokenType::End)
    {
        error = "unexpected content after AtmosphereInput";
        return false;
    }

    TabulatedAtmosphereInput loaded;
    const auto bottomRadius = manifest->getNumber<double>("BottomRadiusM");
    const auto topRadius = manifest->getNumber<double>("TopRadiusM");
    if (!bottomRadius || !topRadius)
    {
        error = "BottomRadiusM and TopRadiusM are required";
        return false;
    }
    loaded.bottomRadiusM = *bottomRadius;
    loaded.topRadiusM = *topRadius;
    const auto base = manifestPath.parent_path();
    if (!loadScatteringComponent(*manifest, "Molecules", base,
                                 loaded.molecules, error) ||
        !loadScatteringComponent(*manifest, "Aerosols", base,
                                 loaded.aerosols, error))
    {
        return false;
    }

    if (const auto* absorberValue = manifest->getValue("Absorber");
        absorberValue != nullptr)
    {
        const auto* absorber = absorberValue->getHash();
        if (absorber == nullptr)
        {
            error = "Absorber must be a hash";
            return false;
        }
        constexpr std::array Allowed{
            std::string_view{ "Density" },
            std::string_view{ "Absorption" },
        };
        if (!validateKeys(*absorber, Allowed, "Absorber", error))
            return false;
        TabulatedAbsorbingComponent component;
        const auto density = requiredPath(*absorber, "Density", base, error);
        const auto absorption = requiredPath(*absorber, "Absorption", base, error);
        if (!density || !absorption ||
            !loadDensity(*density, component.density, error) ||
            !loadSpectrum(*absorption, "absorption_per_m",
                          component.absorption, error))
        {
            return false;
        }
        loaded.absorber = std::move(component);
    }

    if (!ValidateTabulatedAtmosphereInput(loaded, error))
        return false;
    input = std::move(loaded);
    return true;
}

} // namespace celestia::tools
