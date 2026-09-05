# Ring scattering

The optional `Scattering` block in an SSC `Rings` definition describes a
plane-parallel particulate layer without planet-specific shader parameters.
It separates extinction, single-scattering albedo, and angular scattering.
It is a single-scattering baseline, not a complete dense-ring or dust-volume
model. Existing ring definitions are unchanged.

## Example

This is a synthetic material example, **not a fit to any planet**. Radii are
in the same units as existing ring definitions (kilometers by default).

```text
Rings
{
    Inner 100000
    Outer 120000
    Scattering
    {
        OpticalDepth 0.1
        SingleScatteringAlbedo [0.6 0.6 0.6]
        PhaseAsymmetry 0.5
    }
}
```

Positive `PhaseAsymmetry` means forward scattering, negative means
backscattering, and zero means isotropic scattering. No texture is required.
The same inputs apply to a planet, a moon, or another body with rings.

## Optical-property contract

| Property inside `Scattering` | Default | Meaning |
|---|---|---|
| `OpticalDepth` | `0` | Finite, nonnegative normal extinction optical depth. If a depth texture is supplied, this is its scale. Zero means an empty layer. |
| `OpticalDepthTexture` | None | Linear red-channel radial depth profile, sampled at `v = 0.5`. Values are clamped to `[0,1]` and multiplied by `OpticalDepth`. Alpha is ignored. |
| `SingleScatteringAlbedo` | `[1 1 1]` | Linear RGB scattering/extinction ratios, each in `[0,1]`. Not sRGB display colors; no 8-bit color conversion. |
| `SingleScatteringAlbedoTexture` | None | Linear RGB radial albedo, sampled at `v = 0.5`, clamped to `[0,1]`, and multiplied by `SingleScatteringAlbedo`. Alpha is ignored. |
| `PhaseAsymmetry` | `0` | Finite HG asymmetry, strictly between `-1` and `1`. Used when no phase texture is supplied. |
| `PhaseFunctionTexture` | None | Linear RGB lookup of the particle phase function **P only**, normalized so its solid-angle integral is `4*pi`. Overrides HG; albedo is still applied separately. |

The depth profile spans `Inner` to `Outer`. It can represent broad or narrow
axisymmetric rings and gaps, subject to texture and screen resolution.
It does not yet describe arcs or vertical density.

The albedo texture uses the same radial coordinate as the depth texture.
It changes scattered and ambient light, not extinction or ring shadows.
Keep albedo separate from the normalized phase texture: baking radial
brightness factors into `P` would violate its normalization.

The phase texture uses the branch's existing encoding:

```text
u = (radius - Inner) / (Outer - Inner)
theta = pi - phaseAngle
v = 1 - (theta/pi)^(1/4)
P = exp2(-24 + 40 * textureRGB)
```

`theta = 0` is forward scattering. Thus `g > 0` peaks at a phase angle of
180 degrees, not zero. Phase textures should be generated and normalized
offline using an appropriate particle model. The renderer does not infer
particle size or composition from RGB.

Optical textures are loaded in linear color space without generated mipmaps.
Averaging depth is not generally equivalent to averaging transmission, and
averaging logarithmically encoded phase values is not energy-preserving.
Radial phase interpolation therefore decodes adjacent columns before mixing
their phase functions. Angular interpolation retains the logarithmic encoding.
The physical ring surface uses eight radial samples across an approximate
pixel footprint, averaging scattered radiance and opacity after evaluation.
It does not average depth or logarithmic phase values before scattering.
This reduces aliasing without turning narrow opaque bands into a uniformly
semitransparent slab. It is a finite box-filter approximation, not exact
anisotropic footprint integration; shadow lookups and very sharp angular
peaks still require further filtering work.

Within `Modify`, omitted scattering fields retain their values. Empty texture
filenames remove the corresponding map. `Scattering false` disables the block.
Invalid numeric properties, property types, and unresolved optical-map filenames
are reported and do not partially update the ring system.

### Precision and texture support

For faint dust, use a DX10 DDS with `DXGI_FORMAT_R32G32B32A32_FLOAT` (RGBA32F).
An 8-bit depth map at scale 7 has steps of about 0.0275 in normal optical depth,
far too coarse for dust with optical depths around `1e-4`. Float maps preserve
small depth values and phase-encoding values above 1; the latter are valid
for narrow normalized forward peaks and must not be clamped.

Float DDS input must be a tightly packed, non-array 2D image; supplied mip
levels are supported by the image loader, although optical maps disable
mipmapping. Float storage and linear filtering must both be supported by the
GPU. On OpenGL ES, 32-bit float filtering requires the relevant extension;
optical samplers use high precision. Unsupported uploads fail explicitly
rather than silently converting to 8-bit or half-float data.

