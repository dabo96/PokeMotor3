# VXGI Audit — PokeMotor2 vs Reference

**Reference:** `docs/voxel.pdf` pp.14–33 (sections 3.2 Global Illumination, 3.3 Voxel Cone Tracing, 4.1 Implementation).
**Date:** 2026-05-14.
**Scope:** Voxelization → Light Injection → Bounce → Anisotropic Mipmap → Cone Tracing in lighting pass.

---

## Executive summary

| # | Section | Severity | Title |
|---|---------|----------|-------|
| **A1** | Voxelize.frag | **CRITICAL** | `atomicExchange` causes per-frame flicker (last-write-wins). Code pretends to average via packed count, but never adds. |
| **A2** | Voxelize.geom | **MAJOR** | No conservative rasterization → cracks on thin/oblique geometry (book p.18, fig 14–16). |
| **B1** | voxel_light_inject_aniso.comp | **MAJOR** | Lambertian missing `1/π` (book Eq3 p.16). GI is ~3× too bright; user compensates by lowering `mGIDiffuseIntensity`. |
| **C1** | voxel_aniso_mipmap.comp | **MAJOR** | Anisotropic downsample lacks front-to-back occlusion blend (book Eq6 p.22). Light bleeds through walls at coarse mips. |
| **D1** | voxel_bounce.comp | **MAJOR** | Bounce traces full **sphere** (14 dirs) instead of normal-oriented **hemisphere** (book §3.3.3 p.20, fig 17). No cosine weighting. |
| **D2** | voxel_bounce.comp | **MAJOR** | Bounce ignores `uVoxelNormal` entirely — has no surface orientation, so it can't do hemisphere correctly. |
| **E1** | TraceCone (lighting_pass.frag) | **MAJOR** | Diffuse origin offset `voxelSize * 3.0` is excessive — surfaces appear dark and disconnected from their bounce. Book Algorithm 1 uses ~1 voxel. |
| **A6** | voxelize.frag | **MINOR** | `PackColor` writes `count=1` but `imageAtomicExchange` overwrites the whole pixel — count is always 1. Code is misleading. |
| **E4** | VoxelConeTraceDiffuse | **MINOR** | 6-cone hemisphere distribution has a gap near the apex (cosine-weighted central + 5 at 60°). Book recommends "many large cones" (p.21). |
| **D3** | voxel_bounce.comp | **MINOR** | Single bounce iteration default (`mBounceIterations = 1`). Book §3.3.4 (p.24) describes accumulating multiple via ping-pong. |
| **E7** | VoxelConeTraceSpecular | **MINOR** | `aperture = tan(roughness * π/4)` opens too aggressively. Book gives no formula, but `aperture = max(roughness, 0.02)` is the common one. |

**Severity legend:**
- **CRITICAL** — visible artifact (flicker, missing lighting, completely wrong).
- **MAJOR** — visible bias (over/under bright, bleed, dark surfaces).
- **MINOR** — quality polish (banding, undersample, scope decision).

---

## Conventions

- Citations: `file_path:line_number` for code; `book p.XX` for the PDF.
- Algorithm 1 = the formal cone-trace pseudocode on book p.27 (fig 21).
- Variables in `code font` are literal identifiers in your codebase.

---

## A. Voxelization

### A1. CRITICAL — `imageAtomicExchange` is last-write-wins, no averaging

**Book p.17, p.26 (fig 19):**
> "The fragment shader where the voxels are saved to the texture... a shadow map is used to calculate the direct light that is saved in each voxel."
The reference Algorithm (fig 19) writes once per fragment but the broader VCT literature (Crassin 2011, which the book cites as ref [1]) explicitly averages multiple fragments mapping to the same voxel.

**Your code** — `Shaders/voxelize.frag:55-57`:
```glsl
imageAtomicExchange(uAccumAlbedo, voxelCoord, PackColor(albedo));
imageAtomicExchange(uAccumNormal, voxelCoord, PackColor(normalEnc));
```
Comment at line 8–11 says "last-write-wins ... averaging is not needed since voxels are typically dominated by a single surface". This is wrong for two reasons:
1. **Triangles straddling voxels:** a single voxel is overlapped by N fragments from different triangles; whichever fragment runs last on the GPU wins — and that order is **non-deterministic across frames**. → per-frame flicker even on a static scene.
2. **`PackColor` writes `(1u << 24u)` in bit 24**, suggesting intent to use it as a count, but `Exchange` discards the previous value, so count is never accumulated. `voxel_resolve.comp:23` reads `(packedAlbedo >> 24u) & 0xFFu` but uses it only as a `count==0` empty test. The intended averaging never happens.

