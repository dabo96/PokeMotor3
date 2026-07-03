# Ray Tracing Audit — PokeMotor2

**Fecha:** 2026-05-16
**Referencia:** [Ray Tracing in One Weekend](https://raytracing.github.io/books/RayTracingInOneWeekend.html) v4.0.2
**Sistemas auditados:** SSR (screen-space reflections) · SDF tracer · VXGI specular cone
**Metodología:** misma que `docs/VXGI_AUDIT.md` — full audit primero, fixes por rondas, el usuario habilita cada ronda.

---

## 0. Nota previa sobre el mismatch libro ↔ motor

El libro describe un **path tracer offline de esferas en world-space** con materiales Lambertian/Metal/Dielectric. El motor tiene tres ray-tracers *en tiempo real*, ninguno world-space contra primitivas analíticas:

| Sistema motor | Espacio | Primitiva | Output |
|---|---|---|---|
| SSR (`ssr_packet_trace.comp` + 5 más) | screen-space | Hi-Z depth | reflejos especulares |
| VXGI specular cone (`lighting_pass.frag:532`) | world-space | textura 3D anisotrópica | indirect specular |
| SDF tracer (`sdf_raytrace.comp`) | world-space | sphere tracing sobre SDF | shadow + AO |

Capítulos del libro que **sí** aplican casi 1:1 a estos sistemas:
- §2 Rays — parametrización `P(t) = A + t·b` (todos)
- §4 Normals & front_face (SSR back-face rejection, SDF sign)
- §5 Antialiasing por sample (SSR multi-sample para rough)
- §6 Diffuse / `ray_t.min = 0.001` (auto-intersección, todos)
- §7 Metal — `reflect(v,n)` + **fuzz** (SSR rough reflections)
- §8 Dielectric — **Schlick** `R(θ) = R₀ + (1-R₀)(1-cosθ)⁵` (Fresnel en todos)
- §6 recursion depth limit (SSR bounce limit)

Capítulos que **no** aplican: §3 sphere intersection (cuadrática), §9 defocus blur (es de cámara).

---

## 1. Executive Summary

| ID | Sistema | Sección libro | Severidad | Título |
|---|---|---|---|---|
| **C1** | SSR | §7 Metal (fuzz) | CRITICAL | No GGX importance sampling — mirror-only reflections, hard cutoff a roughness ≥ 0.35 |
| **C2** | SSR | §5 AA | CRITICAL | 1 ray/pixel sin distribución sobre lóbulo BRDF — incompatible con superficies rough |
| **M1** | SSR | §6 `ray_t.min` | MAJOR | Origin offset = `normal * 0.05` (constante world-space) — falla a grazing angles y profundidades variables |
| **M2** | SSR | (extensión libro) | MAJOR | Hi-Z mip selection es escalera lineal por step count, no por footprint del cono → wastes samples y under-samples |
| **M3** | SSR | §8 Schlick | MAJOR | Fresnel se aplica DESPUÉS de SVGF, mezclando F0 entre pixels metal/dieléctrico antes de pesar — biased blend |
| **M4** | SSR | §6 gamma | MAJOR | Compresión Karis (`1/(1+lum·0.1)`) se aplica antes del SVGF — A-Trous wavelet asume linealidad |
| **M5** | SDF | §4 front_face | MAJOR | Sign-flip workaround con `abs(SDF)` enmascara bug en generación; shadow direccional disabled como consecuencia |
| **M6** | SDF | (gen perf) | MAJOR | Generación O(voxels × triángulos × 4) sin BVH — bloquea el frame por ~segundos |
| **M7** | VXGI spec | §6 jitter | MAJOR | Cono especular no tiene jitter de origen — banding alineado a la grilla |
| **m1** | SSR | §5 variance | MINOR | SVGF `sigmaL` tiene piso fijo en lugar de derivarse íntegramente de variance |
| **m2** | SSR | — | MINOR | `maxDist *= (1 - roughness*2.5)` es dead code: roughness ≥ 0.35 ya sale antes |
| **m3** | VXGI | §6 hemisphere | MINOR | Sky cone usa dirección fija `(0,1,0)` en vez de hemisferio orientado a N |
| **m4** | SSR | §4 front_face | MINOR | No back-face rejection en el hit: rayo que pega cara trasera devuelve color inválido |
| **m5** | SDF | — | MINOR | `SDFShadow` se traza pero el resultado se descarta hardcodeado (`shadow = 1.0`) |

13 hallazgos: 2 críticos, 7 mayores, 4 menores.

---

## 2. Detalle por hallazgo

### C1 — SSR mirror-only, hard cutoff a roughness ≥ 0.35

**Citación libro:** §7 *Metal Materials*, fórmula:
> `reflected = unit_vector(reflect(unit_vector(r_in.direction()), rec.normal) + fuzz * random_unit_vector())`

El libro perturba la reflexión por `fuzz * random_unit_vector()` para simular rugosidad. Esa es la forma **path-tracer-equivalente** de muestrear el lóbulo GGX/microfacet — para un rayo único por pixel.

**Citación código:**
- `Shaders/ss_trace.comp:203` — `if (roughness < 0.35)` (skip total para ≥ 0.35)
- `Shaders/ssr_ray_sort.comp:65` — `if (length(normal) < 0.1 || roughness >= 0.35) return;`
- `Shaders/ssr_packet_trace.comp:149` — `R = reflect(-V, normal);` (sin fuzz)

**Síntoma visible:** transición dura cuando una superficie va de glossy a rough — el reflejo desaparece de golpe. En materiales con normal map fuerte sobre superficie media-rough, se ve "manchado" porque algunos pixels caen <0.35 y otros ≥0.35.

**Fix propuesto:** GGX VNDF importance sampling. Por pixel, calcular un vector half-vector `H` muestreado del lóbulo GGX (Heitz 2018 VNDF), luego `R = reflect(-V, H)`. Generar el sample con blue noise / Halton sequence indexada por frame para que SVGF temporal pueda promediar a través del tiempo:

```glsl
vec3 H = SampleGGXVNDF(viewLocal, roughness, blueNoise2D);
vec3 R = reflect(-V, normalize(TBN * H));
```

El cutoff a 0.35 se quita; aceptamos rayos hasta `roughness ≈ 0.6-0.7` y dejamos IBL/VXGI cubrir más allá.

**Dependencias:** habilita C2 (multi-sample) como follow-up natural; M3 (Fresnel order) se vuelve más urgente porque ahora F0 varía por sample.

---

### C2 — 1 ray/pixel ↔ §5 Antialiasing

**Citación libro:** §5
> `for (int sample = 0; sample < samples_per_pixel; sample++) { ray r = get_ray(i, j); pixel_color += ray_color(r, world); }`

Multi-sampling es lo que convierte un estimador ruidoso en una estimación aceptable.

**Citación código:** un único `R = reflect(-V, normal)` por pixel. SVGF cumple el rol de "averaging" pero sólo a través del tiempo + vecinos espaciales — el motor *espera* superficies estables y movimientos lentos. Bajo cámara rápida o reflejos no-coherentes (rugosidad alta) la calidad colapsa.

**Síntoma visible:** ghosting / smearing cuando cámara rota; reflejos en superficies medio-rough quedan "secos" y planos.

**Fix propuesto:** después de C1, hacer **stochastic supersampling temporal** — un sample por frame, pero con secuencia indexada por `frameId` para que el SVGF temporal integre N muestras sobre N frames. Esto es básicamente lo que ya hace, pero el sample actual es **determinista** (no varía con `frameId`), entonces SVGF temporal sólo promedia el mismo valor. Hay que romper la determinación con blue noise rotated by `frameId`.

**Dependencias:** requiere C1 (necesitamos jitter del lóbulo para que el frame-to-frame varíe).

---

### M1 — Self-intersection offset constante

**Citación libro:** §6 *Diffuse Materials*
> `if (world.hit(r, interval(0.001, infinity), rec)) { ... }`

El `0.001` es un epsilon en *parámetro t del rayo*, escala con la geometría implícita del libro. La adaptación correcta a screen-space es un offset relativo a la **profundidad del píxel** o al **footprint del píxel proyectado**.

**Citación código:**
- `Shaders/ss_trace.comp:210` — `origin = worldPos + normal * 0.05;`
- `Shaders/ssr_packet_trace.comp:199` — `origin = worldPos + normal * 0.05;`

Constante 0.05 m. A 10 m de distancia, esto es ~0.5% de la profundidad → muy chico para evitar self-hit en pixels donde `dot(R, N) ≈ 0`. A 0.5 m de distancia, es 10% de la profundidad → empuja el rayo demasiado, perdiendo contacto cercano.

**Síntoma:** "skip first sample" enmascara el problema parcialmente, pero a grazing angles y en proximidad cercana, la primera muestra válida ya está demasiado lejos → hay un *gap* en la base del reflejo (ej: en el piso, el reflejo del propio objeto empieza demasiado arriba).

**Fix propuesto:**
```glsl
float depth = -(uView * vec4(worldPos, 1.0)).z;       // distancia a cámara en eye-space
float pixelFootprint = depth * 2.0 / float(uHeight);   // tamaño aproximado de 1 pixel a esa profundidad
vec3 origin = worldPos + normal * max(pixelFootprint * 2.0, 0.01);
```

**Dependencias:** ninguna; se puede aplicar aislado.

---

### M2 — Hi-Z mip selection lineal por step index

**Citación código:**
- `Shaders/ss_trace.comp:86-89`
- `Shaders/ssr_packet_trace.comp:84-88`
```glsl
float mipLevel = startMip;
if (i > 4) mipLevel = max(mipLevel, 1.0);
if (i > 16) mipLevel = max(mipLevel, 2.0);
```

Esto NO es Hi-Z tracing: es ray marching **lineal** que sube de mip por contador de iteraciones. El verdadero Hi-Z (McGuire/Mara 2014, Marrs SIGGRAPH 2018) sube de mip cuando el rayo cruza un tile sin intersectar la depth conservadora, y baja al detectar candidato. El mip se elige por **footprint cubierto por el step**, no por step index.

**Síntoma:** sobre superficies a media distancia, los primeros 4 pasos son finos (cara) y se gastan; después, salta a mips coarse y pierde detalle fino. La elección está desalineada con la geometría.

**Fix propuesto (2 niveles):**

*Conservador (1 día):* basar el mip en el step world-size proyectado:
```glsl
float mipLevel = clamp(log2(ssStep.xy * vec2(uWidth, uHeight)).length() / 1.5, 0.0, float(uHiZMipLevels-1));
```

*Correcto (3-5 días):* implementar hierarchical-Z traversal real con back-tracking. Para PokeMotor probablemente exagerado — la versión conservadora basta.

**Dependencias:** ninguna; mejora generalizada de calidad/perf.

---

### M3 — Fresnel aplicado DESPUÉS del SVGF

**Citación libro:** §8 *Dielectrics*, en `dielectric::scatter`:
> `attenuation = color(1.0, 1.0, 1.0); ... if (... reflectance(cos_theta, refraction_ratio) > random_double()) direction = reflect(...);`

Esquema: cada material decide cuánto de la luz reflejada se transporta (la "attenuation" devuelta) en el momento del bounce. La luz no se "blendea" con la reflectancia *después* de filtrar.

**Citación código:**
- `Shaders/ssr_packet_trace.comp:214` — guarda raw `reflectedColor` en `uSSROut`
- `Shaders/lighting_pass.frag:864-870`:
```glsl
vec3 ssrSpec = ssr.rgb * (F * brdf.x + brdf.y + multiScatter);
float fresnelLum = max(F.r, max(F.g, F.b));
float ssrBlend = ssr.a * fresnelLum;
indirectSpec = mix(nonSSR, ssrSpec, ssrBlend);
```

`F` es per-pixel en `lighting_pass`, pero el `ssr.rgb` ya pasó por filter SVGF que **mezcla pixels con diferentes F0** (ej. metal junto a plástico). La cromaticidad correcta del reflejo se pierde en el filtrado.

**Tradeoff:** SVGF necesita una señal cromaticamente *coherente* para el moments-clamping de YCoCg. Aplicar Fresnel antes implica que metales reflejan tinte de su F0 → la señal pre-filtro tiene F0 baked → SVGF aún funciona pero la variance de luminance se infla.

**Fix propuesto:** pre-multiplicar por F0 (no por F completo) en el trace; aplicar el `(1-F0)(1-cosθ)⁵` término angular en el lighting_pass después del denoise. Esto preserva la cromaticidad del material y deja al denoiser ver luminance coherente.

**Dependencias:** independiente, pero benefit grande junto con C1.

---

### M4 — Karis compression antes del SVGF rompe linealidad

**Citación libro:** §6 introduce gamma correction al final del pipeline, no en medio. El principio implícito: **filtrar en linear, tonemap al final.**

**Citación código:**
- `Shaders/ss_trace.comp:218-219`:
```glsl
float lum = dot(reflectedColor, vec3(0.2126, 0.7152, 0.0722));
reflectedColor *= 1.0 / (1.0 + lum * 0.1);
```
- `Shaders/ssr_packet_trace.comp:207-208` — idem.

Esto es **firefly suppression** estilo Karis 2013 — pero Karis lo aplica para una *suma TAA* simple (lerp con history), no para un A-Trous wavelet. El A-Trous (`ssr_svgf_spatial.comp`) hace edge-stopping con `exp(-|L_a - L_b| / σ)`, asumiendo que `|L_a - L_b|` está en linear-light. Comprimir luminancia antes de filtrar inflará erróneamente la similitud de pixels brillantes y achatará detalle alto-frecuencia.

**Síntoma:** reflejos brillantes (sol en agua, neones) se ven excesivamente blurrosos comparados con su detalle original.

**Fix propuesto:** mover la compresión al final del pipeline (después del spatial pass, antes de devolver al lighting_pass). O cambiar a una técnica de moments-clamp que no necesite compresión upstream — el actual neighborhood clamp en YCoCg ya cumple parte del rol.

**Dependencias:** mejora calidad — independiente.

---

### M5 — SDF sign-flip workaround + shadow direccional disabled

**Citación libro:** §4 *Surface Normals*
> `void set_face_normal(const ray& r, const vec3& outward_normal) { front_face = dot(r.direction(), outward_normal) < 0; normal = front_face ? outward_normal : -outward_normal; }`

Determinación de inside/outside es **fundamental** en el libro — toda la lógica de dielectrics depende de saberlo. El motor reconoce el problema pero lo evade en lugar de fixearlo.

**Citación código:**
- `Shaders/sdf_generate.comp:100-114` — sign via 3 raycast directions, majority vote.
- `Shaders/sdf_raytrace.comp:50` — `float d = abs(SampleSDF(p));` (comment: "to avoid sign-flip artifacts on thin geometry").
- `Shaders/sdf_raytrace.comp:128-132` — directional shadow disabled hardcoded.

3 direcciones no son suficientes para geometría delgada (cables, hojas, vidrio). El parity-test con sólo 3 rayos *votando* falla cuando la geometría no es watertight o cuando un voxel está exactamente en la cáscara.

**Fix propuesto:** 6 ejes en lugar de 3 (`±X, ±Y, ±Z`), majority vote sobre 6 con desempate por menor distancia firmada. O directamente usar **generalized winding number** (Jacobson 2013) si la geometría puede ser no-watertight. Una vez la firma es robusta, quitar el `abs()` y reactivar shadow direccional para tests de penumbra fina que CSM no resuelve.

**Dependencias:** unlock — re-habilita SDF shadow direccional → invalida m5.

---

### M6 — SDF generation O(voxels × triángulos × 4)

**Citación libro:** §4 — `hittable_list::hit` itera todos los objetos lineal. Pero el libro es **offline**; el motor regenera el SDF en tiempo de juego.

**Citación código:** `Shaders/sdf_generate.comp:88-114`:
```glsl
for (int i = 0; i < uTriangleCount; i++) {  // closest point: N
    ...
}
for (int dir = 0; dir < 3; dir++) {
    for (int i = 0; i < uTriangleCount; i++) {  // sign vote: 3N
        ...
    }
}
```

Para 128³ voxels × 1000 triángulos × 4 (closest + 3 sign rays) = 8.4 billones de ops. Bloquea el frame por segundos en geometría compleja.

**Fix propuesto:** uniform grid acceleration. Pre-clasificar triángulos en celdas (igual resolución que SDF), durante la generación sólo iterar triángulos en celdas dentro del radio máximo relevante (típicamente <8 voxels). Speedup 10-50× con poca complejidad de código.

**Dependencias:** trabajo "infra"; independiente del resto del audit. Útil para que C1/C2 sean factibles si quieren rebuild SDF más seguido.

---

### M7 — VXGI specular sin jitter

**Citación libro:** §6 — random sampling para AA y diffuse.

**Citación código:** `Shaders/lighting_pass.frag:537`:
```glsl
vec3 origin = worldPos + N * voxelSize * 2.0;
return TraceCone(origin, R, aperture, maxDist);
```

Comparar con diffuse cone (línea 514):
```glsl
float originJitter = HashPixel(TexCoords, 7.31) * 0.3;
float baseOffset = 1.0 + originJitter;
vec3 origin = worldPos + N * voxelSize * baseOffset;
```

El cono diffuse fue jiterado en el audit VXGI (ronda 2) — el specular se quedó afuera.

**Síntoma:** reflejos especulares en VXGI muestran bandas alineadas a la grilla 128³ cuando la cámara está estacionaria.

**Fix propuesto:** 1 línea — añadir jitter idéntico al diffuse:
```glsl
float originJitter = HashPixel(TexCoords, 13.7) * 0.3;
vec3 origin = worldPos + N * voxelSize * (2.0 + originJitter);
```

**Dependencias:** ninguna.

---

### m1 — SVGF `sigmaL` floor

`Shaders/ssr_svgf_spatial.comp:68` — `sigmaL = max(sigmaL, variance > 0.01 ? 0.05 : 1e-6);`

Piso fijo, no derivado del libro. Funciona pero deja calidad sobre la mesa. Saltar a Schied 2017 ASVGF formula: `σ = sigmaL * sqrt(variance) / sqrt(historyLength + 1)`. Mejora marginal — no priorizar.

### m2 — Dead code en max-distance scaling

`maxDist = uSSRMaxDistance * (1.0 - roughness * 2.5)` — siempre evaluado pero el branch `if (roughness < 0.35)` ya filtró. Eliminar después de C1 (cuando el cutoff se levante, el scaling sí tendrá sentido).

### m3 — VXGI sky cone dirección fija

`Shaders/lighting_pass.frag:820` — `TraceCone(skyOrigin, vec3(0,1,0), 0.577, ...)`. Para "is the sky visible from this point" la dirección es OK; para "how much sky illumination", debería ser hemisferio orientado a `N`. El multiplier `mix(0.3, 1.0, dot(N, up))` parchea esto. Aceptable, low priority.

### m4 — No back-face rejection en SSR hit

Pegar un rayo a la cara trasera de un objeto (cuya normal apunta contra `R`) y leer su color del `uPrevFrame` es físicamente inválido — esa cara no irradia hacia el observador. Comparable a `front_face` test del libro §4.

**Fix:** después del hit, leer `nHit = texture(gNormal, hit.uv)` y rechazar si `dot(nHit, R) > 0` (cara trasera). Confidence se va a 0 en ese caso.

### m5 — SDF directional shadow disabled

`Shaders/sdf_raytrace.comp:132` — `float shadow = 1.0;` hardcoded. El cómputo del `SDFShadow` function existe pero no se llama. Death code hasta resolver M5 — entonces ambas se arreglan juntas.

---

## 3. Things you got right

Refresco para que el audit no sea solo negativo:

- **Reflect formula** ✓ `R = reflect(-V, N)` — idéntica al libro §7 (`v - 2(v·n)n`).
- **Schlick Fresnel** ✓ `lighting_pass.frag:168` — coincide al carácter con la fórmula §8 `R₀ + (1-R₀)(1-cosθ)⁵`.
- **Self-intersection avoidance** ✓ skip-first-sample en SSR; voxelSize * 2 en VXGI; voxelSize * 2 en SDF — el principio §6 (`ray_t.min = 0.001`) está respetado en cada espacio.
- **Confidence/fade matemáticamente sano** ✓ — edge fade, distance fade, roughness fade, NdotV fade. Estructura limpia.
- **Octahedral binning para coherencia de packet** — más allá del libro, técnica moderna (Crassin/Aila). Bien diseñado.
- **SVGF temporal con YCoCg neighborhood clamp** — más allá del libro (Schied 2017), implementado correctamente con moments.
- **Specular aperture = roughness²** ✓ — Crassin-style, ya confirmado en el VXGI audit del 2026-05-14.
- **BRDF LUT split-sum + multi-scatter Kulla-Conty** — muy por encima del libro, físicamente fundamentado.
- **Hi-Z min-depth pyramid** — más allá del libro, infra correcta para SSR (aunque M2 indica que el uso no extrae todo el valor).
- **Quilez 2020 penumbra en SDF shadow** — técnica de referencia (más allá del libro), implementada correctamente.

---

## 4. Implementation rounds

Orden sugerido. Cada ronda es "ship-friendly": calidad visible, riesgo acotado, el usuario decide si pasa a la siguiente.

### Ronda 1 — Quick wins (≤1 día, alto ROI) — DONE 2026-05-16
- **M7** ✅ — jitter del origin en VXGI specular cone (1 línea). Verificado visualmente con TAA on/off: ruido temporal en lugar de bandas.
- **m4** ✅ — back-face rejection en SSR hit (~5 líneas en ambos trace shaders). Asumido OK (low risk, escena de test no estresa el caso).
- **m5** ✅ — eliminada la función `SDFShadow` (32 iter raymarch desperdiciadas) + `uLightDir` uniform. Confirmado: +20 FPS con SDF Shadows ON (140→160).

### Ronda 2 — GGX importance sampling (C1 + C2) — DONE 2026-05-16
Implementado:
1. ✅ Hash per-pixel + per-frame (`HashPixel(uv, frameId * 1.7 + offset)`) duplicado en `ssr_packet_trace.comp` y `ss_trace.comp`.
2. ✅ `SampleGGXVNDF(Ve, alpha, u1, u2)` (Heitz 2018) implementado idénticamente en ambos shaders.
3. ✅ `R = reflect(-V, TBN * H)` con fallback a mirror si `dot(R, N) ≤ 0`.
4. ✅ Cutoff levantado: `roughness >= 0.35` → `roughness >= 0.70` en `ssr_ray_sort.comp` y `ss_trace.comp`. `roughFade` también extendido a `smoothstep(0.70, 0.05, roughness)`.
5. ✅ `maxHistoryLen` SVGF temporal: 32 → 48 para absorber variance extra.
6. ✅ C++: `mFrameId` en HybridTracer, incrementado en `TraceScreenSpace` (que siempre se llama primero), pasado como `uFrameId` a SS-trace y packet-trace.

Decisión técnica clave: `ssr_ray_sort.comp` sigue bineando por `R_mirror = reflect(-V, N)` (no VNDF) para mantener packets coherentes — el trace shader hace su propio VNDF sampling. Aceptamos coherencia degradada para superficies rough (esperado).

Pendiente de verificación visual.

### Ronda 3 — Self-intersection + Hi-Z mip — DONE 2026-05-16
- **M1** ✅ — Origin offset escalado por footprint del pixel: `pixelFootprint = eyeDepth * 2 / height`; `origin = worldPos + N * max(pixelFootprint * 2, 0.01)`. Reemplaza el constante `0.05m`. Aplicado en `ssr_packet_trace.comp` y `ss_trace.comp`.
- **M2** ✅ — Mip selection basado en step world-size: `mipLevel = log2(stepSizePixels)` donde `stepSizePixels = pixelLength / steps`. Constante por rayo (porque el step size es constante). Reemplaza la escalera lineal `if (i > 4) mip=1; if (i > 16) mip=2;`. Aplicado en ambos shaders.

Calidad ganada: reflejos en proximidad cercana sin gap visible; mejor uso de muestras Hi-Z (la mip ahora coincide con el footprint real del step). Bonus: el cómputo de mip se sacó del loop → 1 cálculo por ray en vez de 1 por step.

Pendiente de verificación visual.

### Ronda 4 — Fresnel order + firefly compression — DONE 2026-05-16
- **M3** ✅ — F pre-multiplicado en el trace shader: `reflectedColor *= F` donde `F = F0 + (1-F0)(1-cosθ)⁵` y `F0 = mix(0.04, albedo, metallic)`. Aplicado en `ssr_packet_trace.comp` y `ss_trace.comp`. Lighting pass dejó de multiplicar por F (cambio: `ssr.rgb * (F * brdf.x + brdf.y + multiScatter)` → `ssr.rgb * (brdf.x + brdf.y + multiScatter)`). Requirió agregar `gAlbedo` como uniform en `ssr_packet_trace.comp` (no lo tenía) y bindearlo desde `HybridTracer::TraceSSRSorted`.
- **M4** ✅ — Compresión Karis movida del trace al final de `ssr_svgf_spatial.comp`. SVGF temporal y A-Trous spatial ahora ven señal HDR linear, los weights de edge-stopping computan correctamente.

Calidad ganada: cromaticidad correcta de reflejos metal/dieléctrico (SVGF ya no mezcla F0 across boundaries); detalle preservado en zonas brillantes (compresión solo al final).

Pendiente de verificación visual.

### Ronda 5 — SDF correctness + perf — M5 DONE, M6 DEFERRED 2026-05-16
- **M5** ✅ — Sign determination en `sdf_generate.comp`: 3 axes (con votación ≥2/3) → **6 axes** (votación ≥4/6). Para mallas watertight los 6 votan igual; para geometría delgada/no-watertight la mayoría de 6 es robusta. Función `SDFShadow` reactivada en `sdf_raytrace.comp` SIN el workaround `abs()` — confía en el signed distance. Quilez 2020 penumbra preservado. C++ side: `uLightDir` uniform re-introducido, `SDFRayTracer::Trace` recibe `lightDir` de nuevo. Range del shadow capeado a 16 voxel sizes (~12% de la grilla) para complementar CSM sin duplicarlo.
  - **Trade-off:** Pierde los +20 FPS que dio el m5 cleanup en Ronda 1 (vuelve a correr las 32 iter de raymarch por pixel cuando SDF Shadows está ON). Acepta el costo a cambio de penumbra/contact shadow fina que CSM no resuelve.

- **M6** ✅ **DONE 2026-05-17** vía **BVH** (más general que el uniform grid propuesto originalmente). El cap. 2 de [Ray Tracing: The Next Week](https://raytracing.github.io/books/RayTracingTheNextWeek.html) provee la guía:
  - Nueva clase `accel::BVH` (`Rendering/BVH.h/cpp`) — build O(N log N) con median split sobre longest axis, `std::nth_element` para particionar.
  - Cada nodo es 32 bytes (std430-friendly: `vec3 aabbMin + int leftFirst + vec3 aabbMax + int triCount`). Internal: `triCount = 0`, right child siempre en `leftFirst + 1` (children pre-alocados adyacentes).
  - `SDFGenerator` ahora construye BVH y sube 2 SSBOs (triangles + BVH nodes).
  - `sdf_generate.comp` reescrito con dos funciones de traversal stack-based (depth 32):
    - `ClosestPointBVH(p)` — DFS con pruning por distance-to-AABB
    - `CountRayIntersectionsBVH(ro, rd)` — DFS con slab-test pruning
  - Speedup esperado: 20-100× (vs el O(N) anterior). Para un mesh de 5k tris pasa de minutos a segundos.
  - Razón de elegir BVH sobre uniform grid: adapta a densidad de geometría (uniform falla en meshes con clusters densos y áreas vacías). Y queda como infra general reusable para futuras features (mesh-level ray tracing, collision queries).

- **m1** — opcional, SVGF sigma derivado de variance — no implementado (era nice-to-have)

Calidad ganada con M5: shadows direccionales finos donde CSM no resuelve (penumbras en grietas, contact bajo objetos).

---

## 5. Cost estimates (rough)

| Ronda | LOC | Tiempo estimado | Riesgo |
|---|---|---|---|
| 1 | ~15 | 1-2 h | Bajo |
| 2 | ~80 | 1-2 días | Medio (SVGF convergence) |
| 3 | ~30 | 4-6 h | Bajo-Medio |
| 4 | ~25 | 4-6 h | Medio (regression risk en pixels mixed-material) |
| 5 | ~200 (SDF gen rewrite) | 2-3 días | Medio |

Total si querés todo: ~1 semana de trabajo dedicado.

---

## 6. Próximo paso

Decisión tuya: empezar por la **Ronda 1** (low risk, visible) y avanzar; o saltar directo a **Ronda 2** (la GGX importance sampling es la mejora dominante para SSR).

Mi recomendación: **Ronda 1 → verificar visualmente → Ronda 2**. Si Ronda 2 sale bien, las demás se pueden retomar cuando otra prioridad libere tiempo.