Optical maps must fit in a single hardware texture in both dimensions.
Virtual and automatically tiled textures are rejected for these samplers,
including shadow lookups, which do not implement tile-coordinate remapping.
Byte-only image operations, procedural evaluators, splash/galaxy templates,
and PNG/JPEG export do not accept RGBA32F.

## Compatibility

With no `Scattering` block, `Texture`, `Color`, and the root-level
`PhaseFunctionTexture` retain their existing behavior.

With a `Scattering` block, root-level `Texture`, `Color`, and
`PhaseFunctionTexture` are not used by the physical path. They may remain in
the catalog for use after `Scattering false`.

**The two phase-texture contracts are different.** The older root-level phase
texture stores an already albedo-weighted source (`omega * P`) and obtains
depth by inverting artwork alpha. The new nested texture stores only `P`.
Do not move the existing Saturn LUT into the new block unchanged: that would
double-apply albedo and mix incompatible optical-depth conventions.

## Rendering and shadows

For each source, reflected or diffusely transmitted radiance is computed with
the classical plane-parallel single-scattering expressions. The HG function is
normalized to `4*pi`. Small optical depths and the equal-elevation transmission
limit use cancellation-resistant expressions.

Direct line-of-sight transmission is:

```text
T = exp(-normalOpticalDepth / abs(dot(normal, viewDirection)))
output = scatteredRingContribution + T * background
```

RGB already contains the integrated scattering contribution. The blend factors
are `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`, with alpha `1-T`; there is no additional
RGB multiplication by alpha.

Physical ring shadows on surfaces, meshes, clouds, and atmospheres use the same
depth profile and scale, with the light direction replacing the view direction.
Constant-depth rings also cast shadows. Legacy shadows retain their original
alpha behavior. Optical-map loads do not temporarily substitute a uniform slab
or an unrelated phase function while an asynchronous load is pending.

The result follows Celestia's existing lighting scale; this is not a renderer-wide
absolute radiometric calibration. The ambient-light term is an approximation,
not a multiple-scattering or planetshine solution.

For faint-ring photographic comparisons, `SceneExposure` in `celestia.cfg`
scales the fully composited linear HDR scene before sRGB output conversion.
It requires `SRGBRendering`, defaults to `1`, and works with `ToneMappingMode
"Off"`. This changes camera presentation, not ring optical depth or scattering.
Unlike stretching an 8-bit screenshot, it preserves faint detail until final
output quantization. When exponential tone mapping is enabled, its
`ToneMappingExposure` is applied after this scene exposure.

## HDR stars behind rings