**Fix — option 1 (correct, robust):** Use `imageAtomicAdd` on a per-component packed format and divide by count in resolve.
```glsl
// Pack: 8 bits per channel, summed; count separate
uint packed = uint(albedo.r * 255.0) |
              (uint(albedo.g * 255.0) << 8u) |
              (uint(albedo.b * 255.0) << 16u);
imageAtomicAdd(uAccumAlbedo, voxelCoord, packed);
imageAtomicAdd(uAccumCount, voxelCoord, 1u); // separate R32UI count texture
```
Then resolve divides each channel by count. Cost: one extra `R32UI` 3D texture (`uAccumCount`).

**Fix — option 2 (simpler, cheap):** Replace `Exchange` with `Max` per channel. `imageAtomicMax` is deterministic across runs (independent of execution order), kills the flicker, and is simpler than averaging. Visually inferior to averaging but consistent.
```glsl
imageAtomicMax(uAccumAlbedo, voxelCoord, PackColor(albedo));
```

**Recommendation:** Option 2 first (1-line change, instant kill of flicker). If quality is insufficient, upgrade to option 1 later. Option 1 needs the C++ side to add `uAccumCount` and the resolve shader to divide.

**Dependencies:** none. This is the first fix to land.

---

### A2. MAJOR — No conservative rasterization → cracks in voxel coverage

**Book p.18:**
> "Conservative rasterization is important when voxelizing the scene to minimize errors when later voxelizing it."
And fig 14–16 (pp.18–19) explicitly illustrate the cracks problem: a triangle whose interior covers a voxel center but whose edges miss neighboring voxel centers leaves gaps.

**Your code** — `Shaders/voxelize.geom`:
- Dominant-axis swizzle: ✓ correct (lines 17–24).
- No edge expansion / no `GL_NV_conservative_raster` / no manual conservative pass: ✗

**Effect:** Thin geometry, walls at oblique angles, and silhouette edges are voxelized **with holes**. During cone tracing, these holes leak light through walls (the most visible VXGI artifact).

**Fix — option 1 (hardware, fast, NVIDIA-only):** Enable `GL_NV_conservative_raster` extension in `voxelize.geom`. Single state change in `VoxelizePass`:
```cpp
// In Renderer::VoxelizePass before draw:
glEnable(GL_CONSERVATIVE_RASTERIZATION_NV);
// ... draw entities ...
glDisable(GL_CONSERVATIVE_RASTERIZATION_NV);
```
And add `#extension GL_NV_conservative_raster : enable` in `voxelize.geom`. **Your hardware is RTX 5070 (per the GL_VENDOR log) → supports this natively.**

**Fix — option 2 (manual, portable):** In the geometry shader, expand each triangle's edges outward by `0.5 * texelSize` in clip space (half a voxel in projection). This is the classic Crassin/Hasselgren technique. ~30 lines of geometry shader code.

**Recommendation:** Option 1. You're NVIDIA-locked anyway (your GL debug callback filtering targets NVIDIA IDs), and it's a 4-line change.

**Dependencies:** none.

---

### A6. MINOR — Misleading `count` field in PackColor

**Your code** — `Shaders/voxelize.frag:30-32`:
```glsl
return c.r | (c.g << 8u) | (c.b << 16u) | (1u << 24u);
```
The `(1u << 24u)` is a count of 1, but `imageAtomicExchange` overwrites everything, so the count is always exactly 1 after the last write. `voxel_resolve.comp:23` extracts it but only checks `count == 0u` (line 25) to detect empty voxels.

**Fix:** If you adopt A1 option 1 (atomicAdd + average), the count becomes meaningful — keep it. If you adopt A1 option 2 (atomicMax), remove the `count` bits from the pack (use full 32 bits for RGBA8 or RGB10A2) and use a sentinel `0` value to detect empty.

