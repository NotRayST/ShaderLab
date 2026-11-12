#include "text_tool.h"
#include "GUI/gui_manager.h"
#include "GUI/types.h"
#if __has_include("layer_system.h")
#include "layer_system.h"
#define SHADERLAB_HAS_LAYER_SYSTEM 1
#else
#define SHADERLAB_HAS_LAYER_SYSTEM 0
#endif

#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <set>

namespace shaderlab {
namespace {

std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), needed);
    return out;
}

std::string wide_to_utf8(const std::wstring &w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

class OutlineTextRenderer final : public IDWriteTextRenderer {
public:
    OutlineTextRenderer(
        ID2D1Factory *d2d_factory,
        ID2D1RenderTarget *rt,
        ID2D1Brush *fill_brush,
        ID2D1Brush *stroke_brush,
        ID2D1StrokeStyle *stroke_style,
        float stroke_width)
        : m_d2d_factory(d2d_factory)
        , m_rt(rt)
        , m_fill_brush(fill_brush)
        , m_stroke_brush(stroke_brush)
        , m_stroke_style(stroke_style)
        , m_stroke_width(stroke_width)
    {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *ppvObject = static_cast<IDWriteTextRenderer *>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_ref;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *isDisabled) override {
        if (isDisabled) *isDisabled = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *transform) override {
        if (!transform) return E_POINTER;
        *transform = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *pixelsPerDip) override {
        if (pixelsPerDip) *pixelsPerDip = 1.0f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void *,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        DWRITE_MEASURING_MODE,
        DWRITE_GLYPH_RUN const *glyphRun,
        DWRITE_GLYPH_RUN_DESCRIPTION const *,
        IUnknown *) override
    {
        if (!glyphRun || !m_rt || !m_d2d_factory) return E_INVALIDARG;

        ComPtr<ID2D1PathGeometry> path;
        HRESULT hr = m_d2d_factory->CreatePathGeometry(&path);
        if (FAILED(hr)) return hr;

        ComPtr<ID2D1GeometrySink> sink;
        hr = path->Open(&sink);
        if (FAILED(hr)) return hr;

        hr = glyphRun->fontFace->GetGlyphRunOutline(
            glyphRun->fontEmSize,
            glyphRun->glyphIndices,
            glyphRun->glyphAdvances,
            glyphRun->glyphOffsets,
            glyphRun->glyphCount,
            glyphRun->isSideways,
            (glyphRun->bidiLevel % 2) != 0,
            sink.Get()
        );
        sink->Close();

        if (FAILED(hr)) {
            // Color/bitmap glyphs without vector outlines fall back to direct glyph rendering
            m_rt->DrawGlyphRun(
                D2D1::Point2F(baselineOriginX, baselineOriginY),
                glyphRun,
                m_fill_brush,
                DWRITE_MEASURING_MODE_NATURAL
            );
            return S_OK;
        }

        D2D1::Matrix3x2F prev_tf;
        m_rt->GetTransform(&prev_tf);
        m_rt->SetTransform(D2D1::Matrix3x2F::Translation(baselineOriginX, baselineOriginY) * prev_tf);

        if (m_stroke_brush && m_stroke_width > 0.01f) {
            m_rt->DrawGeometry(path.Get(), m_stroke_brush, m_stroke_width * 2.0f, m_stroke_style);
        }
        if (m_fill_brush) {
            m_rt->FillGeometry(path.Get(), m_fill_brush);
        }

        m_rt->SetTransform(prev_tf);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void *,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        DWRITE_UNDERLINE const *underline,
        IUnknown *) override
    {
        if (!underline || !m_rt) return S_OK;
        D2D1_RECT_F rect = D2D1::RectF(
            baselineOriginX,
            baselineOriginY + underline->offset,
            baselineOriginX + underline->width,
            baselineOriginY + underline->offset + std::max(1.0f, underline->thickness)
        );
        if (m_stroke_brush && m_stroke_width > 0.01f) {
            m_rt->DrawRectangle(rect, m_stroke_brush, m_stroke_width * 2.0f, m_stroke_style);
        }
        if (m_fill_brush) {
            m_rt->FillRectangle(rect, m_fill_brush);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(
        void *,
        FLOAT baselineOriginX,
        FLOAT baselineOriginY,
        DWRITE_STRIKETHROUGH const *strikethrough,
        IUnknown *) override
    {
        if (!strikethrough || !m_rt) return S_OK;
        D2D1_RECT_F rect = D2D1::RectF(
            baselineOriginX,
            baselineOriginY + strikethrough->offset,
            baselineOriginX + strikethrough->width,
            baselineOriginY + strikethrough->offset + std::max(1.0f, strikethrough->thickness)
        );
        if (m_stroke_brush && m_stroke_width > 0.01f) {
            m_rt->DrawRectangle(rect, m_stroke_brush, m_stroke_width * 2.0f, m_stroke_style);
        }
        if (m_fill_brush) {
            m_rt->FillRectangle(rect, m_fill_brush);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(
        void *,
        FLOAT originX,
        FLOAT originY,
        IDWriteInlineObject *inlineObject,
        BOOL isSideways,
        BOOL isRightToLeft,
        IUnknown *clientDrawingEffect) override
    {
        if (!inlineObject) return S_OK;
        return inlineObject->Draw(nullptr, this, originX, originY, isSideways, isRightToLeft, clientDrawingEffect);
    }

private:
    ~OutlineTextRenderer() = default;
    std::atomic<ULONG> m_ref{ 1 };
    ID2D1Factory *m_d2d_factory = nullptr;
    ID2D1RenderTarget *m_rt = nullptr;
    ID2D1Brush *m_fill_brush = nullptr;
    ID2D1Brush *m_stroke_brush = nullptr;
    ID2D1StrokeStyle *m_stroke_style = nullptr;
    float m_stroke_width = 0.0f;
};

void apply_separable_gaussian_shadow(
    const std::vector<float> &src_alpha,
    uint32_t w,
    uint32_t h,
    float blur_radius,
    float offset_x,
    float offset_y,
    std::vector<float> &out_shadow_alpha)
{
    out_shadow_alpha.assign(static_cast<size_t>(w) * h, 0.0f);
    if (w == 0 || h == 0) return;

    const int ox = static_cast<int>(std::llround(offset_x));
    const int oy = static_cast<int>(std::llround(offset_y));

    std::vector<float> shifted(static_cast<size_t>(w) * h, 0.0f);
    for (uint32_t y = 0; y < h; ++y) {
        int sy = static_cast<int>(y) - oy;
        if (sy < 0 || sy >= static_cast<int>(h)) continue;
        for (uint32_t x = 0; x < w; ++x) {
            int sx = static_cast<int>(x) - ox;
            if (sx < 0 || sx >= static_cast<int>(w)) continue;
            shifted[static_cast<size_t>(y) * w + x] = src_alpha[static_cast<size_t>(sy) * w + sx];
        }
    }

    if (blur_radius < 0.25f) {
        out_shadow_alpha = std::move(shifted);
        return;
    }

    float sigma = std::clamp(blur_radius * 0.5f, 0.5f, 32.0f);
    int radius = std::max(1, static_cast<int>(std::ceil(sigma * 2.5f)));
    std::vector<float> kernel(static_cast<size_t>(radius * 2 + 1), 0.0f);
    float ksum = 0.0f;
    for (int k = -radius; k <= radius; ++k) {
        float wgt = std::exp(-(static_cast<float>(k * k)) / (2.0f * sigma * sigma));
        kernel[static_cast<size_t>(k + radius)] = wgt;
        ksum += wgt;
    }
    for (float &v : kernel) v /= ksum;

    std::vector<float> temp(static_cast<size_t>(w) * h, 0.0f);
    for (uint32_t y = 0; y < h; ++y) {
        size_t row = static_cast<size_t>(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int sx = static_cast<int>(x) + k;
                if (sx >= 0 && sx < static_cast<int>(w)) {
                    acc += shifted[row + sx] * kernel[static_cast<size_t>(k + radius)];
                }
            }
            temp[row + x] = acc;
        }
    }

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int sy = static_cast<int>(y) + k;
                if (sy >= 0 && sy < static_cast<int>(h)) {
                    acc += temp[static_cast<size_t>(sy) * w + x] * kernel[static_cast<size_t>(k + radius)];
                }
            }
            out_shadow_alpha[static_cast<size_t>(y) * w + x] = std::clamp(acc, 0.0f, 1.0f);
        }
    }
}

} // namespace

