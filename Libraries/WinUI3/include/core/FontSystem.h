#pragma once
#include "Math/Color.h"
#include "Math/Vec2.h"
#include "core/FontMSDF.h"
#include "core/MSDFGenerator.h"
#include "core/FontManager.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace FluentUI {

class RenderBackend;

// Font subsystem extracted from Renderer (brief 23, phase 1). Owns FreeType, the
// bitmap glyph atlas, the static + dynamic MSDF atlases and the multi-font
// manager. It is a DEVICE-level resource: in a multi-window shared-device setup
// (brief 08) the first window's FontSystem OWNS the GPU atlases and every
// secondary window's FontSystem routes every glyph lookup/generation to that
// owner (via owner_), so the atlas is never rebuilt per window.
//
// Renderer keeps the text DRAW methods (they emit quads into its batch); this
// class only provides glyph data, atlas handles and text metrics. The public API
// (LoadFont/MeasureText/…) mirrors what Renderer used to expose, so the Renderer
// facade delegates without changing widget-facing behavior.
class FontSystem {
public:
  struct Glyph {
    Vec2 size;     // glyph size in pixels
    Vec2 bearing;  // offset from baseline to top-left
    float advance = 0.0f;  // horizontal advance in pixels
    Vec2 uv0;
    Vec2 uv1;
    bool valid = false;
    uint32_t lastAccessFrame = 0;  // Perf R6: for LRU eviction
  };

  FontSystem() = default;
  ~FontSystem() { Shutdown(); }

  // Owner init: FreeType + atlas textures + MSDF font/generator + default font.
  bool Init(RenderBackend* backend);
  // Secondary init (brief 08): reference the device-owner's FontSystem; no baking.
  // All reads route to `owner`, so this instance holds no font GPU resources.
  bool InitShared(RenderBackend* backend, FontSystem* owner);
  // Frees the GPU/FreeType resources — only when this instance is the owner.
  // Idempotent (safe to call from Renderer::Shutdown and again from ~FontSystem).
  void Shutdown();
  // Per-frame housekeeping: bump the frame counter and LRU-evict stale dynamic
  // MSDF glyphs. Call once per frame from Renderer::BeginFrame.
  void NewFrame();

  bool LoadFont(const std::string& filepath, int pixelHeight);
  bool LoadIconFont(const std::string& filepath, int pixelHeight);

  // ── Queries used by Renderer's text-draw methods (route to owner if shared) ──
  FontMSDF* ActiveMSDF() const { return Owner()->msdfFont.get(); }
  // ── Brief 35-D: optical-size axis ──────────────────────────────────────────
  // An MSDF pipeline cannot hint, but a variable font's `opsz` axis already bakes
  // the small-size correction into the DESIGN: thicker stems, more open counters,
  // looser spacing. We keep two atlases — `text` (opsz≈14) and `display` (opsz≈28)
  // — and pick by the size in DEVICE pixels: on a 200% display an 11pt label comes
  // out ~29 real px and must use `display`. Falls back to the single text atlas
  // when no display atlas was generated (see tools/gen_text_atlas.sh).
  // NOTE: draw and measure MUST call this with the same size, or layout desyncs
  // from the pixels — the two instances have different advances.
  FontMSDF* MSDFForSize(float devicePx) const;
  // Device-px cutover between the `text` and `display` optical instances.
  static constexpr float DISPLAY_ATLAS_PX = 20.0f;
  FontManager& Manager() { return Owner()->fontManager; }
  void* BitmapAtlasTexture() const { return Owner()->fontAtlasTexture; }
  void* DynamicMSDFAtlasTexture() const { return Owner()->dynamicMSDFAtlasTexture; }

  bool IsFontLoaded() const { return Owner()->fontLoaded; }
  bool IsIconFontLoaded() const { return Owner()->iconFontLoaded; }
  float FontPixelHeight() const { return Owner()->fontPixelHeight; }
  float FontAscent() const { return Owner()->fontAscent; }
  float FontLineHeight() const { return Owner()->fontLineHeight; }
  float IconFontPixelHeight() const { return Owner()->iconFontPixelHeight; }
  float IconFontAscent() const { return Owner()->iconFontAscent; }

  const Glyph* GetGlyph(uint32_t cp);
  const Glyph* GetIconGlyph(uint32_t cp);
  const Glyph* GetOrGenerateMSDFGlyph(uint32_t cp);