**Dependencies:** depends on A1 outcome.

---

## B. Light Injection

### B1. MAJOR — Lambertian missing `1/π`

**Book p.16, Eq3:**
> `L_o(ω_o) = L_e(ω_o) + L_i(ω_i) cos(θ_i) f_r(ω_i, ω_o)` and for Lambertian, `f_r = albedo/π` (book p.14, §3.2.6 implies the standard PBR Lambertian).

**Your code** — `Shaders/voxel_light_inject_aniso.comp:213-214`:
```glsl
float NdotL = max(dot(N, uLightDir), 0.0);
vec3 radiance = albedo * uLightColor * uLightIntensity * NdotL * shadow;
```
No `1/π` division. Same on lines 231 (point lights) and 250 (spot lights).

**Effect:** GI is ~π× (3.14×) too bright at the source. User compensates with `mGIDiffuseIntensity = 1.0` but should be `~3.14` for energy conservation. This is why VXGI feels "fluorescent" when enabled.

**Fix:** divide radiance by π in the three accumulations:
```glsl
const float INV_PI = 0.31830988;
vec3 radiance = albedo * uLightColor * uLightIntensity * NdotL * shadow * INV_PI;
```
And then bump `mGIDiffuseIntensity` default from `1.0` to `~3.14` so the visual brightness is unchanged for the user — only now it's physically correct, and future BRDF/material changes won't compound the error.

**Dependencies:** none on the rendering side. Visually a wash — same brightness, just with sane semantics.

---

### B2. NONE — Anisotropic distribution per light direction

**Your code** — `voxel_light_inject_aniso.comp:164-177` (`AccumulateToFaces`): distributes radiance to 6 faces weighted by `max(toLightDir.axis, 0.0)`.

This matches the Crassin anisotropic-voxel convention (each face stores radiance arriving from that axis direction, and a Lambertian distribution maps `cos(θ_face, L)` clamped). **No action needed.**

---

## C. Anisotropic Mipmap

### C1. MAJOR — No front-to-back occlusion in 2×2×2 downsample

**Book p.22, Eq6 (occlusion-aware front-to-back composition):**
```
c ← (1-a)·c' + a·c_s     // color
a ← (1-a) + a_s          // opacity
```
This is the standard volumetric blend used during cone tracing — but it also applies when building the mip chain: a coarse voxel's radiance should be the front-to-back composition of its 4 "front" children weighted by the opacity of the "back" children.

**Your code** — `Shaders/voxel_aniso_mipmap.comp:43-65`: simple weighted average over 2×2×2 with `dirWeight = 1.0 + max(dot(offset * 2.0, faceDir), 0.0)`. No opacity term, no front-to-back composition.

**Effect:** at coarse mip levels (LOD 3+), the voxel "sees through" the front face — radiance from voxels hidden behind a wall (within the same 2³ block) leaks forward. This is the canonical "VXGI light leak" artifact.

**Fix:** anisotropic mip per face needs four front-to-back composition chains. For face `+X` (faceDir = (1,0,0)):
```glsl
// Group the 2×2×2 block as 4 columns along +X.
// For each column, composite front-to-back: front voxel first, then back voxel attenuated by front opacity.
// Then average the 4 columns.

vec3 result = vec3(0.0);
for (int y = 0; y < 2; y++) {
    for (int z = 0; z < 2; z++) {
        // For face +X: "front" is x=1 (the +X side), "back" is x=0.
        vec3 cFront = sampleRadiance(1, y, z); float aFront = sampleOpacity(1, y, z);
        vec3 cBack  = sampleRadiance(0, y, z); float aBack  = sampleOpacity(0, y, z);
        // Front-to-back composite
        vec3 c = cFront + (1.0 - aFront) * cBack;
        result += c;
    }
}
result *= 0.25;
```
And analogously for the other 5 faces (swapping which axis is "front"). Requires sampling **opacity** in the mipmap pass too — currently you only sample `uSrcTexture` (radiance). Add a second `sampler3D uOpacitySrc` uniform and bind opacity alongside radiance.

**Dependencies:** none. Standalone fix in `voxel_aniso_mipmap.comp` + the call site that has to pass opacity. The biggest visual win on the list — kills light bleed across walls.