TextToolEngine &TextToolEngine::get() {
    static TextToolEngine instance;
    return instance;
}

TextToolEngine::~TextToolEngine() {
    shutdown();
}

bool TextToolEngine::ensure_factories() {
    if (!m_dwrite_factory) {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory1),
            reinterpret_cast<IUnknown **>(m_dwrite_factory.GetAddressOf())
        );
        if (FAILED(hr)) return false;
    }

    if (!m_d2d_factory) {
        HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            m_d2d_factory.GetAddressOf()
        );
        if (FAILED(hr)) return false;
    }

    if (!m_wic_factory) {
        HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(co_hr)) {
            m_com_initialized_by_us = true;
        }
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_wic_factory)
        );
        if (FAILED(hr)) return false;
    }

    return true;
}

bool TextToolEngine::initialize() {
    if (m_initialized) return true;
    if (!ensure_factories()) return false;
    refresh_system_fonts();

    std::string initial = m_settings.text_utf8;
    std::memset(m_text_edit_buf, 0, sizeof(m_text_edit_buf));
    std::snprintf(m_text_edit_buf, sizeof(m_text_edit_buf), "%s", initial.c_str());
    m_text_buf_synced = true;

    m_initialized = true;
    return true;
}

void TextToolEngine::shutdown() {
    m_system_fonts.clear();
    m_wic_factory.Reset();
    m_d2d_factory.Reset();
    m_dwrite_factory.Reset();
    if (m_com_initialized_by_us) {
        CoUninitialize();
        m_com_initialized_by_us = false;
    }
    m_initialized = false;
}