  // ── Brief 29 Part A: per-size hinted bitmap buckets for crisp small text ──
  // At small physical sizes MSDF minification puts stems "between" pixels and the
  // apparent weight wobbles per glyph. A hinted bitmap rasterized at the exact
  // device pixel size and drawn 1:1 fixes this. Buckets are keyed by (sizePx,cp)
  // and share the R8 bitmap atlas. `sizePx` is the physical glyph height — in this
  // renderer the projection is in device pixels and fontSize is already physical,
  // so sizePx = round(fontSize) (no separate dpiScale factor).
  struct TextPath { bool bitmap = false; int sizePx = 0; };
  // Single source of truth for the bitmap-vs-MSDF decision, used by BOTH DrawText
  // and MeasureText so layout never desyncs from the pixels drawn.
  TextPath PickTextPath(float fontSize) const;
  // Lazily rasterize+cache the glyph for (sizePx,cp) in the bitmap atlas.
  const Glyph* GetGlyphBucket(int sizePx, uint32_t cp);
  // Grid-fit metrics for a bucket size (read from face->size->metrics, not scaled).
  float BucketAscent(int sizePx);
  float BucketLineHeight(int sizePx);
  // True when a hinted TTF is available to serve buckets (else all text is MSDF).
  bool HasBucketFont() const { return Owner()->fontFace != nullptr; }
  // Threshold (physical px): below it → bitmap buckets, at/above → MSDF. Brief 29
  // suggests wiring this from Style; call this from app setup to override 28.0f.
  void SetBitmapThresholdPx(float px) { Owner()->bitmapThresholdPx_ = px; }
  float BitmapThresholdPx() const { return Owner()->bitmapThresholdPx_; }

  // ── Brief 35-C: vertical grid fitting without deformation ──────────────────
  // Optimal vertical phase δ for a run drawn at `ppem` device pixels, in the
  // range (-0.5, 0.5]. Adding δ to an already pixel-snapped baseline translates
  // the WHOLE run so that its three dominant horizontal features (baseline,
  // x-height, cap-height) land as close to the pixel grid as one shared offset
  // allows. No contour is touched — that is hinting, and invariant 1 forbids it.
  // Cached per (active font, ppem); ~20 live entries for a normal UI.
  float VerticalPhase(float ppem);

  // ── Metrics / measuring ──
  Vec2 MeasureText(const std::string& text, float fontSize);
  Vec2 MeasureTextWrapped(const std::string& text, float maxWidth, float fontSize);
  // Break text into lines, wrapping by word within maxWidth and honoring '\n'.
  std::vector<std::string> WrapTextLines(const std::string& text, float maxWidth,
                                         float fontSize, float& outMaxWidth);
  // Vertical advance per line, consistent with DrawText's multiline stepping.
  float LineAdvancePx(float fontSize);
  float GetFontAscender() const;
  float GetGlyphAdvance(uint32_t codepoint, float fontSize);
  Vec2 MeasureTextWithFont(const std::string& text, const std::string& fontName, float fontSize);

private:
  // The FontSystem that physically owns the GPU/FreeType resources: `this` when
  // standalone / device-owner, or the origin's FontSystem for a secondary window.
  FontSystem* Owner() { return owner_ ? owner_ : this; }
  const FontSystem* Owner() const { return owner_ ? owner_ : this; }
  bool ownsResources() const { return owner_ == nullptr; }

  void InitializeDefaultFont();
  void ClearGlyphs();
  bool LoadGlyph(uint32_t cp);
  bool LoadIconGlyph(uint32_t cp);
  bool GenerateMSDFGlyph(uint32_t cp);
  bool EnsureAtlasSpace(int glyphWidth, int glyphHeight, int& outX, int& outY);
  // Brief 29 Part A: recreate a taller bitmap atlas and re-rasterize every cached
  // glyph (base + icon + buckets) when the shelf packer overflows. Mirrors the
  // dynamic MSDF atlas growth; returns false at the size cap.
  bool GrowBitmapAtlas();
  // Brief 29 Part A: ensure a hinted TTF is loaded into fontFace to serve buckets,
  // even when the MSDF atlas loaded fine (fontFace would otherwise stay null).
  void EnsureBucketFont();
  bool LoadGlyphBucket(int sizePx, uint32_t cp);
  // Brief 35-C: x-height / cap-height of the active face in font units, read once
  // from OS/2 (with an outline-measuring fallback for faces that don't fill it).
  struct DominantLines { float xHeight = 0.0f; float capHeight = 0.0f; float unitsPerEm = 0.0f; bool valid = false; };
  const DominantLines& EnsureDominantLines();
  DominantLines dominantLines_;
  // Key = round(ppem * 4) — quarter-pixel granularity keeps the table small while
  // covering fractional DPI-scaled sizes.
  std::unordered_map<int, float> verticalPhaseCache_;
  bool EnsureDynamicMSDFAtlasSpace(int glyphWidth, int glyphHeight, int& outX, int& outY);