---

### C2. NONE — Opacity uses isotropic HW mipmap

`VoxelGI.cpp:428` uses `glGenerateTextureMipmap(c.opacity)`. Opacity is single-channel R8, isotropic. Box filter is fine for the trace's occlusion test (it overestimates a bit on diagonal cones but rarely visible). **No action.**

---

## D. Bounce

### D1. MAJOR — Spherical bounce instead of hemispherical

**Book p.20, §3.3.3:**
> "Indirect diffuse illumination is sampled by sending out cones from every surface in a half sphere ... A few large cones will be traced and approximate the incoming indirect light all over the hemisphere Ω."

**Your code** — `Shaders/voxel_bounce.comp:127-137`: traces **14 cones over the full sphere** (6 axial + 8 diagonal), with no orientation by surface normal.

**Effect:** Half the cones for any given voxel point **into** the surface they sit on, integrating "light from below the ground" that doesn't exist or is meaningless. The bounce energy is under-correlated with the surface's actual hemisphere.

**Fix:** Use the voxel's stored normal (`uVoxelNormal` — already exists in light_inject but **never sampled in bounce**). Restrict the 14 cones to the upper hemisphere by clamping `max(dot(dir, N), 0.0)` as a weight:
```glsl
// Sample normal for this voxel
vec3 N = normalize(texelFetch(uVoxelNormal, coord, 0).rgb * 2.0 - 1.0);

for (int i = 0; i < 14; i++) {
    float cosTheta = max(dot(dirs[i], N), 0.0);
    if (cosTheta < 0.01) continue; // skip cones below surface

    vec3 bounced = TraceConeVoxel(originUV, dirs[i], apt) * albedo * cosTheta;
    // ... distribute to faces as before ...
}
// Normalize by sum of (cosTheta * weight) instead of fixed totalWeight.
```
The `cosTheta` weighting both restricts to the hemisphere and applies the Lambertian factor at the same time.

**Dependencies:** D2 (need normal sampler in bounce). Implement together.

---

### D2. MAJOR — `uVoxelNormal` not declared/sampled in bounce

**Your code** — `Shaders/voxel_bounce.comp`: no `uniform sampler3D uVoxelNormal`. The bounce shader is geometrically blind.

**Fix:** Add the uniform and bind in `VoxelGI::ComputeMultiBounceCascade`:
```glsl
// voxel_bounce.comp top:
uniform sampler3D uVoxelNormal;
```
```cpp
// VoxelGI.cpp ComputeMultiBounceCascade — add after the opacity bind (~line 391):
glBindTextureUnit(8, c.normal);
mBounceShader->SetInt("uVoxelNormal", 8);
```

**Dependencies:** ships with D1.

---

### D3. MINOR — Single bounce iteration

**Book p.24, §3.3.4 (multiple bounces):**
> "computing one bounce at a time for each voxel using Eq4. Then store the latest indirect light information each bounce iteration."

**Your code** — `VoxelGI.h:138`: `int mBounceIterations = 1;`. `VoxelGI::ComputeMultiBounce` (line 419) is called once per frame from `Renderer::LightInjectPass` — but it does **one** bounce, despite the field name. The "Multi" in `ComputeMultiBounce` is misleading.

**Fix:** Loop in `ComputeMultiBounce`:
```cpp
void VoxelGI::ComputeMultiBounce(GLuint globalSDF) {
    for (int i = 0; i < mBounceIterations; i++) {
        ComputeMultiBounceCascade(mCascades[0], globalSDF);
        if (mClipmapEnabled) ComputeMultiBounceCascade(mCascades[1], globalSDF);
        if (i + 1 < mBounceIterations) SwapRadianceBuffers(); // ping-pong for next iter
    }
}
```
Then expose `mBounceIterations` in RendererUI as a 1–4 slider. Cost: linear in iterations.

**Dependencies:** D1+D2 (no point doing more bounces of a broken bounce).

---

## E. Cone Tracing (lighting_pass.frag)

### E1. MAJOR — Diffuse origin offset is `3.0 * voxelSize`

**Book p.27, Algorithm 1, line 5:**
> `origin ← pos + Offset × dir`
The book doesn't quantify `Offset`, but Crassin's original implementation and most modern VCT use **0.5 to 1.5 voxels** along the normal.