bool TextToolEngine::refresh_system_fonts() {
    if (!ensure_factories()) return false;

    ComPtr<IDWriteFontCollection> collection;
    HRESULT hr = m_dwrite_factory->GetSystemFontCollection(&collection, TRUE);
    if (FAILED(hr) || !collection) return false;

    m_system_fonts.clear();
    UINT32 count = collection->GetFontFamilyCount();
    std::set<std::wstring> seen_lower;

    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IDWriteFontFamily> family;
        if (FAILED(collection->GetFontFamily(i, &family)) || !family) continue;

        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(family->GetFamilyNames(&names)) || !names) continue;

        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (FAILED(names->FindLocaleName(L"en-us", &idx, &exists)) || !exists) {
            idx = 0;
        }

        UINT32 len = 0;
        if (FAILED(names->GetStringLength(idx, &len)) || len == 0) continue;

        std::wstring fname(static_cast<size_t>(len), L'\0');
        if (FAILED(names->GetString(idx, fname.data(), len + 1))) continue;

        std::wstring key = fname;
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });
        if (!seen_lower.insert(key).second) continue;

        SystemFontEntry entry;
        entry.family_w = fname;
        entry.family_utf8 = wide_to_utf8(fname);
        m_system_fonts.push_back(std::move(entry));
    }

    std::sort(m_system_fonts.begin(), m_system_fonts.end(), [](const SystemFontEntry &a, const SystemFontEntry &b) {
        return _wcsicmp(a.family_w.c_str(), b.family_w.c_str()) < 0;
    });
    return !m_system_fonts.empty();
}