[CelestiaProject/Celestia#2661](https://github.com/CelestiaProject/Celestia/issues/2661)
is a required visual regression case. Correct draw order does not guarantee a
visible reduction after tone mapping. For example, a source value of 60000 and
transmission of 0.01 still produce a background contribution of 600.

Do not clamp the source to 1, tone-map before attenuation, or increase opacity
based on background brightness to force an occultation. A transmitted value
above 1 is valid HDR data; a very bright source can remain visible through a
partially transmitting ring.

The ring test should distinguish raw scene-linear attenuation from final display
appearance, and cover sources in front of and behind rings, gaps, high depth,
grazing views, and exposure changes. The broader PSF issue also needs separate
attention to source/support brightness calibration and to applying optical PSF
spread after scene visibility/transmission rather than treating an expanded
glare sprite as a physical emitting surface. This ring-property change alone
does **not** resolve that architectural issue.

## Remaining physical limitations

- The current phase-LUT lookup retains the solar-angular-radius cutoff. It is
  not integration over the finite, possibly partly occulted solar disc.
- HG is a fallback model, not a replacement for size-integrated diffraction or
  measured phase functions. The existing external Saturn generator's global
  inverse-cube diffraction tail is not corrected by this change.
- Single scattering omits multiple scattering, finite packing, wake anisotropy,
  and intrinsic/interparticle opposition effects.
- A local zero-thickness slab and a grazing-angle cosine floor do not represent
  extended dusty halos or paths through several radial regions.
- Physical penumbrae, calibrated planetshine, arcs, and more accurate
  transmission-aware footprint filtering remain separate extensions.
- Occultation-derived apparent optical depths must be interpreted with their
  wavelength, geometry, forward-scattering collection, and saturation limits.
  They are not automatically microscopic extinction depths.

## Scientific references

- [Salo & Karjalainen (2003)](https://doi.org/10.1016/S0019-1035(03)00132-5):
  Monte Carlo transfer, finite packing, and the classical baseline.
- [Salo, Karjalainen & French (2004)](https://doi.org/10.1016/j.icarus.2004.03.012):
  azimuthal reflection/transmission asymmetry from self-gravity wakes.
- [Porco et al. (2008)](https://doi.org/10.1088/0004-6256/136/5/2172):
  classical reflection/transmission equations, explicit-particle models,
  optical-depth definitions, and planetshine.
- [Harbison, Nicholson & Hedman (2013)](https://arxiv.org/abs/1312.2927):
  size-integrated diffraction, distinct angular regimes, and occultation
  interpretation.
- [Hedman & Stark (2015)](https://arxiv.org/html/1508.00261):
  measured D68 and G-ring phase curves, empirical three-HG fits, and the
  uncertainty in extrapolating their extreme forward-scattering peaks.

## Real-image references

These are observations, not artist impressions. Public-release JPGs are useful
for morphology and qualitative lighting, not calibrated absolute radiance or
RGB albedo. Preserve exposure, filter, viewing geometry, and processing metadata
when selecting original archival data for quantitative comparisons.

| System | Official image page | Observation and useful comparison | Caveats |
|---|---|---|---|
| Saturn | [High-phase Rings, PIA09875](https://science.nasa.gov/photojournal/high-phase-rings/) | Cassini, visible light, 2008-02-20; 166-degree phase, about 2 degrees above the unlit ring face. Faint inner rings and high-phase scattering. | Exposure/display stretch unspecified; cosmic-ray specks are not particles. Credit: NASA/JPL/Space Science Institute. |
| Saturn | [Cassini Targets a Propeller in Saturn's A Ring, PIA21433](https://science.nasa.gov/photojournal/cassini-targets-a-propeller-in-saturns-a-ring/) | Cassini, 2017-02-21; real views from lit and unlit sides, reprojected to 207 m/pixel. Density/brightness response on opposite faces. | Local feature, not a whole-ring profile; no numerical phase angles supplied. Credit: NASA/JPL-Caltech/Space Science Institute. |
| Jupiter | [Jovian Ring System Mosaic, PIA03001](https://science.nasa.gov/photojournal/jovian-ring-system-mosaic/) | Galileo SSI, 1996-11-09; spacecraft in Jupiter's shadow looking toward the Sun. Dusty-ring morphology in backlight. | Use the observed top portion, not the schematic inset. Mosaic; filter/stretch unspecified. Credit: NASA/JPL/University of Arizona. |
| Jupiter | [Jupiter's Rings, PIA09249](https://science.nasa.gov/photojournal/jupiters-rings/) | New Horizons LORRI, 2007-02-24; narrow main ring and fainter inward material. | Processed approach image, NOT the later strongly backlit view; phase unspecified. Credit: NASA/Johns Hopkins University Applied Physics Laboratory/Southwest Research Institute. |
| Uranus | [Rings of Uranus, PIA01981](https://science.nasa.gov/photojournal/rings-of-uranus/) | Voyager 2, clear filter, 1986-01-23; approximately 10 km resolution. Narrow-ring spacing and the eta ring's two components. | Phase/exposure/stretch unspecified; not a resolved-width reference below 10 km. Credit: NASA/JPL. |
| Uranus | [Rings of Uranus, PIA01985](https://science.nasa.gov/photojournal/rings-of-uranus-2/) | Voyager 2, 1986-01-24; 0.5-second exposure, rings silhouetted against bright clouds. Transmission against a planetary background. | Foreshortening affects widths. Caption says wide-angle camera; page metadata says NAC. Credit: NASA/JPL. |
| Neptune | [Neptune: Ring Arcs, PIA02256](https://science.nasa.gov/photojournal/neptune-ring-arcs/) | Voyager 2 NAC, clear filter, 1989-08-19; 61-second exposure. Azimuthal arcs and their nonuniform edges. | Bright upper corner is a residual from an earlier exposure, not ring material. Arc positions are epoch dependent. Credit: NASA/JPL. |
| Neptune | [Neptune's Rings, PIA02207](https://science.nasa.gov/photojournal/neptunes-rings/) | Voyager 2 departing view, clear filter, 135-degree phase; 111-second exposure. Forward scattering and clumpy arcs. | Caption says wide-angle, metadata says NAC. The caption's brightness comparison is not a matched JPG pixel calibration. Credit: NASA/JPL. |

Images are linked rather than redistributed. Consult
[NASA's media guidelines](https://www.nasa.gov/nasa-brand-center/images-and-media/)
and each image's credits before copying assets; NASA hosting alone is not a
blanket license for third-party material.

### Reproducing the Cassini PIA09875 viewing geometry

The release matches archived ISS WAC frame **W1582222673**, with two detector
pixels cropped from each edge of its 1024-square image, without rotation or
reflection. Its [original label](https://opus.pds-rings.seti.org/holdings/volumes/COISS_2xxx/COISS_2042/data/1582034353_1582239032/W1582222673_1.LBL)
gives an exposure midpoint of **2008-02-20 17:41:14.562 UTC**, an 80 ms exposure,
and clear filters CL1/CL2.

The [original index](https://opus.pds-rings.seti.org/holdings/metadata/COISS_2xxx/COISS_2042/COISS_2042_index.tab),
row 1646, gives boresight RA **344.42168 degrees**, declination
**-5.8401394 degrees**, and target north **31.186941 degrees clockwise from
image up**. These are pointing constraints, not a direction toward Saturn's
center. Use the [index field definitions](https://opus.pds-rings.seti.org/holdings/metadata/COISS_2xxx/COISS_2042/COISS_2042_index.lbl)
when interpreting its position vectors: the spacecraft-to-planet vector is
corrected for light travel time and stellar aberration, unlike a simultaneous
sampled trajectory.

Two caption values must not be used as camera constraints without qualification:

- The approximately 166-degree phase is at the system center. The
  [observed ring patch](https://opus.pds-rings.seti.org/api/metadata/co-iss-w1582222673.json)
  spans approximately **169-173 degrees**.
- The index's approximately 9 km/pixel is defined at the subspacecraft point
  on the target body, not at Saturn's center distance. The
  [ISS camera kernel](https://naif.jpl.nasa.gov/pub/naif/CASSINI/kernels/ik/cas_iss_v10.ti)
  specifies a WAC focal length of 200.77 mm and 12 micrometer pixels, giving
  an undistorted pinhole field of approximately **3.506 degrees** across 1024
  pixels. Inferring a 2.49-degree field from 9 km/pixel and 211,000 km is wrong.

Matching this geometry does not establish a photometric match. The image shows
D-ring material below the 74,510 km inner boundary of the current main-ring
profile. A uniform HG fallback and an 8-bit depth map cannot by themselves
reproduce the observed faint dust structures and their angular response.

### Conditional Saturn photometry experiment

Use the [calibrated source label](https://opus.pds-rings.seti.org/holdings/calibrated/COISS_2xxx/COISS_2042/data/1582034353_1582239032/W1582222673_1_CALIB.LBL)
and corresponding `W1582222673_1_CALIB.IMG`, not the public JPEG, for radiance
comparisons. The CISSCAL 4.0beta product has units of I/F. Its VICAR header
specifies a 4096-byte label and one 4096-byte binary-header record: actual
little-endian float pixels begin at byte 8192. Account for additive sky
background and retain negative residuals until aggregation; clipping noisy
samples first biases faint material bright.

The same issue applies to UVIS occultation profiles. In the selected
`TAU01KM` products, `-1` is the missing-depth sentinel; other negative
measurements are noise estimates, not missing values. Aggregate signed
measurements before enforcing a nonnegative physical profile. A weak positive
median of positive-only samples is not a D-ring detection.

One experimental material fixes main-ring depth to this corrected UVIS proxy
and fits an effective dust extinction fraction to calibrated, unshadowed ISS
samples. Large particles use separately normalized diffraction and reflected
phase components. Porco's C-ring reflected phase law is a published empirical
model, but extending it to this image's 169-173 degree phase is an extrapolation,
not a measurement at those angles.

The dust prior is the [D68 three-HG fit, Table 7](https://arxiv.org/html/1508.00261#S4.T7):

```text
g = [0.995, 0.585, 0.005]
w = [0.754, 0.151, 0.095]
P_dust(theta) = sum(w_i * HG_4pi(g_i, theta))
```

These weights describe an empirical angular curve, not particle abundances.
Do not add another diffraction tail to that measured curve. Its extension
from D68 to the broad D continuum and C-ring dust is an explicit assumption;
its absolute normalization also depends on an unobserved extreme forward peak.

For a dust extinction fraction `f`, use a scattering-weighted mixture:

```text
omega = (1-f)*omega_large + f*omega_dust
P = ((1-f)*omega_large*P_large + f*omega_dust*P_dust) / omega
```

Fit radial fractions within `[0,1]`, report estimates outside those bounds,
and leave unsupported radial regions unconstrained rather than inventing
structure. In the thin D ring, inferred depth is conditional on the assumed
albedo and phase. Save reference-angle normal I/F alongside that depth proxy;
it is the more directly constrained photometric quantity.

The experiment uses 50 km radial medians and alternating 64-pixel blocks for
fitting and held-out comparisons. These test prediction elsewhere **within the
same observation**, not independent physical validity or unique particle sizes.
The fitted material can absorb residual planetshine, stray light, camera
distortion, or errors in the large-particle model. It is an isolated
experimental catalog, not a replacement for installed planetary defaults.

Compare the calibrated reference and an actual application capture using the
same linear exposure and sRGB output, with tone mapping off. Label CPU forward
predictions separately; do not present them as rendered application output.