**Your code** — `Shaders/lighting_pass.frag:514-516`:
```glsl
float originJitter = HashPixel(TexCoords, 7.31) * 0.3;
float baseOffset = 3.0 + originJitter;
vec3 origin = worldPos + N * voxelSize * baseOffset;
```
**Effect:** for a 128³ grid covering ±15 units (your config), voxelSize = 30/128 ≈ 0.234 units. Offset = 3.0 × 0.234 ≈ **0.7 units**. On a 1-unit-tall character, that's 70% of their height pushed away from the surface — the GI cone starts inside a different voxel entirely, often above the floor's voxel layer. Result: indirect light looks **disconnected from the surface**, especially on small objects.

**Fix:** Reduce to `baseOffset = 1.0` (one voxel). Keep the jitter for banding suppression. If self-occlusion appears, you may need to raise to 1.5; never go above 2.0.

The comment in your code says "Flat surfaces need ~2.5 voxels ... curved geometry (spheres) needs more because side cones at 60° trace back into the object's own voxels." That's a real concern, but the right fix is **conservative rasterization (A2)** — once voxelization is gap-free, the cones don't re-enter the same surface, and you can drop offset to 1.0.

**Dependencies:** ideally lands after A2 (conservative raster). Standalone if A2 is deferred — just expect slight darkening near small geometry; tune to taste.

---

### E2. NONE — Algorithm 1 mapping

Your `TraceCone` (lines 368–478) maps cleanly to book Algorithm 1:

| Book Algorithm 1 | Your code |
|------------------|-----------|
| `radiance ← vec3(0)` | `colorAcc = vec3(0.0)` ✓ |
| `distance ← VoxelSize` | `dist = voxelSize * (2.0 + jitter * 0.5)` ✓ (with jitter, fine) |
| `occlusion ← 0` | `occAcc = 0.0` ✓ |
| `while occlusion < 1` | `while ... occAcc < 0.95` ✓ |
| `diameter = 2·tan(θ/2)·distance` | `diameter = 2.0 * aperture * dist` ✓ (your `aperture` = `tan(θ/2)`) |
| `LOD = log2(diameter)` | `log2(max(diameter / voxelSize, 1.0))` ✓ |
| `radiance += sample·(1-occ)` | front-to-back blend at line 400–402 ✓ |
| `occlusion += (1-occ)·sample.a` | line 402 ✓ |

No discrepancy here. **No action.**

---

### E4. MINOR — 6-cone hemisphere has an apex gap

**Book p.21, fig 17:** shows ~6–12 cones for indirect diffuse over the hemisphere, no specific layout prescribed.

**Your code** — `lighting_pass.frag:484-526`: 1 cone along `N` + 5 cones at 60° from `N` (cos=0.5, sin=0.866), distributed at 72° azimuth intervals.

**Geometry check:** 5 side cones at 60° from N each have aperture half-angle = `atan(0.577) ≈ 30°`. They overlap each other but leave a small ring near the apex that the central cone (also 30° half-angle) doesn't fully cover. Result: subtle banding on flat surfaces under high-contrast lighting.

**Fix — option 1:** Add a 7th cone (or change to 4 + 4 layout: 4 inner at 30° from N, 4 outer at 70°). Cost: +16% trace time.

**Fix — option 2:** Increase the central cone aperture to 0.7 (40° half) so it covers the apex ring properly. Cost: zero, just visual tweak.

**Recommendation:** option 2 is free; try first.

**Dependencies:** none.

---

### E7. MINOR — Specular aperture opens too aggressively

**Your code** — `lighting_pass.frag:531`:
```glsl
float aperture = max(tan(roughness * PI * 0.25), 0.02);
```
At roughness=0.5 (typical material): `tan(π/8) ≈ 0.414` → ~22° half-angle. At roughness=1: `tan(π/4) = 1.0` → 45° half-angle. That's a 90° cone for a fully rough surface — too wide for "specular".

**Fix:** common formula from Crassin's papers and follow-ups:
```glsl
float aperture = max(roughness, 0.02); // tan(half-angle) directly proportional
```
This caps full-rough at 45° half-angle equivalent (since `tan⁻¹(1) = 45°`) and is closer to physically-based GGX behavior. Alternatively `aperture = 0.025 + roughness * roughness * 0.5` for a sharper falloff to mirror at low roughness.