bool TextToolEngine::rasterize_text_rgba(
    const TextToolSettings &style,
    uint32_t doc_w,
    uint32_t doc_h,
    std::vector<uint8_t> &out_rgba,
    uint32_t &out_w,
    uint32_t &out_h)
{
    if (!ensure_factories()) return false;

    std::wstring wtext = utf8_to_wide(style.text_utf8);
    if (wtext.empty()) {
        wtext = L" ";
    }

    const float clamped_pt = std::clamp(style.font_size, 6.0f, 500.0f);
    const float font_dip = clamped_pt * (96.0f / 72.0f);
    const std::wstring family = style.font_family.empty() ? L"Segoe UI" : style.font_family;

    ComPtr<IDWriteTextFormat> text_format;
    HRESULT hr = m_dwrite_factory->CreateTextFormat(
        family.c_str(),
        nullptr,
        style.font_bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        style.font_italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        font_dip,
        L"en-us",
        &text_format
    );
    if (FAILED(hr) || !text_format) return false;

    DWRITE_TEXT_ALIGNMENT dalign = DWRITE_TEXT_ALIGNMENT_LEADING;
    if (style.text_align == 1) dalign = DWRITE_TEXT_ALIGNMENT_CENTER;
    else if (style.text_align == 2) dalign = DWRITE_TEXT_ALIGNMENT_TRAILING;
    else if (style.text_align == 3) dalign = DWRITE_TEXT_ALIGNMENT_JUSTIFIED;
    text_format->SetTextAlignment(dalign);
    text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    const float max_layout_w = std::max(128.0f, doc_w > 0 ? static_cast<float>(doc_w) * 0.92f : 1600.0f);
    const float max_layout_h = std::max(128.0f, doc_h > 0 ? static_cast<float>(doc_h) * 2.0f : 4096.0f);

    ComPtr<IDWriteTextLayout> layout;
    hr = m_dwrite_factory->CreateTextLayout(
        wtext.c_str(),
        static_cast<UINT32>(wtext.size()),
        text_format.Get(),
        max_layout_w,
        max_layout_h,
        &layout
    );
    if (FAILED(hr) || !layout) return false;

    const DWRITE_TEXT_RANGE full_range{ 0u, static_cast<UINT32>(wtext.size()) };
    layout->SetUnderline(style.font_underline ? TRUE : FALSE, full_range);
    layout->SetStrikethrough(style.font_strikethrough ? TRUE : FALSE, full_range);

    // Leading / Line height
    const float line_mult = std::clamp(style.line_spacing, 0.80f, 2.50f);
    const float line_px = font_dip * line_mult;
    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_px, line_px * 0.80f);

    // Character tracking via IDWriteTextLayout1
    ComPtr<IDWriteTextLayout1> layout1;
    if (SUCCEEDED(layout.As(&layout1)) && layout1) {
        const float track_em = std::clamp(style.tracking, -50.0f, 200.0f) / 1000.0f;
        const float track_dips = track_em * font_dip;
        const float min_adv = (track_dips < 0.0f) ? (font_dip * 0.1f) : 0.0f;
        layout1->SetCharacterSpacing(0.0f, track_dips, min_adv, full_range);
    }

    // Measure tight bounds first so non-justified single/multi-line layers stay compact
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    if (style.text_align != 3) {
        float tight_w = std::max(8.0f, tm.widthIncludingTrailingWhitespace + 4.0f);
        layout->SetMaxWidth(tight_w);
        layout->GetMetrics(&tm);
    }

    DWRITE_OVERHANG_METRICS oh{};
    layout->GetOverhangMetrics(&oh);

    const float stroke_pad = std::clamp(style.stroke_width, 0.0f, 20.0f);
    const float shadow_pad = style.shadow_enabled
        ? (std::clamp(style.shadow_blur, 0.0f, 32.0f) * 2.5f +
           std::max(std::abs(style.shadow_offset_x), std::abs(style.shadow_offset_y)))
        : 0.0f;
    const float pad = std::ceil(stroke_pad + shadow_pad) + 6.0f;

    const float extra_left = std::max(0.0f, -oh.left);
    const float extra_top = std::max(0.0f, -oh.top);
    const float content_w = std::max(1.0f, std::max(tm.widthIncludingTrailingWhitespace, tm.layoutWidth));
    const float content_h = std::max(1.0f, tm.height);

    const uint32_t tex_w = static_cast<uint32_t>(std::clamp(std::ceil(content_w + extra_left + pad * 2.0f), 8.0f, 8192.0f));
    const uint32_t tex_h = static_cast<uint32_t>(std::clamp(std::ceil(content_h + extra_top + pad * 2.0f), 8.0f, 8192.0f));

    ComPtr<IWICBitmap> wic_bmp;
    hr = m_wic_factory->CreateBitmap(
        tex_w,
        tex_h,
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnLoad,
        &wic_bmp
    );
    if (FAILED(hr) || !wic_bmp) return false;

    D2D1_RENDER_TARGET_PROPERTIES rt_props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f
    );
    ComPtr<ID2D1RenderTarget> rt;
    hr = m_d2d_factory->CreateWicBitmapRenderTarget(wic_bmp.Get(), rt_props, &rt);
    if (FAILED(hr) || !rt) return false;

    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    ComPtr<ID2D1SolidColorBrush> fill_brush;
    rt->CreateSolidColorBrush(
        D2D1::ColorF(style.text_color[0], style.text_color[1], style.text_color[2], style.text_color[3]),
        &fill_brush
    );

    ComPtr<ID2D1SolidColorBrush> stroke_brush;
    ComPtr<ID2D1StrokeStyle> stroke_style;
    if (stroke_pad > 0.01f && style.stroke_color[3] > 0.001f) {
        rt->CreateSolidColorBrush(
            D2D1::ColorF(style.stroke_color[0], style.stroke_color[1], style.stroke_color[2], style.stroke_color[3]),
            &stroke_brush
        );
        D2D1_STROKE_STYLE_PROPERTIES ss_props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND
        );
        m_d2d_factory->CreateStrokeStyle(ss_props, nullptr, 0, &stroke_style);
    }

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    ComPtr<OutlineTextRenderer> renderer;
    renderer.Attach(new OutlineTextRenderer(
        m_d2d_factory.Get(),
        rt.Get(),
        fill_brush.Get(),
        stroke_brush.Get(),
        stroke_style.Get(),
        stroke_pad
    ));
    layout->Draw(nullptr, renderer.Get(), pad + extra_left, pad + extra_top);
    hr = rt->EndDraw();
    if (FAILED(hr)) return false;

    // Lock WIC bitmap and read PBGRA bytes
    WICRect lock_rect{ 0, 0, static_cast<INT>(tex_w), static_cast<INT>(tex_h) };
    ComPtr<IWICBitmapLock> bmp_lock;
    hr = wic_bmp->Lock(&lock_rect, WICBitmapLockRead, &bmp_lock);
    if (FAILED(hr) || !bmp_lock) return false;

    UINT stride = 0;
    UINT buf_sz = 0;
    WICInProcPointer data_ptr = nullptr;
    bmp_lock->GetStride(&stride);
    bmp_lock->GetDataPointer(&buf_sz, &data_ptr);
    if (!data_ptr) return false;

    const size_t px_count = static_cast<size_t>(tex_w) * tex_h;
    out_rgba.assign(px_count * 4, 0);

    // Keep premultiplied float RGBA if shadow pass needs Porter-Duff Over composition
    if (style.shadow_enabled && style.shadow_color[3] > 0.001f) {
        std::vector<float> fg_alpha(px_count, 0.0f);
        for (uint32_t y = 0; y < tex_h; ++y) {
            const uint8_t *row = data_ptr + static_cast<size_t>(y) * stride;
            for (uint32_t x = 0; x < tex_w; ++x) {
                fg_alpha[static_cast<size_t>(y) * tex_w + x] = static_cast<float>(row[x * 4 + 3]) / 255.0f;
            }
        }

        std::vector<float> shadow_a;
        apply_separable_gaussian_shadow(
            fg_alpha,
            tex_w,
            tex_h,
            style.shadow_blur,
            style.shadow_offset_x,
            style.shadow_offset_y,
            shadow_a
        );

        const float sr = style.shadow_color[0];
        const float sg = style.shadow_color[1];
        const float sb = style.shadow_color[2];
        const float sa_base = style.shadow_color[3];

        for (uint32_t y = 0; y < tex_h; ++y) {
            const uint8_t *row = data_ptr + static_cast<size_t>(y) * stride;
            for (uint32_t x = 0; x < tex_w; ++x) {
                size_t idx = static_cast<size_t>(y) * tex_w + x;
                // Foreground premultiplied RGB + A in [0..1]
                float fb_p = static_cast<float>(row[x * 4 + 0]) / 255.0f;
                float fg_p = static_cast<float>(row[x * 4 + 1]) / 255.0f;
                float fr_p = static_cast<float>(row[x * 4 + 2]) / 255.0f;
                float fa = static_cast<float>(row[x * 4 + 3]) / 255.0f;

                // Shadow premultiplied
                float sha = shadow_a[idx] * sa_base;
                float shr_p = sr * sha;
                float shg_p = sg * sha;
                float shb_p = sb * sha;

                float inv_fa = 1.0f - fa;
                float out_r_p = fr_p + shr_p * inv_fa;
                float out_g_p = fg_p + shg_p * inv_fa;
                float out_b_p = fb_p + shb_p * inv_fa;
                float out_a = std::clamp(fa + sha * inv_fa, 0.0f, 1.0f);

                uint8_t *dst = &out_rgba[idx * 4];
                if (out_a > 1e-4f) {
                    dst[0] = static_cast<uint8_t>(std::clamp(std::lround((out_r_p / out_a) * 255.0f), 0l, 255l));
                    dst[1] = static_cast<uint8_t>(std::clamp(std::lround((out_g_p / out_a) * 255.0f), 0l, 255l));
                    dst[2] = static_cast<uint8_t>(std::clamp(std::lround((out_b_p / out_a) * 255.0f), 0l, 255l));
                    dst[3] = static_cast<uint8_t>(std::clamp(std::lround(out_a * 255.0f), 0l, 255l));
                }
            }
        }
    } else {
        for (uint32_t y = 0; y < tex_h; ++y) {
            const uint8_t *row = data_ptr + static_cast<size_t>(y) * stride;
            for (uint32_t x = 0; x < tex_w; ++x) {
                uint8_t b_p = row[x * 4 + 0];
                uint8_t g_p = row[x * 4 + 1];
                uint8_t r_p = row[x * 4 + 2];
                uint8_t a = row[x * 4 + 3];
                uint8_t *dst = &out_rgba[(static_cast<size_t>(y) * tex_w + x) * 4];
                if (a == 0) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                } else if (a == 255) {
                    dst[0] = r_p;
                    dst[1] = g_p;
                    dst[2] = b_p;
                    dst[3] = 255;
                } else {
                    float inv_a = 255.0f / static_cast<float>(a);
                    dst[0] = static_cast<uint8_t>(std::clamp(std::lround(r_p * inv_a), 0l, 255l));
                    dst[1] = static_cast<uint8_t>(std::clamp(std::lround(g_p * inv_a), 0l, 255l));
                    dst[2] = static_cast<uint8_t>(std::clamp(std::lround(b_p * inv_a), 0l, 255l));
                    dst[3] = a;
                }
            }
        }
    }

    out_w = tex_w;
    out_h = tex_h;
    return true;
}