  RenderBackend* backend = nullptr;
  // null => this instance owns the resources; else routes reads to the owner.
  FontSystem* owner_ = nullptr;

  // ── Bitmap (FreeType) font + atlas ──
  uint32_t glyphCacheFrame = 0;
  static constexpr size_t MAX_GLYPH_CACHE = 2048;
  static constexpr uint32_t GLYPH_EVICT_AGE = 600;  // Evict after 10s at 60fps
  std::unordered_map<std::uint32_t, Glyph> glyphCache;
  // Brief 29 Part A: per-size bitmap glyphs. Key = (uint64(sizePx)<<32)|codepoint.
  std::unordered_map<std::uint64_t, Glyph> bucketGlyphCache;
  struct BucketMetrics { float ascent = 0.0f; float lineHeight = 0.0f; bool valid = false; };
  std::unordered_map<int, BucketMetrics> bucketMetrics_;
  BucketMetrics& EnsureBucketMetrics(int sizePx);
  float bitmapThresholdPx_ = 0.0f;          // 0 = MSDF for every size (bitmap buckets
                                            // disabled: MSDF gave more uniform small-text
                                            // weight than the 1:1 bitmap path)
  static constexpr int BITMAP_MAX_PX = 48;  // largest bucket we rasterize
  bool fontLoaded = false;
  float fontPixelHeight = 16.0f;
  float fontAscent = 0.0f;
  float fontDescent = 0.0f;
  float fontLineHeight = 0.0f;
  FT_Library ftLibrary = nullptr;
  FT_Face fontFace = nullptr;

  void* fontAtlasTexture = nullptr;
  int atlasWidth = 0;
  int atlasHeight = 0;
  int atlasNextX = 0;
  int atlasNextY = 0;
  int atlasCurrentRowHeight = 0;
  bool growingAtlas_ = false; // re-entrancy guard for GrowBitmapAtlas
  // Brief 29 Part A: old atlas textures replaced by GrowBitmapAtlas are deleted at
  // the NEXT NewFrame, not immediately — batches queued earlier THIS frame still
  // reference the old handle and must stay valid until EndFrame draws them.
  std::vector<void*> pendingAtlasDeletes_;

  // ── Icon font (secondary bitmap font, shares the bitmap atlas) ──
  FT_Face iconFontFace = nullptr;
  bool iconFontLoaded = false;
  float iconFontPixelHeight = 16.0f;
  float iconFontAscent = 0.0f;
  std::unordered_map<uint32_t, Glyph> iconGlyphCache;

  // ── MSDF (static + dynamic atlas) ──
  std::unique_ptr<FontMSDF> msdfFont;
  // Brief 35-D: optional second static atlas baked from the `opsz` display
  // instance. Null when assets/fonts/atlas_display.{png,json} isn't shipped.
  std::unique_ptr<FontMSDF> msdfFontDisplay;
  std::unique_ptr<MSDFGenerator> msdfGenerator;
  static constexpr int DYNAMIC_ATLAS_SIZE = 2048;
  static constexpr int MSDF_GLYPH_SIZE = 64;
  void* dynamicMSDFAtlasTexture = nullptr;
  int dynamicAtlasWidth = DYNAMIC_ATLAS_SIZE;
  int dynamicAtlasHeight = DYNAMIC_ATLAS_SIZE;
  int dynamicAtlasNextX = 0;
  int dynamicAtlasNextY = 0;
  int dynamicAtlasCurrentRowHeight = 0;
  std::unordered_map<std::uint32_t, Glyph> dynamicMSDFGlyphCache;

  // ── Multi-font manager (Phase 5) ──
  FontManager fontManager;
};

} // namespace FluentUI