**Dependencies:** none. Pure look-and-feel.

---

## F. Things you got right (no action needed)

For balance — your implementation is largely sound. These are correct:

- **Dominant-axis swizzle in voxelize.geom** matches book fig 19 exactly.
- **VoxelizePass setup** (`Renderer.cpp:1051`): depth/cull/colormask disabled, viewport sized to cascade resolution. Textbook.
- **`TraceCone` front-to-back composition** (lines 400–402) — Eq6 from book p.22, correct.
- **LOD formula** `log2(diameter / voxelSize)` — book Algorithm 1, correct.
- **Jitter in cone trace origin** (line 376, 514) — anti-banding technique not in the book but standard practice. Keep.
- **Voxel grid centering with hysteresis** (`VoxelGI::UpdateCenter`) — sound camera tracking.
- **R11F_G11F_B10F radiance format** — efficient HDR storage, perfect for VXGI.
- **Aniso radiance distribution by light direction** in `AccumulateToFaces` — matches Crassin's anisotropic convention.
- **Cone direction weighting in `SampleAniso`** — correct directional reconstruction.

---

## Suggested implementation order

Group by dependency chain. Verify visually after each group.

### Round 1 — Eliminate flicker and physical errors (independent, safe)
1. **A1** (1 line): `imageAtomicExchange` → `imageAtomicMax` in `voxelize.frag:55-57, 62`. Expected: per-frame flicker disappears.
2. **B1** (3 lines): divide radiance by π in `voxel_light_inject_aniso.comp:214, 231, 250`. Bump `mGIDiffuseIntensity` default to `~3.14`. Expected: brightness unchanged, semantics correct.

### Round 2 — Coverage and walls (medium effort, big visual)
3. **A2** (~5 lines): enable `GL_NV_conservative_raster` in `VoxelizePass` + add extension to `voxelize.geom`. Expected: voxel coverage becomes gap-free; light leaks reduce.
4. **E1** (1 line): drop `baseOffset` from 3.0 to 1.0 in `lighting_pass.frag:515`. Expected: GI reconnects to surfaces. Best done after #3 since #3 reduces self-occlusion risk.

### Round 3 — Hemisphere and bounce (medium effort, correctness)
5. **D2** (3 lines): expose `uVoxelNormal` in `voxel_bounce.comp` + bind in `ComputeMultiBounceCascade`.
6. **D1** (~10 lines): apply `cosTheta` hemisphere weight in `voxel_bounce.comp` main loop.

### Round 4 — Wall-leak through coarse mips (most code change)
7. **C1** (~30 lines): rewrite `voxel_aniso_mipmap.comp` to do per-face front-to-back composition. Sample both radiance and opacity. Plumb opacity sampler in.

### Round 5 — Polish (optional, defer until 1–4 are stable)
8. **D3** (~8 lines): real multi-bounce loop in `ComputeMultiBounce`.
9. **E4** (1 line): tweak central diffuse cone aperture.
10. **E7** (1 line): simpler specular aperture formula.
11. **A6** (cleanup): remove or repurpose the count field.

---

## Out of scope (worth noting but not in this PDF)

- **Temporal accumulation** in voxel grid (you have `mGITemporalEnabled` for screen-space; voxel-space temporal would denoise the bounce further).
- **Sparse voxel octrees** (book mentions, doesn't implement). Your dense 128³ + 64³ is fine for the engine's scale.
- **GPU-driven cone count** (LOD-based: more cones for nearby pixels, fewer for distant). Optimization, not correctness.
- **VXAO** as a standalone pass (you compute it inside `VoxelConeTraceDiffuse` as `voxelAO` — fine, but could be separated for cheaper AO-only rendering).

---

## What's needed from you to start fixing

For each round, confirm scope before I touch code. Round 1 is 4 lines total — safest first cut, instant flicker kill. Round 2 is the highest-visual-impact pair (conservative raster + offset reduction). I recommend starting with **Round 1 only**, verifying visually, then deciding on Round 2.

Each fix should be applied with VXGI enabled in `RendererUI` so you can see the before/after directly.