bool TextToolEngine::rasterize_text_layer(
    ID3D11Device *device,
    DocumentLayer &layer,
    uint32_t doc_w,
    uint32_t doc_h,
    ID3D11DeviceContext *context)
{
#if SHADERLAB_HAS_LAYER_SYSTEM
    TextToolSettings style;
    style.text_utf8 = wide_to_utf8(layer.text_content);
    style.font_family = layer.font_family;
    style.font_size = layer.font_size;
    style.font_bold = layer.font_bold;
    style.font_italic = layer.font_italic;
    style.font_underline = layer.font_underline;
    style.font_strikethrough = layer.font_strikethrough;
    style.text_align = layer.text_align;
    style.line_spacing = layer.line_spacing;
    style.tracking = layer.tracking;
    std::memcpy(style.text_color, layer.text_color, sizeof(style.text_color));
    style.stroke_width = layer.stroke_width;
    std::memcpy(style.stroke_color, layer.stroke_color, sizeof(style.stroke_color));
    style.shadow_enabled = layer.shadow_enabled;
    style.shadow_offset_x = layer.shadow_offset_x;
    style.shadow_offset_y = layer.shadow_offset_y;
    style.shadow_blur = layer.shadow_blur;
    std::memcpy(style.shadow_color, layer.shadow_color, sizeof(style.shadow_color));

    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!rasterize_text_rgba(style, doc_w, doc_h, rgba, w, h)) {
        return false;
    }

    layer.is_text_layer = true;
    layer.width = w;
    layer.height = h;
    layer.rgba_pixels = std::move(rgba);
    layer.mark_dirty();
    if (device) {
        layer.sync_to_gpu(device, context);
    }
    return true;
