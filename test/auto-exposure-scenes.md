# Automatic-exposure regression captures

`auto-exposure-scenes.json` records the scene URLs, rendering settings, Sun-distance
matrix and adaptation-history sequences. Run the entire matrix in both
`PointSpreadFunction` and `FuzzyPointStars` modes.

## Captures

- Capture all 11 named views, with simulation time frozen at each URL timestamp.
- Capture both recorded Earth/Sun/starfield history sequences without resetting
  adaptation between views.
- Center the Sun along the normalized recorded direction in the Universal frame.
  Capture all 25 recorded distances with bloom strength 0.2, followed by the reverse
  sequence. Clear the selection and stop observer motion.
- Repeat the 1.6 and 0.8 AU Sun views with automatic exposure disabled and fixed
  exposure `3.40129951e-8`. These controls distinguish exposure changes from the
  bloom kernel's screen-space response.

Use the reference 2048x1536 framebuffer, 192 DPI and 4x MSAA. Allow 12 seconds per
view, including transitions, and check exposure drift over the last two seconds.
Save the final composed sRGB image as lossless PNG without exposure normalization;
record scene HDR and bloom luminance separately from the screenshot.

Keep the screenshots in separate `psf` and `fuzzy` directories, with logs, the
actual captured URLs, exposure history, final exposure, settled drift, and bloom
measurements. The complete matrix contains 48 captures per style: 11 scenes,
6 history views, 25 Sun distances, 4 reverse distances and 2 fixed-exposure controls.

## Checks

The JSON records each named scene's purpose. In particular, check that daylight
sky and Earth retain detail, night and Sun-free starfields recover, and a visible
Sun near 1 AU suppresses faint stars. Compare both history directions to distinguish
settled behavior from adaptation lag.

For centered-Sun views, verify geometry, finite luminance and stable exposure.
Check the former 1.6-to-0.8 AU bloom inversion and the 0.095-to-0.094 AU percentile
boundary. Do not require constant brightness at offsets measured in solar radii:
the bloom kernel has a fixed screen-space scale. Exposure fixes intentionally
change affected images; a regression comparison is not a requirement for
pixel-identical output.

## Production-shader GPU tests

### Authored surface emission

SSC bodies and alternate surfaces accept `NightLuminance` and
`EmissiveLuminance`, finite nonnegative values in cd/m^2. These specify the
reference luminance of a **linear white** texel/material; RGB colors modulate
that reference, rather than being normalized to constant luminance.
Zero disables the corresponding emission.

In sRGB rendering, the reference defaults are 1 cd/m^2 for night maps and
100 cd/m^2 for emissive surfaces and CMOD materials. These are authoring
defaults, **not measured luminances for Earth or arbitrary add-ons**. Override
them when calibrated asset measurements are available:

```text
Modify "Earth" "Sol"
{
    NightLuminance 1
}
```

`EmissiveLuminance` scales both constant CMOD emissive colors and emissive maps,
independently of diffuse color, albedo, incident illumination and the
display-only surface dimming. With `Emissive true`, it also scales the body's
unlit diffuse surface. Generic unlit nebula models use the same default
reference; this does not normalize them to their catalog magnitudes.
Stellar surfaces keep their magnitude-derived normalization.
Emissive bodies' point representations use the same reference-disc luminance,
not reflected sunlight, and crossfade with the resolved surface. Their points
preserve authored linear RGB rather than applying stellar green-channel
normalization or desaturation. As with the
existing stellar/reflected reference surfaces, textures and irregular geometry
are not automatically integrated or average-normalized; sparse night maps and
individual lit-mesh materials do not supply a catalog-level total flux.

The conversion is `L * logicalPixelAngularWidth^2 / 2.54e-6`, where the
denominator is the conventional zero-magnitude illuminance in lux. This is a
photopic approximation to V-band Vega units, not a spectral renderer: exact
cd/m^2-to-V-band conversion depends on the source spectrum. Emission is added
before atmospheric transmission and camera exposure, enters the physical meter,
and never changes surface opacity. Night maps retain their existing
directional day/night switch; it controls where lights are on, not their output.
Legacy non-sRGB rendering retains its old brightness and material behavior.

The GPU tests cover night/emissive textures over a black diffuse texture,
constant CMOD emission, zero/multiple lights, albedo, opacity and independent
visual/physical exposure. The unit test checks integrated emission against
stellar flux as logical pixel size changes.

The optional macOS desktop-OpenGL `autoexposure_gpu` target exercises the actual
meter shaders in an offscreen CGL context: isolated pixels and NPOT edges,
percentile-boundary continuity, clipped HDR recovery and independent display
exposure. It requires a working GPU context, not a simulator. Other platforms
still need runtime validation on their own graphics backends.

For the existing `build-annotations` configuration:

```sh
cmake -S . -B build-annotations -DENABLE_TESTS=ON
cmake --build build-annotations --target autoexposure_gpu unit -j6
ctest --test-dir build-annotations -R '^Automatic exposure GPU/' --output-on-failure
build-annotations/test/unit/unit --test-suite=PSF
```
