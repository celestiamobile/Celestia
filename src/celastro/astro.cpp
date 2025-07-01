// astro.cpp
//
// Copyright (C) 2001-2009, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "astro.h"
#include "keplerelements.h"

#include <celcompat/numbers.h>
#include <celmath/geomutil.h>

using namespace std::string_view_literals;

namespace celestia::astro
{

namespace
{

const Eigen::Quaterniond ECLIPTIC_TO_EQUATORIAL_ROTATION = math::XRotation(-J2000Obliquity);
const Eigen::Matrix3d ECLIPTIC_TO_EQUATORIAL_MATRIX = ECLIPTIC_TO_EQUATORIAL_ROTATION.toRotationMatrix();

const Eigen::Quaterniond EQUATORIAL_TO_ECLIPTIC_ROTATION =
    Eigen::Quaterniond(Eigen::AngleAxis<double>(-J2000Obliquity, Eigen::Vector3d::UnitX()));
const Eigen::Matrix3d EQUATORIAL_TO_ECLIPTIC_MATRIX = EQUATORIAL_TO_ECLIPTIC_ROTATION.toRotationMatrix();
const Eigen::Matrix3f EQUATORIAL_TO_ECLIPTIC_MATRIX_F = EQUATORIAL_TO_ECLIPTIC_MATRIX.cast<float>();

// Equatorial to galactic coordinate transformation
// North galactic pole at:
// RA 12h 51m 26.282s (192.85958 deg)
// Dec 27 d 07' 42.01" (27.1283361 deg)
// Zero longitude at position angle 122.932
// (J2000 coordinates)
constexpr double GALACTIC_NODE = 282.85958;
constexpr double GALACTIC_INCLINATION = 90.0 - 27.1283361;
constexpr double GALACTIC_LONGITUDE_AT_NODE = 32.932;

const Eigen::Quaterniond EQUATORIAL_TO_GALACTIC_ROTATION =
    math::ZRotation(math::degToRad(GALACTIC_NODE)) *
    math::XRotation(math::degToRad(GALACTIC_INCLINATION)) *
    math::ZRotation(math::degToRad(-GALACTIC_LONGITUDE_AT_NODE));
const Eigen::Matrix3d EQUATORIAL_TO_GALACTIC_MATRIX = EQUATORIAL_TO_GALACTIC_ROTATION.toRotationMatrix();

} // end unnamed namespace

float
lumToAbsMag(float lum)
{
    return SOLAR_ABSMAG - std::log(lum) * LN_MAG;
}

// Return the apparent magnitude of a star with lum times solar
// luminosity viewed at lyrs light years
float
lumToAppMag(float lum, float lyrs)
{
    return absToAppMag(lumToAbsMag(lum), lyrs);
}

float
absMagToLum(float mag)
{
    return std::exp((SOLAR_ABSMAG - mag) / LN_MAG);
}

float
appMagToLum(float mag, float lyrs)
{
    return absMagToLum(appToAbsMag(mag, lyrs));
}

void
decimalToDegMinSec(double angle, int& degrees, int& minutes, double& seconds)
{
    degrees = static_cast<int>(angle);

    double A = angle - static_cast<double>(degrees);
    double B = A * 60.0;
    minutes = static_cast<int>(B);
    double C = B - static_cast<double>(minutes);
    seconds = C * 60.0;
}

double
degMinSecToDecimal(int degrees, int minutes, double seconds)
{
    return static_cast<double>(degrees) + (seconds / 60.0 + static_cast<double>(minutes)) / 60.0;
}

void
decimalToHourMinSec(double angle, int& hours, int& minutes, double& seconds)
{
    double A = angle / 15.0;
    hours = static_cast<int>(A);
    double B = (A - static_cast<double>(hours)) * 60.0;
    minutes = static_cast<int>(B);
    seconds = (B - (double) minutes) * 60.0;
}

// Convert equatorial coordinates to Cartesian celestial (or ecliptical)
// coordinates.
Eigen::Vector3f
equatorialToCelestialCart(float ra, float dec, float distance)
{
    using celestia::numbers::pi;
    double theta = ra / 24.0 * pi * 2 + pi;
    double phi = (dec / 90.0 - 1.0) * pi / 2;
    double stheta;
    double ctheta;
    math::sincos(theta, stheta, ctheta);
    double sphi;
    double cphi;
    math::sincos(phi, sphi, cphi);
    auto x = static_cast<float>(ctheta * sphi * distance);
    auto y = static_cast<float>(cphi * distance);
    auto z = static_cast<float>(-stheta * sphi * distance);

    return EQUATORIAL_TO_ECLIPTIC_MATRIX_F * Eigen::Vector3f(x, y, z);
}

// Convert equatorial coordinates to Cartesian celestial (or ecliptical)
// coordinates.
Eigen::Vector3d
equatorialToCelestialCart(double ra, double dec, double distance)
{
    using celestia::numbers::pi;
    double theta = ra / 24.0 * pi * 2 + pi;
    double phi = (dec / 90.0 - 1.0) * pi / 2;
    double stheta;
    double ctheta;
    math::sincos(theta, stheta, ctheta);
    double sphi;
    double cphi;
    math::sincos(phi, sphi, cphi);
    double x = ctheta * sphi * distance;
    double y = cphi * distance;
    double z = -stheta * sphi * distance;

    return EQUATORIAL_TO_ECLIPTIC_MATRIX * Eigen::Vector3d(x, y, z);
}

void
anomaly(double meanAnomaly, double eccentricity,
        double& trueAnomaly, double& eccentricAnomaly)
{
    using celestia::numbers::pi;
    constexpr double tol = 1.745e-8;
    int iterations = 20;    // limit while() to maximum of 20 iterations.

    double e = meanAnomaly - 2.0 * pi * static_cast<int>(meanAnomaly / (2.0 * pi));
    double err = 1.0;
    while(std::abs(err) > tol && iterations > 0)
    {
        err = e - eccentricity * std::sin(e) - meanAnomaly;
        double delta = err / (1.0 - eccentricity * std::cos(e));
        e -= delta;
        iterations--;
    }

    trueAnomaly = 2.0 * std::atan(std::sqrt((1.0 + eccentricity) / (1.0 - eccentricity)) * std::tan(0.5 * e));
    eccentricAnomaly = e;
}

/*! Return the angle between the mean ecliptic plane and mean equator at
 *  the specified Julian date.
 */
// TODO: replace this with a better precession model
double
meanEclipticObliquity(double jd)
{
    jd -= 2451545.0;
    double t = jd / 36525;
    double de = (46.815 * t + 0.0006 * t * t - 0.00181 * t * t * t) / 3600;

    return J2000Obliquity - de;
}

/*! Return a quaternion giving the transformation from the J2000 ecliptic
 *  coordinate system to the J2000 Earth equatorial coordinate system.
 */
Eigen::Quaterniond
eclipticToEquatorial()
{
    return ECLIPTIC_TO_EQUATORIAL_ROTATION;
}

/*! Rotate a vector in the J2000 ecliptic coordinate system to
 *  the J2000 Earth equatorial coordinate system.
 */
Eigen::Vector3d
eclipticToEquatorial(const Eigen::Vector3d& v)
{
    return ECLIPTIC_TO_EQUATORIAL_MATRIX.transpose() * v;
}

/*! Return a quaternion giving the transformation from the J2000 Earth
 *  equatorial coordinate system to the galactic coordinate system.
 */
Eigen::Quaterniond
equatorialToGalactic()
{
    return EQUATORIAL_TO_GALACTIC_ROTATION;
}

/*! Rotate a vector int the J2000 Earth equatorial coordinate system to
 *  the galactic coordinate system.
 */
Eigen::Vector3d
equatorialToGalactic(const Eigen::Vector3d& v)
{
    return EQUATORIAL_TO_GALACTIC_MATRIX.transpose() * v;
}

} // end namespace celestia::astro