#else
    (void)device;
    (void)layer;
    (void)doc_w;
    (void)doc_h;
    (void)context;
    return false;
#endif
}

void TextToolEngine::sync_settings_from_layer(const DocumentLayer &layer) {
#if SHADERLAB_HAS_LAYER_SYSTEM
    m_settings.text_utf8 = wide_to_utf8(layer.text_content);
    m_settings.font_family = layer.font_family;
    m_settings.font_size = layer.font_size;
    m_settings.font_bold = layer.font_bold;
    m_settings.font_italic = layer.font_italic;
    m_settings.font_underline = layer.font_underline;
    m_settings.font_strikethrough = layer.font_strikethrough;
    m_settings.text_align = layer.text_align;
    m_settings.line_spacing = layer.line_spacing;
    m_settings.tracking = layer.tracking;
    std::memcpy(m_settings.text_color, layer.text_color, sizeof(m_settings.text_color));
    m_settings.stroke_width = layer.stroke_width;
    std::memcpy(m_settings.stroke_color, layer.stroke_color, sizeof(m_settings.stroke_color));
    m_settings.shadow_enabled = layer.shadow_enabled;
    m_settings.shadow_offset_x = layer.shadow_offset_x;
    m_settings.shadow_offset_y = layer.shadow_offset_y;
    m_settings.shadow_blur = layer.shadow_blur;
    std::memcpy(m_settings.shadow_color, layer.shadow_color, sizeof(m_settings.shadow_color));

    std::memset(m_text_edit_buf, 0, sizeof(m_text_edit_buf));
    std::snprintf(m_text_edit_buf, sizeof(m_text_edit_buf), "%s", m_settings.text_utf8.c_str());
    m_text_buf_synced = true;
#else
    (void)layer;
#endif
}

void TextToolEngine::apply_settings_to_layer(DocumentLayer &layer) const {
#if SHADERLAB_HAS_LAYER_SYSTEM
    layer.is_text_layer = true;
    layer.text_content = utf8_to_wide(m_settings.text_utf8);
    layer.font_family = m_settings.font_family;
    layer.font_size = m_settings.font_size;
    layer.font_bold = m_settings.font_bold;
    layer.font_italic = m_settings.font_italic;
    layer.font_underline = m_settings.font_underline;
    layer.font_strikethrough = m_settings.font_strikethrough;
    layer.text_align = m_settings.text_align;
    layer.line_spacing = m_settings.line_spacing;
    layer.tracking = m_settings.tracking;
    std::memcpy(layer.text_color, m_settings.text_color, sizeof(layer.text_color));
    layer.stroke_width = m_settings.stroke_width;
    std::memcpy(layer.stroke_color, m_settings.stroke_color, sizeof(layer.stroke_color));
    layer.shadow_enabled = m_settings.shadow_enabled;
    layer.shadow_offset_x = m_settings.shadow_offset_x;
    layer.shadow_offset_y = m_settings.shadow_offset_y;
    layer.shadow_blur = m_settings.shadow_blur;
    std::memcpy(layer.shadow_color, m_settings.shadow_color, sizeof(layer.shadow_color));
#else
    (void)layer;
#endif
}

