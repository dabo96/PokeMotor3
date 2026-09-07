#include "core/FontSystem.h"
#include "core/RenderBackend.h"
#include "core/Context.h"      // Log / LogLevel
#include "UI/WidgetHelpers.h"  // DecodeUTF8
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
// Brief 35-C: OS/2 metrics (x-height / cap-height) + outline bbox fallback.
#include FT_TRUETYPE_TABLES_H
#include FT_OUTLINE_H

namespace FluentUI {

// Defined at namespace scope in Renderer.cpp (external linkage). Resolves a
// resource path against the executable/current-dir search roots.
std::filesystem::path ResolveResourcePath(const std::string& resource);

// Brief 29 Part A: copy a FreeType gray bitmap into a tightly-packed w×h R8 buffer
// (honoring pitch/padding). Snapshotting the pixels lets a caller safely trigger an
// atlas grow — which re-rasterizes other glyphs on the same face — between the load
// and the upload, without reading a clobbered slot buffer.
static std::vector<unsigned char> CopyGlyphBitmap(const FT_Bitmap& bmp) {
    int w = static_cast<int>(bmp.width), h = static_cast<int>(bmp.rows);
    std::vector<unsigned char> out;
    if (w <= 0 || h <= 0 || !bmp.buffer) return out;
    out.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    const int pitch = bmp.pitch;
    for (int r = 0; r < h; ++r) {
        const unsigned char* src = bmp.buffer + static_cast<std::ptrdiff_t>(r) * pitch;
        std::memcpy(out.data() + static_cast<size_t>(r) * static_cast<size_t>(w), src, static_cast<size_t>(w));
    }
    return out;
}

// ─── Lifecycle ──────────────────────────────────────────────────────────────

bool FontSystem::Init(RenderBackend* backend) {
    this->backend = backend;
    owner_ = nullptr; // this instance owns the resources
    if (!this->backend) return false;
    if (FT_Init_FreeType(&ftLibrary)) return false;

    // Brief 29 Part A: 2048² gives ample room for several per-size bitmap buckets
    // (a handful of UI sizes × Latin glyphs) alongside icons; GrowBitmapAtlas is
    // the safety net beyond that.
    atlasWidth = 2048; atlasHeight = 2048;
    fontAtlasTexture = backend->CreateTexture(atlasWidth, atlasHeight, nullptr, true);

    dynamicMSDFAtlasTexture = backend->CreateTexture(DYNAMIC_ATLAS_SIZE, DYNAMIC_ATLAS_SIZE, nullptr, false);
    dynamicAtlasNextX = 2; dynamicAtlasNextY = 2;
    dynamicAtlasCurrentRowHeight = 0;

    msdfFont = std::make_unique<FontMSDF>(backend);
    msdfGenerator = std::make_unique<MSDFGenerator>();

    // Initialize FontManager (Phase 5)
    fontManager.Init(backend, ftLibrary);

    InitializeDefaultFont();

    // Phase 5.2: Pre-generate MSDF glyphs only for codepoints the static atlas does
    // NOT already cover. Brief 29: fontFace is now always loaded (for bitmap
    // buckets), so without the GetGlyph() guard this loop would redundantly rebuild
    // ~160 ASCII/Latin glyphs the static atlas already has into the dynamic atlas.
    if (msdfFont && msdfFont->IsLoaded() && fontFace) {
        for (uint32_t c = 32; c < 128; ++c) {
            if (!msdfFont->GetGlyph(c)) GetOrGenerateMSDFGlyph(c);
        }
        // Latin Extended (common accented characters)
        for (uint32_t c = 192; c <= 255; ++c) {
            if (!msdfFont->GetGlyph(c)) GetOrGenerateMSDFGlyph(c);
        }
    }
    return true;
}

bool FontSystem::InitShared(RenderBackend* backend, FontSystem* owner) {
    this->backend = backend;
    owner_ = owner; // reference the owner's resources; hold none of our own
    // Every glyph lookup / metric read routes through Owner(), so no baking and no
    // atlas handles are needed here (brief 08 shared-device text).
    return this->backend != nullptr && owner_ != nullptr;
}

void FontSystem::Shutdown() {
    // Only the resource owner frees the font GPU/FreeType objects. Guarding on a
    // live backend makes this idempotent (Renderer::Shutdown then ~FontSystem).
    if (ownsResources() && backend) {
        fontManager.Shutdown();
        msdfFont.reset();
        msdfFontDisplay.reset(); // brief 35-D
        msdfGenerator.reset();
        ClearGlyphs();
        if (fontFace) { FT_Done_Face(fontFace); fontFace = nullptr; }
        if (iconFontFace) { FT_Done_Face(iconFontFace); iconFontFace = nullptr; }
        if (ftLibrary) { FT_Done_FreeType(ftLibrary); ftLibrary = nullptr; }
        if (fontAtlasTexture) backend->DeleteTexture(fontAtlasTexture);
        for (void* t : pendingAtlasDeletes_) if (t) backend->DeleteTexture(t);
        pendingAtlasDeletes_.clear();
        if (dynamicMSDFAtlasTexture) backend->DeleteTexture(dynamicMSDFAtlasTexture);
    }
    fontAtlasTexture = nullptr;
    dynamicMSDFAtlasTexture = nullptr;
    backend = nullptr;
}

void FontSystem::NewFrame() {
    // Brief 29 Part A: free bitmap-atlas textures retired by GrowBitmapAtlas last
    // frame — the frame that referenced them has now been drawn and presented.
    if (backend && !pendingAtlasDeletes_.empty()) {
        for (void* t : pendingAtlasDeletes_) if (t) backend->DeleteTexture(t);
        pendingAtlasDeletes_.clear();
    }
    // Perf R6: Evict stale dynamic MSDF glyphs periodically. On a secondary
    // FontSystem the dynamic cache lives in the owner, so this is a no-op there.
    glyphCacheFrame++;
    if ((glyphCacheFrame % 300) == 0 && dynamicMSDFGlyphCache.size() > MAX_GLYPH_CACHE) {
        for (auto it = dynamicMSDFGlyphCache.begin(); it != dynamicMSDFGlyphCache.end(); ) {
            if ((glyphCacheFrame - it->second.lastAccessFrame) > GLYPH_EVICT_AGE) {
                it = dynamicMSDFGlyphCache.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void FontSystem::ClearGlyphs() {
    glyphCache.clear();
    dynamicMSDFGlyphCache.clear();
    bucketGlyphCache.clear();
    bucketMetrics_.clear();
}

// ─── Bitmap font ────────────────────────────────────────────────────────────

bool FontSystem::LoadFont(const std::string& filepath, int pixelHeight) {
    // Secondary renderer: load into the owner's shared atlas instead.
    if (!ownsResources()) return Owner()->LoadFont(filepath, pixelHeight);
    std::filesystem::path resolved = ResolveResourcePath(filepath);
    if (resolved.empty()) return false;
    if (FT_New_Face(ftLibrary, resolved.string().c_str(), 0, &fontFace)) return false;
    FT_Set_Pixel_Sizes(fontFace, 0, pixelHeight);
    fontPixelHeight = static_cast<float>(pixelHeight);
    fontAscent = static_cast<float>(fontFace->size->metrics.ascender) / 64.0f;
    fontDescent = static_cast<float>(fontFace->size->metrics.descender) / 64.0f;
    fontLineHeight = static_cast<float>(fontFace->size->metrics.height) / 64.0f;
    fontLoaded = true; ClearGlyphs();
    for (uint32_t c = 32; c < 128; ++c) LoadGlyph(c);
    return true;
}

const FontSystem::Glyph* FontSystem::GetGlyph(uint32_t cp) {
    // Secondary renderer: the owner is the sole writer of the shared bitmap atlas.
    if (!ownsResources()) return Owner()->GetGlyph(cp);
    auto it = glyphCache.find(cp); if (it != glyphCache.end() && it->second.valid) return &it->second;
    if (LoadGlyph(cp)) return &glyphCache[cp];
    return nullptr;
}

bool FontSystem::LoadGlyph(uint32_t cp) {
    if (!fontFace) return false;
    // Brief 29 Part A: the shared fontFace's active size is changed by bucket
    // rasterization, so pin it back to the base height before loading a base glyph.
    FT_Set_Pixel_Sizes(fontFace, 0, static_cast<FT_UInt>(fontPixelHeight));
    if (FT_Load_Char(fontFace, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) return false;
    FT_GlyphSlot slot = fontFace->glyph;
    int w = slot->bitmap.width, h = slot->bitmap.rows;
    // Brief 29 Part A: snapshot pixels + metrics BEFORE EnsureAtlasSpace — if it
    // grows the atlas it re-rasterizes other glyphs on this same face, clobbering
    // `slot`. (Mirrors how the MSDF path snapshots into its own buffer first.)
    float adv = static_cast<float>(slot->advance.x) / 64.0f;
    Vec2 bearing(static_cast<float>(slot->bitmap_left), static_cast<float>(slot->bitmap_top));
    std::vector<unsigned char> pixels = CopyGlyphBitmap(slot->bitmap);
    int ax, ay; if (!EnsureAtlasSpace(w, h, ax, ay)) return false;
    if (w > 0 && h > 0) backend->UpdateTexture(fontAtlasTexture, ax, ay, w, h, pixels.data());
    auto& g = glyphCache[cp];
    g.advance = adv;
    g.bearing = bearing;
    g.size = Vec2(static_cast<float>(w), static_cast<float>(h));
    g.uv0 = Vec2(static_cast<float>(ax) / atlasWidth, static_cast<float>(ay) / atlasHeight);
    g.uv1 = Vec2(static_cast<float>(ax + w) / atlasWidth, static_cast<float>(ay + h) / atlasHeight);
    g.valid = true; return true;
}

// ─── Brief 29 Part A: per-size hinted bitmap buckets ─────────────────────────

void FontSystem::EnsureBucketFont() {
    if (fontFace) return; // fallback path or a prior call already loaded a face
#if defined(_WIN32)
    LoadFont("C:/Windows/Fonts/segoeui.ttf", 14);
#elif defined(__APPLE__)
    LoadFont("/System/Library/Fonts/SFNS.ttf", 14);
#else
    LoadFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14);
#endif
}

// ─── Brief 35-D: optical-size atlas selection ───────────────────────────────

FontMSDF* FontSystem::MSDFForSize(float devicePx) const {
    const FontSystem* o = Owner();
    // The threshold is in DEVICE pixels, not logical size: at 200% scale an 11pt
    // label is ~29 real px and belongs to the display instance.
    if (devicePx >= DISPLAY_ATLAS_PX && o->msdfFontDisplay && o->msdfFontDisplay->IsLoaded())
        return o->msdfFontDisplay.get();
    return o->msdfFont.get();
}

// ─── Brief 35-C: vertical grid fitting ──────────────────────────────────────

const FontSystem::DominantLines& FontSystem::EnsureDominantLines() {
    if (dominantLines_.valid) return dominantLines_;
    EnsureBucketFont();  // the MSDF atlas is baked from this same face
    if (!fontFace) return dominantLines_;  // stays invalid → phase 0 (baseline snap only)

    dominantLines_.unitsPerEm = static_cast<float>(fontFace->units_per_EM);
    if (dominantLines_.unitsPerEm <= 0.0f) return dominantLines_;

    // OS/2 is the cheap, authoritative source when the designer filled version >= 2.
    auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(fontFace, FT_SFNT_OS2));
    if (os2 && os2->version >= 2) {
        dominantLines_.xHeight = static_cast<float>(os2->sxHeight);
        dominantLines_.capHeight = static_cast<float>(os2->sCapHeight);
    }
    // Fallback (and repair for fonts that ship zeros): measure the outlines of 'x'
    // and 'H' unscaled. FT_LOAD_NO_SCALE keeps everything in font units.
    auto measureTop = [&](uint32_t cp) -> float {
        if (FT_Load_Char(fontFace, cp, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0) return 0.0f;
        FT_BBox bbox{};
        FT_Outline_Get_CBox(&fontFace->glyph->outline, &bbox);
        return static_cast<float>(bbox.yMax);
    };
    if (dominantLines_.xHeight <= 0.0f) dominantLines_.xHeight = measureTop('x');
    if (dominantLines_.capHeight <= 0.0f) dominantLines_.capHeight = measureTop('H');

    dominantLines_.valid = (dominantLines_.xHeight > 0.0f || dominantLines_.capHeight > 0.0f);
    return dominantLines_;
}

float FontSystem::VerticalPhase(float ppem) {
    FontSystem* o = Owner();
    if (ppem <= 0.0f) return 0.0f;

    const int key = static_cast<int>(std::lround(ppem * 4.0f));
    auto it = o->verticalPhaseCache_.find(key);
    if (it != o->verticalPhaseCache_.end()) return it->second;

    const DominantLines& m = o->EnsureDominantLines();
    float best = 0.0f;
    if (m.valid) {
        const float scale = ppem / m.unitsPerEm;
        const float xHeight = m.xHeight * scale;
        const float capHeight = m.capHeight * scale;

        auto distToGrid = [](float v) { return std::fabs(v - std::round(v)); };

        float bestErr = std::numeric_limits<float>::max();
        for (int i = 0; i < 64; ++i) {
            const float d = static_cast<float>(i) / 64.0f;
            // Weights reflect how many glyphs rest on each line: everything sits on
            // the baseline, most lowercase reaches the x-height, capitals are the
            // minority. Tune here if a font's proportions argue otherwise.
            const float err = 1.00f * distToGrid(0.0f     + d)    // baseline
                            + 0.70f * distToGrid(xHeight  + d)    // x-height
                            + 0.40f * distToGrid(capHeight + d);  // cap-height
            if (err < bestErr) { bestErr = err; best = d; }
        }
        // The error is periodic with period 1, so δ and δ-1 align identically. Pick
        // the representative closest to zero: same grid fit, at most half a pixel of
        // displacement instead of almost a full one.
        if (best > 0.5f) best -= 1.0f;
    }

    o->verticalPhaseCache_[key] = best;
    return best;
}

FontSystem::TextPath FontSystem::PickTextPath(float fontSize) const {
    const FontSystem* o = Owner();
    // Bitmap buckets only when a hinted TTF is available and the physical size is
    // below the threshold; otherwise MSDF (scalable, sharp when magnified).
    if (o->fontFace && fontSize > 0.0f && fontSize < o->bitmapThresholdPx_) {
        int sizePx = std::clamp(static_cast<int>(std::round(fontSize)), 6, BITMAP_MAX_PX);
        return { true, sizePx };
    }
    return { false, 0 };
}

FontSystem::BucketMetrics& FontSystem::EnsureBucketMetrics(int sizePx) {
    BucketMetrics& m = bucketMetrics_[sizePx];
    if (!m.valid && fontFace) {
        FT_Set_Pixel_Sizes(fontFace, 0, sizePx);
        m.ascent = static_cast<float>(fontFace->size->metrics.ascender) / 64.0f;
        m.lineHeight = static_cast<float>(fontFace->size->metrics.height) / 64.0f;
        if (m.lineHeight <= 0.0f) m.lineHeight = static_cast<float>(sizePx);
        m.valid = true;
    }
    return m;
}

float FontSystem::BucketAscent(int sizePx) { return Owner()->EnsureBucketMetrics(sizePx).ascent; }
float FontSystem::BucketLineHeight(int sizePx) { return Owner()->EnsureBucketMetrics(sizePx).lineHeight; }

bool FontSystem::LoadGlyphBucket(int sizePx, uint32_t cp) {
    if (!fontFace) return false;
    FT_Set_Pixel_Sizes(fontFace, 0, sizePx);
    if (FT_Load_Char(fontFace, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) return false;
    FT_GlyphSlot slot = fontFace->glyph;
    int w = slot->bitmap.width, h = slot->bitmap.rows;
    // Brief 29 Part A: snapshot before EnsureAtlasSpace may grow (see LoadGlyph).
    float adv = static_cast<float>(slot->advance.x) / 64.0f;
    Vec2 bearing(static_cast<float>(slot->bitmap_left), static_cast<float>(slot->bitmap_top));
    std::vector<unsigned char> pixels = CopyGlyphBitmap(slot->bitmap);
    int ax, ay; if (!EnsureAtlasSpace(w, h, ax, ay)) return false;
    if (w > 0 && h > 0) backend->UpdateTexture(fontAtlasTexture, ax, ay, w, h, pixels.data());
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(sizePx)) << 32) | cp;
    auto& g = bucketGlyphCache[key];
    g.advance = adv;
    g.bearing = bearing;
    g.size = Vec2(static_cast<float>(w), static_cast<float>(h));
    g.uv0 = Vec2(static_cast<float>(ax) / atlasWidth, static_cast<float>(ay) / atlasHeight);
    g.uv1 = Vec2(static_cast<float>(ax + w) / atlasWidth, static_cast<float>(ay + h) / atlasHeight);
    g.valid = true; return true;
}

const FontSystem::Glyph* FontSystem::GetGlyphBucket(int sizePx, uint32_t cp) {
    // Secondary renderer: the owner is the sole writer of the shared bitmap atlas.
    if (!ownsResources()) return Owner()->GetGlyphBucket(sizePx, cp);
    if (!fontFace) return nullptr;
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(sizePx)) << 32) | cp;
    auto it = bucketGlyphCache.find(key);
    if (it != bucketGlyphCache.end() && it->second.valid) return &it->second;
    if (LoadGlyphBucket(sizePx, cp)) return &bucketGlyphCache[key];
    return nullptr;
}

// ─── Icon Font (secondary bitmap font) ──────────────────────────────────────

bool FontSystem::LoadIconFont(const std::string& filepath, int pixelHeight) {
    // Secondary renderer: load into the owner's shared atlas instead.
    if (!ownsResources()) return Owner()->LoadIconFont(filepath, pixelHeight);
    std::filesystem::path resolved = ResolveResourcePath(filepath);
    if (resolved.empty()) {
        Log(LogLevel::Error, "Icon font not found: %s", filepath.c_str());
        return false;
    }
    if (FT_New_Face(ftLibrary, resolved.string().c_str(), 0, &iconFontFace)) {
        Log(LogLevel::Error, "Failed to load icon font: %s", resolved.string().c_str());
        return false;
    }
    FT_Set_Pixel_Sizes(iconFontFace, 0, pixelHeight);
    iconFontPixelHeight = static_cast<float>(pixelHeight);
    iconFontAscent = static_cast<float>(iconFontFace->size->metrics.ascender) / 64.0f;
    iconFontLoaded = true;
    Log(LogLevel::Info, "Icon font loaded: %s (%dpx)", resolved.string().c_str(), pixelHeight);
    return true;
}

bool FontSystem::LoadIconGlyph(uint32_t cp) {
    if (!iconFontFace) return false;
    FT_Set_Pixel_Sizes(iconFontFace, 0, static_cast<FT_UInt>(iconFontPixelHeight));
    if (FT_Load_Char(iconFontFace, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) return false;
    FT_GlyphSlot slot = iconFontFace->glyph;
    int w = slot->bitmap.width, h = slot->bitmap.rows;
    // Brief 29 Part A: snapshot before EnsureAtlasSpace may grow (see LoadGlyph).
    float adv = static_cast<float>(slot->advance.x) / 64.0f;
    Vec2 bearing(static_cast<float>(slot->bitmap_left), static_cast<float>(slot->bitmap_top));
    std::vector<unsigned char> pixels = CopyGlyphBitmap(slot->bitmap);
    int ax, ay; if (!EnsureAtlasSpace(w, h, ax, ay)) return false;
    if (w > 0 && h > 0) backend->UpdateTexture(fontAtlasTexture, ax, ay, w, h, pixels.data());
    auto& g = iconGlyphCache[cp];
    g.advance = adv;
    g.bearing = bearing;
    g.size = Vec2(static_cast<float>(w), static_cast<float>(h));
    g.uv0 = Vec2(static_cast<float>(ax) / atlasWidth, static_cast<float>(ay) / atlasHeight);
    g.uv1 = Vec2(static_cast<float>(ax + w) / atlasWidth, static_cast<float>(ay + h) / atlasHeight);
    g.valid = true; return true;
}

const FontSystem::Glyph* FontSystem::GetIconGlyph(uint32_t cp) {
    // Secondary renderer: icon glyphs live in the owner's shared bitmap atlas.
    if (!ownsResources()) return Owner()->GetIconGlyph(cp);
    auto it = iconGlyphCache.find(cp);
    if (it != iconGlyphCache.end() && it->second.valid) return &it->second;
    if (LoadIconGlyph(cp)) return &iconGlyphCache[cp];
    return nullptr;
}

// ─── Atlas packing ──────────────────────────────────────────────────────────

bool FontSystem::EnsureAtlasSpace(int w, int h, int& ox, int& oy) {
    int pad = 2;
    if (atlasNextX + w + pad > atlasWidth) { atlasNextX = 0; atlasNextY += atlasCurrentRowHeight + pad; atlasCurrentRowHeight = 0; }
    if (atlasNextY + h + pad > atlasHeight) {
        // Brief 29 Part A: grow instead of failing silently (the old behavior
        // dropped glyphs once buckets filled 1024²). growingAtlas_ blocks the
        // re-rasterization pass from recursively re-growing.
        if (growingAtlas_ || !GrowBitmapAtlas()) return false;
        // Retry placement in the grown (reset) atlas.
        if (atlasNextX + w + pad > atlasWidth) { atlasNextX = 0; atlasNextY += atlasCurrentRowHeight + pad; atlasCurrentRowHeight = 0; }
        if (atlasNextY + h + pad > atlasHeight) return false;
    }
    ox = atlasNextX + pad; oy = atlasNextY + pad;
    atlasNextX += w + pad; atlasCurrentRowHeight = std::max(atlasCurrentRowHeight, h);
    return true;
}

bool FontSystem::GrowBitmapAtlas() {
    int newH = atlasHeight * 2;
    if (newH > 8192) {
        Log(LogLevel::Warning, "Bitmap font atlas at size cap (%dx%d); some glyphs may be dropped",
            atlasWidth, atlasHeight);
        return false;
    }
    void* newTex = backend->CreateTexture(atlasWidth, newH, nullptr, true);
    if (!newTex) return false;
    // Defer deletion of the old texture: batches queued earlier this frame still
    // reference it and are drawn at EndFrame. NewFrame frees it next frame.
    pendingAtlasDeletes_.push_back(fontAtlasTexture);
    fontAtlasTexture = newTex;
    atlasHeight = newH;
    atlasNextX = 0; atlasNextY = 0; atlasCurrentRowHeight = 0;

    // Re-rasterize every cached glyph into the fresh atlas. Each loader pins its own
    // face to the right pixel size and snapshots pixels, so this pass is safe.
    growingAtlas_ = true;
    auto base    = std::move(glyphCache);       glyphCache.clear();
    auto icons   = std::move(iconGlyphCache);   iconGlyphCache.clear();
    auto buckets = std::move(bucketGlyphCache); bucketGlyphCache.clear();
    for (auto& kv : base)  LoadGlyph(kv.first);
    for (auto& kv : icons) LoadIconGlyph(kv.first);
    for (auto& kv : buckets) {
        int sz = static_cast<int>(kv.first >> 32);
        uint32_t cp = static_cast<uint32_t>(kv.first);
        LoadGlyphBucket(sz, cp);
    }
    growingAtlas_ = false;
    Log(LogLevel::Info, "Bitmap font atlas grown to %dx%d", atlasWidth, atlasHeight);
    return true;
}

// ─── Dynamic MSDF glyphs ────────────────────────────────────────────────────

const FontSystem::Glyph* FontSystem::GetOrGenerateMSDFGlyph(uint32_t cp) {
    // Secondary renderer: the owner is the sole writer of the shared dynamic atlas.
    if (!ownsResources()) return Owner()->GetOrGenerateMSDFGlyph(cp);
    auto it = dynamicMSDFGlyphCache.find(cp);
    if (it != dynamicMSDFGlyphCache.end() && it->second.valid) {
        it->second.lastAccessFrame = glyphCacheFrame; // Perf R6: touch for LRU
        return &it->second;
    }
    if (GenerateMSDFGlyph(cp)) {
        dynamicMSDFGlyphCache[cp].lastAccessFrame = glyphCacheFrame;
        return &dynamicMSDFGlyphCache[cp];
    }
    return nullptr;
}

bool FontSystem::GenerateMSDFGlyph(uint32_t cp) {
    if (!msdfGenerator || !fontFace) return false;
    FT_UInt idx = FT_Get_Char_Index(fontFace, cp); if (idx == 0) return false;
    auto data = msdfGenerator->GenerateFromGlyph(fontFace, idx, MSDF_GLYPH_SIZE, 4.0f, 4);
    if (!data) return false;
    int ax, ay; if (!EnsureDynamicMSDFAtlasSpace(data->width, data->height, ax, ay)) return false;
    backend->UpdateTexture(dynamicMSDFAtlasTexture, ax, ay, data->width, data->height, data->pixels.data());
    FT_Load_Glyph(fontFace, idx, FT_LOAD_NO_BITMAP);
    auto& g = dynamicMSDFGlyphCache[cp];
    g.size = Vec2(static_cast<float>(data->width), static_cast<float>(data->height));
    g.bearing = Vec2(static_cast<float>(fontFace->glyph->metrics.horiBearingX)/64.0f, static_cast<float>(fontFace->glyph->metrics.horiBearingY)/64.0f);
    g.advance = static_cast<float>(fontFace->glyph->metrics.horiAdvance)/64.0f;
    g.uv0 = Vec2(static_cast<float>(ax)/DYNAMIC_ATLAS_SIZE, static_cast<float>(ay)/DYNAMIC_ATLAS_SIZE);
    g.uv1 = Vec2(static_cast<float>(ax+data->width)/DYNAMIC_ATLAS_SIZE, static_cast<float>(ay+data->height)/DYNAMIC_ATLAS_SIZE);
    g.valid = true; return true;
}

bool FontSystem::EnsureDynamicMSDFAtlasSpace(int w, int h, int& ox, int& oy) {
    int pad = 2;
    if (dynamicAtlasNextX + w + pad > dynamicAtlasWidth) {
        dynamicAtlasNextX = pad;
        dynamicAtlasNextY += dynamicAtlasCurrentRowHeight + pad;
        dynamicAtlasCurrentRowHeight = 0;
    }
    if (dynamicAtlasNextY + h + pad > dynamicAtlasHeight) {
        // Dynamic Atlas Growth — create a larger atlas and re-generate glyphs
        int newHeight = dynamicAtlasHeight * 2;
        if (newHeight > 8192) return false; // Max 8192px

        void* newAtlas = backend->CreateTexture(dynamicAtlasWidth, newHeight, nullptr, false);
        if (!newAtlas) return false;

        backend->DeleteTexture(dynamicMSDFAtlasTexture);
        dynamicMSDFAtlasTexture = newAtlas;
        dynamicAtlasHeight = newHeight;

        dynamicAtlasNextX = pad;
        dynamicAtlasNextY = pad;
        dynamicAtlasCurrentRowHeight = 0;

        auto oldGlyphs = std::move(dynamicMSDFGlyphCache);
        dynamicMSDFGlyphCache.clear();
        for (auto& [cp, _] : oldGlyphs) {
            GenerateMSDFGlyph(cp);
        }

        if (dynamicAtlasNextX + w + pad > dynamicAtlasWidth) {
            dynamicAtlasNextX = pad;
            dynamicAtlasNextY += dynamicAtlasCurrentRowHeight + pad;
            dynamicAtlasCurrentRowHeight = 0;
        }
        if (dynamicAtlasNextY + h + pad > dynamicAtlasHeight) return false;
    }
    ox = dynamicAtlasNextX; oy = dynamicAtlasNextY;
    dynamicAtlasNextX += w + pad; dynamicAtlasCurrentRowHeight = std::max(dynamicAtlasCurrentRowHeight, h);
    return true;
}

void FontSystem::InitializeDefaultFont() {
    std::filesystem::path atlasPng = ResolveResourcePath("assets/fonts/atlas.png");
    std::filesystem::path atlasJson = ResolveResourcePath("assets/fonts/atlas.json");

    bool textReady = false;
    if (!atlasPng.empty() && !atlasJson.empty()) {
        if (msdfFont->Load(atlasPng.string(), atlasJson.string())) {
            Log(LogLevel::Info, "MSDF Font loaded successfully from: %s", atlasPng.string().c_str());
            textReady = true;
        }
    }

    // Brief 35-D: optional `display` optical instance (opsz≈28) for text at or
    // above DISPLAY_ATLAS_PX device pixels. Entirely opt-in — shipping only the
    // text atlas keeps the previous single-atlas behavior and VRAM cost.
    std::filesystem::path displayPng = ResolveResourcePath("assets/fonts/atlas_display.png");
    std::filesystem::path displayJson = ResolveResourcePath("assets/fonts/atlas_display.json");
    if (textReady && !displayPng.empty() && !displayJson.empty()) {
        msdfFontDisplay = std::make_unique<FontMSDF>(backend);
        if (msdfFontDisplay->Load(displayPng.string(), displayJson.string())) {
            Log(LogLevel::Info, "MSDF display atlas loaded (opsz): %s", displayPng.string().c_str());
        } else {
            msdfFontDisplay.reset();
            Log(LogLevel::Warning, "MSDF display atlas found but failed to load — using the text atlas at every size");
        }
    }

    if (!textReady) {
        Log(LogLevel::Error, "Could not load MSDF font, falling back to System font.");
    }

    // Brief 29 Part A: load a hinted TTF into fontFace so the per-size bitmap
    // buckets have a face to rasterize small UI text crisply. This serves BOTH as
    // the MSDF-fail fallback render font and, when MSDF loaded fine, the bucket
    // font for sub-threshold sizes. No-op if a face is already loaded.
    EnsureBucketFont();

    // Auto-load the Lucide icon font here so icon glyphs render on EVERY entry
    // point — FluentApp, the standalone gallery, or an external-GL host — not only
    // when FluentApp's constructor runs. FluentApp may override the path afterwards.
    if (!iconFontLoaded) {
        LoadIconFont("assets/fonts/lucide.ttf", 32);
    }
}

// ─── Metrics / measuring ────────────────────────────────────────────────────

Vec2 FontSystem::MeasureText(const std::string& text, float fontSize) {
    // Brief 29 Part A: measure with the SAME path DrawText will use, so layout can
    // never desync from the pixels drawn. The bitmap bucket replicates DrawText's
    // rounded-pen accumulation (Part D); metrics are physical (at sizePx).
    TextPath tp = PickTextPath(fontSize);
    if (tp.bitmap) {
        float lineH = BucketLineHeight(tp.sizePx);
        float penX = 0.0f, maxW = 0.0f; int lines = 1;
        const char* ptr = text.data(); const char* end = ptr + text.size();
        while (ptr < end) {
            uint32_t cp = DecodeUTF8(ptr, end);
            if (cp == 0) break;
            if (cp == '\n') { maxW = std::max(maxW, penX); penX = 0.0f; lines++; continue; }
            const Glyph* g = GetGlyphBucket(tp.sizePx, cp);
            if (!g) g = GetGlyphBucket(tp.sizePx, '?'); // mirror DrawText's fallback
            if (g) penX = std::round(penX + g->advance);
        }
        maxW = std::max(maxW, penX);
        return { maxW, lineH * static_cast<float>(lines) };
    }
    FontMSDF* mf = MSDFForSize(fontSize); // brief 35-D: same instance DrawText picks
    if (mf && mf->IsLoaded()) {
        float scale = fontSize / mf->GetEmSize(); float currentW = 0.0f, maxW = 0.0f;
        float totalH = fontSize * mf->GetLineHeight();
        const char* ptr = text.data(); const char* end = ptr + text.size();
        while (ptr < end) {
            uint32_t cp = DecodeUTF8(ptr, end);
            if (cp == 0) break;
            if (cp == '\n') { maxW = std::max(maxW, currentW); currentW = 0.0f; totalH += fontSize * mf->GetLineHeight(); continue; }
            const FontMSDF::Glyph* g = mf->GetGlyph(cp);
            if (g) currentW += g->advance * scale; else currentW += fontSize * 0.3f;
        }
        return {std::max(maxW, currentW), totalH};
    }
    const FontSystem* o = Owner();
    if (!o->fontLoaded) return {static_cast<float>(text.size()) * fontSize * 0.6f, fontSize};
    float scale = fontSize / o->fontPixelHeight; float currentW = 0.0f, maxW = 0.0f;
    float totalH = (o->fontLineHeight > 0 ? o->fontLineHeight : o->fontPixelHeight) * scale;
    const char* ptr = text.data(); const char* end = ptr + text.size();
    while (ptr < end) {
        uint32_t cp = DecodeUTF8(ptr, end);
        if (cp == 0) break;
        if (cp == '\n') { maxW = std::max(maxW, currentW); currentW = 0.0f; totalH += (o->fontLineHeight * scale); continue; }
        const Glyph* g = GetGlyph(cp);
        if (g) currentW += g->advance * scale;
    }
    return {std::max(maxW, currentW), totalH};
}

float FontSystem::LineAdvancePx(float fontSize) {
    TextPath tp = PickTextPath(fontSize); // Brief 29 Part A: match DrawText's path
    if (tp.bitmap) return BucketLineHeight(tp.sizePx);
    FontMSDF* mf = MSDFForSize(fontSize); // brief 35-D: same instance DrawText picks
    if (mf && mf->IsLoaded()) return fontSize * mf->GetLineHeight();
    const FontSystem* o = Owner();
    if (o->fontLoaded) {
        float scale = fontSize / o->fontPixelHeight;
        return (o->fontLineHeight > 0 ? o->fontLineHeight : o->fontPixelHeight) * scale;
    }
    return fontSize;
}

std::vector<std::string> FontSystem::WrapTextLines(const std::string& text, float maxWidth,
                                                   float fontSize, float& outMaxWidth) {
    std::vector<std::string> lines;
    outMaxWidth = 0.0f;
    const float spaceW = GetGlyphAdvance(' ', fontSize);
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string paragraph = (nl == std::string::npos)
                                    ? text.substr(start)
                                    : text.substr(start, nl - start);
        std::string line;
        float lineW = 0.0f;
        bool anyWord = false;
        size_t wstart = 0;
        while (wstart <= paragraph.size()) {
            size_t sp = paragraph.find(' ', wstart);
            std::string word = (sp == std::string::npos)
                                   ? paragraph.substr(wstart)
                                   : paragraph.substr(wstart, sp - wstart);
            if (!word.empty()) {
                float wordW = MeasureText(word, fontSize).x;
                if (!anyWord) {
                    line = word; lineW = wordW; anyWord = true;
                } else if (lineW + spaceW + wordW <= maxWidth) {
                    line += ' '; line += word; lineW += spaceW + wordW;
                } else {
                    lines.push_back(line);
                    outMaxWidth = std::max(outMaxWidth, lineW);
                    line = word; lineW = wordW;
                }
            }
            if (sp == std::string::npos) break;
            wstart = sp + 1;
        }
        lines.push_back(line);
        outMaxWidth = std::max(outMaxWidth, lineW);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

Vec2 FontSystem::MeasureTextWrapped(const std::string& text, float maxWidth, float fontSize) {
    if (fontSize <= 0.0f) fontSize = 16.0f;
    float maxW = 0.0f;
    std::vector<std::string> lines = WrapTextLines(text, maxWidth, fontSize, maxW);
    return { maxW, LineAdvancePx(fontSize) * static_cast<float>(lines.size()) };
}

float FontSystem::GetFontAscender() const {
    // Size-agnostic query: both optical instances share the family's ascender, so
    // the text atlas answers for all of them (brief 35-D).
    FontMSDF* mf = ActiveMSDF();
    if (mf && mf->IsLoaded()) {
        return mf->GetAscender();
    }
    // Fallback razonable para fonts típicos (sans-serif estándar).
    return 0.8f;
}

float FontSystem::GetGlyphAdvance(uint32_t codepoint, float fontSize) {
    TextPath tp = PickTextPath(fontSize); // Brief 29 Part A: match DrawText's path
    if (tp.bitmap) {
        const Glyph* g = GetGlyphBucket(tp.sizePx, codepoint);
        if (g) return g->advance;
        return fontSize * 0.5f;
    }
    FontMSDF* mf = MSDFForSize(fontSize); // brief 35-D: same instance DrawText picks
    if (mf && mf->IsLoaded()) {
        float scale = fontSize / mf->GetEmSize();
        const FontMSDF::Glyph* g = mf->GetGlyph(codepoint);
        if (g) return g->advance * scale;
        return fontSize * 0.3f;
    }
    const FontSystem* o = Owner();
    if (!o->fontLoaded) return fontSize * 0.6f;
    float scale = fontSize / o->fontPixelHeight;
    const Glyph* g = GetGlyph(codepoint);
    if (g) return g->advance * scale;
    return fontSize * 0.6f;
}

Vec2 FontSystem::MeasureTextWithFont(const std::string& text, const std::string& fontName, float fontSize) {
    return Manager().MeasureText(fontName, text, fontSize);
}

} // namespace FluentUI