void TextToolEngine::on_canvas_click(float doc_x, float doc_y) {
    m_req_create_layer = true;
    m_req_create_x = doc_x;
    m_req_create_y = doc_y;
}

bool TextToolEngine::pop_create_layer_request(float &out_doc_x, float &out_doc_y) {
    if (!m_req_create_layer) return false;
    m_req_create_layer = false;
    out_doc_x = m_req_create_x;
    out_doc_y = m_req_create_y;
    return true;
}

bool TextToolEngine::pop_live_update_request() {
    if (!m_req_live_update) return false;
    m_req_live_update = false;
    return true;
}

void TextToolEngine::render_text_tab(
    gui::GuiManager &gui,
    bool has_document,
    bool active_layer_is_text,
    bool include_apply_button)
{
    if (!m_initialized) {
        initialize();
    }
    if (!m_text_buf_synced) {
        std::memset(m_text_edit_buf, 0, sizeof(m_text_edit_buf));
        std::snprintf(m_text_edit_buf, sizeof(m_text_edit_buf), "%s", m_settings.text_utf8.c_str());
        m_text_buf_synced = true;
    }

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), "System Font Typography");
    ImGui::TextDisabled("Click canvas (T) to place, or edit active layer.");
    ImGui::PopTextWrapPos();

    bool changed = false;

    // Text Content input (compact height so Font, Size, Style & Align fit comfortably at 1440x900)
    ImGui::TextUnformatted("Content");
    if (ImGui::InputTextMultiline("##text_content", m_text_edit_buf, sizeof(m_text_edit_buf), ImVec2(-1.0f, 40.0f))) {
        m_settings.text_utf8 = m_text_edit_buf;
        changed = true;
    }
    gui.register_dom_last_item("text_input_content", "Text Content", "input", false, true);

    // Searchable System Font Picker
    std::string cur_font = wide_to_utf8(m_settings.font_family);
    if (cur_font.empty()) cur_font = "Segoe UI";
    ImGui::TextUnformatted("Font Family");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##text_font_family", cur_font.c_str(), ImGuiComboFlags_HeightLarge)) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##font_search_box", "Search installed fonts...", m_font_filter, sizeof(m_font_filter));
        ImGui::Separator();

        std::string filter_low = to_lower_ascii(m_font_filter);
        ImGui::BeginChild("##font_list_scroll", ImVec2(0.0f, 220.0f), false);
        for (const auto &entry : m_system_fonts) {
            if (!filter_low.empty()) {
                std::string fname_low = to_lower_ascii(entry.family_utf8);
                if (fname_low.find(filter_low) == std::string::npos) continue;
            }
            bool selected = (_wcsicmp(entry.family_w.c_str(), m_settings.font_family.c_str()) == 0);
            if (ImGui::Selectable(entry.family_utf8.c_str(), selected)) {
                m_settings.font_family = entry.family_w;
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndChild();
        ImGui::EndCombo();
    }
    gui.register_dom_last_item("text_combo_font", cur_font, "combo", false, true);

    // Font Size + single compact preset row
    ImGui::TextUnformatted("Size (pt)");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##text_size_slider", &m_settings.font_size, 6.0f, 500.0f, "%.0f pt")) {
        changed = true;
    }
    gui.register_dom_last_item("text_slider_size", "Font Size", "slider", false, true);

    static constexpr float kSizePresets[] = { 12.f, 24.f, 36.f, 48.f, 72.f, 96.f };
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ImGui::SameLine();
        char pbuf[16] = {};
        snprintf(pbuf, sizeof(pbuf), "%.0f", kSizePresets[i]);
        if (ImGui::SmallButton(pbuf)) {
            m_settings.font_size = kSizePresets[i];
            changed = true;
        }
    }

    // Style toggles: Bold / Italic / Underline / Strikethrough
    auto toggle_btn = [&](const char *label, const char *dom_id, bool &flag) {
        if (flag) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.78f, 1.0f));
        if (ImGui::Button(label, ImVec2(32.0f, 24.0f))) {
            flag = !flag;
            changed = true;
        }
        if (flag) ImGui::PopStyleColor();
        gui.register_dom_last_item(dom_id, label, "toggle", flag, true);
    };

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Style");
    ImGui::SameLine();
    toggle_btn("B", "text_btn_bold", m_settings.font_bold);
    ImGui::SameLine();
    toggle_btn("I", "text_btn_italic", m_settings.font_italic);
    ImGui::SameLine();
    toggle_btn("U", "text_btn_underline", m_settings.font_underline);
    ImGui::SameLine();
    toggle_btn("S", "text_btn_strike", m_settings.font_strikethrough);

    // Alignment toggles: Left / Center / Right / Justify
    auto align_btn = [&](const char *label, const char *dom_id, int idx) {
        bool active = (m_settings.text_align == idx);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.78f, 1.0f));
        if (ImGui::Button(label, ImVec2(38.0f, 24.0f))) {
            m_settings.text_align = idx;
            changed = true;
        }
        if (active) ImGui::PopStyleColor();
        gui.register_dom_last_item(dom_id, label, "toggle", active, true);
    };

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Align");
    ImGui::SameLine();
    align_btn("Left", "text_align_left", 0);
    ImGui::SameLine();
    align_btn("Ctr", "text_align_center", 1);
    ImGui::SameLine();
    align_btn("Rgt", "text_align_right", 2);
    ImGui::SameLine();
    align_btn("Just", "text_align_justify", 3);

    ImGui::Spacing();

    // Line Height & Character Tracking
    ImGui::TextUnformatted("Line Height");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##text_leading", &m_settings.line_spacing, 0.80f, 2.50f, "%.2fx")) {
        changed = true;
    }

    ImGui::TextUnformatted("Tracking");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##text_tracking", &m_settings.tracking, -50.0f, 200.0f, "%.0f")) {
        changed = true;
    }

    ImGui::Spacing();

    // Fill Color & Outline Stroke
    ImGui::TextUnformatted("Fill Color");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::ColorEdit4("##text_fill_color", m_settings.text_color, ImGuiColorEditFlags_AlphaBar)) {
        changed = true;
    }

    ImGui::TextUnformatted("Outline Stroke");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##text_stroke_w", &m_settings.stroke_width, 0.0f, 20.0f, "%.1f px")) {
        changed = true;
    }
    if (m_settings.stroke_width > 0.01f) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::ColorEdit4("##text_stroke_color", m_settings.stroke_color, ImGuiColorEditFlags_AlphaBar)) {
            changed = true;
        }
    }

    ImGui::Spacing();

    // Drop Shadow
    if (ImGui::Checkbox("Drop Shadow", &m_settings.shadow_enabled)) {
        changed = true;
    }
    if (m_settings.shadow_enabled) {
        ImGui::TextUnformatted("Shadow Blur");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##tx_sh_blur", &m_settings.shadow_blur, 0.0f, 24.0f, "%.1f px")) {
            changed = true;
        }
        ImGui::TextUnformatted("Shadow Offset X");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##tx_sh_ox", &m_settings.shadow_offset_x, -40.0f, 40.0f, "%.1f px")) {
            changed = true;
        }
        ImGui::TextUnformatted("Shadow Offset Y");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##tx_sh_oy", &m_settings.shadow_offset_y, -40.0f, 40.0f, "%.1f px")) {
            changed = true;
        }
        ImGui::TextUnformatted("Shadow Color");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::ColorEdit4("##tx_sh_col", m_settings.shadow_color, ImGuiColorEditFlags_AlphaBar)) {
            changed = true;
        }
    }

    if (changed && active_layer_is_text) {
        m_req_live_update = true;
    }

    if (include_apply_button) {
        ImGui::Spacing();
        render_text_bottom_button(gui, has_document, active_layer_is_text);
    }
}

void TextToolEngine::render_text_bottom_button(
    gui::GuiManager &gui,
    bool has_document,
    bool active_layer_is_text)
{
    if (!has_document) ImGui::BeginDisabled();
    const char *label = active_layer_is_text ? "Re-Rasterize Text Layer" : "Create Text Layer";
    if (ImGui::Button(label, ImVec2(-1.0f, 24.0f))) {
        if (active_layer_is_text) {
            m_req_live_update = true;
        } else {
            m_req_create_layer = true;
            m_req_create_x = -1.0f; // center in document
            m_req_create_y = -1.0f;
        }
    }
    gui.register_dom_last_item("text_btn_apply", label, "button", false, has_document);
    if (!has_document) ImGui::EndDisabled();
}

} // namespace shaderlab
