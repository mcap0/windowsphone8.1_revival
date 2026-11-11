// Navit graphics plugin "d2d" for Windows Phone 8.1: Direct2D 1.1 + DirectWrite + WIC, drawing into an offscreen
// bitmap per layer (root map + overlays) that is composed into the SwapChainPanel's swap chain.
// Modeled on navit/graphics/qt5 (offscreen pixmaps + compose on paint). All calls run on the Navit thread.
#include <vector>
#include <string>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C" {
#include "config.h"
#include "item.h"
#include "attr.h"
#include "point.h"
#include "graphics.h"
#include "color.h"
#include "callback.h"
#include "debug.h"
#include "event.h"
#include "plugin.h"
#include "window.h"
}
#include "navit_wp81.h"

using Microsoft::WRL::ComPtr;

struct graphics_gc_priv;

struct graphics_priv {
    bool root;
    struct graphics_priv *parent;
    ComPtr<ID2D1Bitmap1> target;
    int x, y, w, h;
    int scroll_x, scroll_y;
    int disable;
    int wraparound;
    int use_count;
    struct graphics_gc_priv *background_gc;
    struct callback_list *cbl;
    std::vector<struct graphics_priv *> overlays;
};

struct graphics_gc_priv {
    struct graphics_priv *gr;
    D2D1_COLOR_F fg;
    D2D1_COLOR_F bg;
    float width;
    std::vector<float> dashes;
    float dash_offset;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle1> stroke;
};

struct graphics_font_priv {
    ComPtr<IDWriteTextFormat> format;
};

struct graphics_image_priv {
    ComPtr<ID2D1Bitmap> bitmap;
    int w, h;
};

namespace {

struct Surface {
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Bitmap1> backbuffer;
    ComPtr<ID2D1StrokeStyle1> round;
    int w, h;
    float dpi;
};

Surface g;
struct graphics_priv *g_root;
struct graphics_priv *g_current;  // layer whose BeginDraw is open on the device context
struct event_timeout *g_compose_timeout;
struct callback *g_compose_cb;

const float kTextPx = 300.0f / 72.0f / 64.0f;  // Navit font size unit (freetype 26.6 @ 300 dpi) -> pixels

D2D1_COLOR_F ToColor(const struct color *c) {
    return D2D1::ColorF(c->r / 65535.0f, c->g / 65535.0f, c->b / 65535.0f, c->a / 65535.0f);
}

std::wstring Wide(const char *utf8) {
    std::wstring w;
    if (!utf8)
        return w;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0)
        return w;
    w.resize(n - 1);
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    return w;
}

D2D1_BITMAP_PROPERTIES1 TargetProps(D2D1_BITMAP_OPTIONS options) {
    return D2D1::BitmapProperties1(options, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                   96.0f, 96.0f);
}

void EndCurrent() {
    if (!g_current)
        return;
    HRESULT hr = g.ctx->EndDraw();
    if (FAILED(hr))
        dbg(lvl_error, "EndDraw failed 0x%08x", (unsigned)hr);
    g_current = NULL;
}

// Makes gr's bitmap the drawing target, with BeginDraw open.
bool Ensure(struct graphics_priv *gr) {
    if (!gr || !gr->target || !g.ctx)
        return false;
    if (g_current == gr)
        return true;
    EndCurrent();
    g.ctx->SetTarget(gr->target.Get());
    g.ctx->BeginDraw();
    g.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    g_current = gr;
    return true;
}

bool MakeTarget(struct graphics_priv *gr, int w, int h) {
    gr->target.Reset();
    if (w <= 0 || h <= 0)
        return false;
    HRESULT hr = g.ctx->CreateBitmap(D2D1::SizeU(w, h), NULL, 0, TargetProps(D2D1_BITMAP_OPTIONS_TARGET),
                                     gr->target.GetAddressOf());
    if (FAILED(hr)) {
        dbg(lvl_error, "CreateBitmap %dx%d failed 0x%08x", w, h, (unsigned)hr);
        return false;
    }
    gr->w = w;
    gr->h = h;
    return true;
}

bool MakeBackbuffer() {
    g.backbuffer.Reset();
    ComPtr<IDXGISurface> surface;
    HRESULT hr = g.swap->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (SUCCEEDED(hr))
        hr = g.ctx->CreateBitmapFromDxgiSurface(
                 surface.Get(), TargetProps(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW),
                 g.backbuffer.GetAddressOf());
    if (FAILED(hr)) {
        dbg(lvl_error, "backbuffer failed 0x%08x", (unsigned)hr);
        return false;
    }
    return true;
}

void DrawOverlays(struct graphics_priv *gr) {
    for (size_t i = 0; i < gr->overlays.size(); i++) {
        struct graphics_priv *o = gr->overlays[i];
        if (o->disable || !o->target)
            continue;
        float x = (float)(o->x + o->scroll_x), y = (float)(o->y + o->scroll_y);
        g.ctx->DrawBitmap(o->target.Get(), D2D1::RectF(x, y, x + o->w, y + o->h));
        DrawOverlays(o);
    }
}

void Compose() {
    if (!g_root || !g.backbuffer || !g_root->target)
        return;
    EndCurrent();
    g.ctx->SetTarget(g.backbuffer.Get());
    g.ctx->BeginDraw();
    g.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_COLOR_F bg = g_root->background_gc ? g_root->background_gc->fg : D2D1::ColorF(0xFEFEE9);
    g.ctx->Clear(bg);
    float sx = (float)g_root->scroll_x, sy = (float)g_root->scroll_y;
    g.ctx->DrawBitmap(g_root->target.Get(), D2D1::RectF(sx, sy, sx + g_root->w, sy + g_root->h));
    if (!g_root->disable)
        DrawOverlays(g_root);
    HRESULT hr = g.ctx->EndDraw();
    if (FAILED(hr))
        dbg(lvl_error, "compose EndDraw failed 0x%08x", (unsigned)hr);
    hr = g.swap->Present(1, 0);
    if (FAILED(hr))
        dbg(lvl_error, "Present failed 0x%08x", (unsigned)hr);
}

void ComposeTimeout(void *unused) {
    g_compose_timeout = NULL;  // one-shot: the event loop frees it
    Compose();
}

void RequestCompose() {
    if (g_compose_timeout)
        return;
    if (!g_compose_cb)
        g_compose_cb = callback_new_1(callback_cast(ComposeTimeout), NULL);
    g_compose_timeout = event_add_timeout(0, 0, g_compose_cb);
}

// --- gc ---

void GcDestroy(struct graphics_gc_priv *gc) {
    delete gc;
}

void GcSetLinewidth(struct graphics_gc_priv *gc, int w) {
    gc->width = (float)(w > 0 ? w : 1);
}

void GcSetDashes(struct graphics_gc_priv *gc, int w, int offset, unsigned char *dash_list, int n) {
    gc->width = (float)(w > 0 ? w : 1);
    gc->dashes.clear();
    for (int i = 0; i < n; i++)
        gc->dashes.push_back(dash_list[i] / gc->width);
    if (gc->dashes.size() % 2)
        gc->dashes.push_back(gc->dashes.back());
    gc->dash_offset = offset / gc->width;
    gc->stroke.Reset();
    if (!gc->dashes.empty()) {
        D2D1_STROKE_STYLE_PROPERTIES1 p = D2D1::StrokeStyleProperties1(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10.0f,
            D2D1_DASH_STYLE_CUSTOM, gc->dash_offset);
        g.factory->CreateStrokeStyle(p, &gc->dashes[0], (UINT32)gc->dashes.size(), gc->stroke.GetAddressOf());
    }
}

void GcSetForeground(struct graphics_gc_priv *gc, struct color *c) {
    gc->fg = ToColor(c);
    if (gc->brush)
        gc->brush->SetColor(gc->fg);
}

void GcSetBackground(struct graphics_gc_priv *gc, struct color *c) {
    gc->bg = ToColor(c);
}

void GcSetTexture(struct graphics_gc_priv *gc, struct graphics_image_priv *img) {
    // textures are not supported: areas are filled with the plain color
}

struct graphics_gc_methods g_gc_methods = {
    GcDestroy, GcSetLinewidth, GcSetDashes, GcSetForeground, GcSetBackground, GcSetTexture,
};

ID2D1SolidColorBrush *Brush(struct graphics_gc_priv *gc) {
    if (!gc->brush)
        g.ctx->CreateSolidColorBrush(gc->fg, gc->brush.GetAddressOf());
    return gc->brush.Get();
}

struct graphics_gc_priv *GcNew(struct graphics_priv *gr, struct graphics_gc_methods *meth) {
    struct graphics_gc_priv *gc = new graphics_gc_priv;
    gc->gr = gr;
    gc->fg = D2D1::ColorF(0, 0, 0, 1);
    gc->bg = D2D1::ColorF(1, 1, 1, 1);
    gc->width = 1;
    gc->dash_offset = 0;
    *meth = g_gc_methods;
    return gc;
}

// --- fonts and text ---

void FontDestroy(struct graphics_font_priv *font) {
    delete font;
}

struct graphics_font_methods g_font_methods = {FontDestroy};

struct graphics_font_priv *FontNew(struct graphics_priv *gr, struct graphics_font_methods *meth, char *font, int size,
                                   int flags) {
    struct graphics_font_priv *f = new graphics_font_priv;
    float px = size * kTextPx;
    if (px < 6)
        px = 6;
    HRESULT hr = g.dwrite->CreateTextFormat(L"Segoe WP", NULL, flags ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"it-it",
                                            f->format.GetAddressOf());
    if (FAILED(hr)) {
        dbg(lvl_error, "CreateTextFormat failed 0x%08x", (unsigned)hr);
        delete f;
        return NULL;
    }
    f->format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    *meth = g_font_methods;
    return f;
}

ComPtr<IDWriteTextLayout> Layout(struct graphics_font_priv *font, const std::wstring &text, float *baseline) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(g.dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), font->format.Get(), 4096.0f, 4096.0f,
                                          layout.GetAddressOf())))
        return layout;
    DWRITE_LINE_METRICS line;
    UINT32 count = 0;
    *baseline = 0;
    if (SUCCEEDED(layout->GetLineMetrics(&line, 1, &count)) && count)
        *baseline = line.baseline;
    return layout;
}

void DrawLabel(struct graphics_priv *gr, struct graphics_gc_priv *fg, struct graphics_gc_priv *bg,
              struct graphics_font_priv *font, char *text, struct point *p, int dx, int dy) {
    if (!font || !text || !*text || !Ensure(gr))
        return;
    std::wstring w = Wide(text);
    float baseline;
    ComPtr<IDWriteTextLayout> layout = Layout(font, w, &baseline);
    if (!layout)
        return;
    float c = dx / 65536.0f, s = dy / 65536.0f;
    g.ctx->SetTransform(D2D1::Matrix3x2F(c, s, -s, c, (float)p->x, (float)p->y));
    if (bg) {
        static const float off[8][2] = {{-1.5f, 0}, {1.5f, 0}, {0, -1.5f}, {0, 1.5f},
                                        {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        ID2D1SolidColorBrush *halo = Brush(bg);
        for (int i = 0; i < 8; i++)
            g.ctx->DrawTextLayout(D2D1::Point2F(off[i][0], off[i][1] - baseline), layout.Get(), halo);
    }
    g.ctx->DrawTextLayout(D2D1::Point2F(0, -baseline), layout.Get(), Brush(fg));
    g.ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

void GetTextBbox(struct graphics_priv *gr, struct graphics_font_priv *font, char *text, int dx, int dy,
                 struct point *ret, int estimate) {
    float baseline = 0;
    DWRITE_TEXT_METRICS m;
    memset(&m, 0, sizeof(m));
    if (font && text) {
        ComPtr<IDWriteTextLayout> layout = Layout(font, Wide(text), &baseline);
        if (layout)
            layout->GetMetrics(&m);
    }
    int left = 0, right = (int)(m.widthIncludingTrailingWhitespace + 0.5f);
    int top = (int)(-baseline), bottom = (int)(m.height - baseline + 0.5f);
    ret[0].x = left;   // low left
    ret[0].y = bottom;
    ret[1].x = left;   // top left
    ret[1].y = top;
    ret[2].x = right;  // top right
    ret[2].y = top;
    ret[3].x = right;  // low right
    ret[3].y = bottom;
    if (dy != 0 || dx != 0x10000) {
        for (int i = 0; i < 4; i++) {
            struct point pt = ret[i];
            ret[i].x = (pt.x * dx - pt.y * dy) / 0x10000;
            ret[i].y = (pt.y * dx + pt.x * dy) / 0x10000;
        }
    }
}

// --- images ---

void ImageDestroy(struct graphics_image_priv *img) {
    delete img;
}

struct graphics_image_methods g_image_methods = {ImageDestroy};

bool FileExists(const std::wstring &path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) != 0;
}

struct graphics_image_priv *ImageNew(struct graphics_priv *gr, struct graphics_image_methods *meth, char *path, int *w,
                                     int *h, struct point *hot, int rotation) {
    if (!path || !*path)
        return NULL;
    std::wstring file = Wide(path);
    for (size_t i = 0; i < file.size(); i++)
        if (file[i] == L'/')
            file[i] = L'\\';
    size_t dot = file.find_last_of(L'.');
    size_t slash = file.find_last_of(L'\\');
    bool has_ext = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
    if (has_ext && _wcsicmp(file.c_str() + dot, L".svg") == 0)
        file = file.substr(0, dot) + L".png";  // icons are converted to PNG on the PC
    else if (!has_ext)
        file += L".png";
    if (!FileExists(file))
        return NULL;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    HRESULT hr = g.wic->CreateDecoderFromFilename(file.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  decoder.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, frame.GetAddressOf());
    UINT fw = 0, fh = 0;
    if (SUCCEEDED(hr))
        hr = frame->GetSize(&fw, &fh);
    ComPtr<IWICBitmapSource> source = frame;
    int tw = (*w > 0) ? *w : (int)fw, th = (*h > 0) ? *h : (int)fh;
    if (SUCCEEDED(hr) && (tw != (int)fw || th != (int)fh)) {
        ComPtr<IWICBitmapScaler> scaler;
        hr = g.wic->CreateBitmapScaler(scaler.GetAddressOf());
        if (SUCCEEDED(hr))
            hr = scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant);
        if (SUCCEEDED(hr))
            source = scaler;
    }
    if (SUCCEEDED(hr))
        hr = g.wic->CreateFormatConverter(conv.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = conv->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0,
                              WICBitmapPaletteTypeMedianCut);
    struct graphics_image_priv *img = NULL;
    if (SUCCEEDED(hr)) {
        img = new graphics_image_priv;
        hr = g.ctx->CreateBitmapFromWicBitmap(conv.Get(), NULL, img->bitmap.GetAddressOf());
        if (FAILED(hr)) {
            delete img;
            img = NULL;
        }
    }
    if (!img) {
        dbg(lvl_warning, "image %s failed 0x%08x", path, (unsigned)hr);
        return NULL;
    }
    img->w = tw;
    img->h = th;
    *w = tw;
    *h = th;
    if (hot) {
        hot->x = tw / 2;
        hot->y = th / 2;
    }
    *meth = g_image_methods;
    return img;
}

void ImageFree(struct graphics_priv *gr, struct graphics_image_priv *img) {
    delete img;
}

void DrawImage(struct graphics_priv *gr, struct graphics_gc_priv *fg, struct point *p, struct graphics_image_priv *img) {
    if (!img || !Ensure(gr))
        return;
    g.ctx->DrawBitmap(img->bitmap.Get(), D2D1::RectF((float)p->x, (float)p->y, (float)(p->x + img->w),
                                                     (float)(p->y + img->h)));
}

// --- primitives ---

ComPtr<ID2D1PathGeometry> Path(struct point *p, int count, bool closed, int hole_count, int *ccount,
                               struct point **holes) {
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(g.factory->CreatePathGeometry(geo.GetAddressOf())) || FAILED(geo->Open(sink.GetAddressOf())))
        return ComPtr<ID2D1PathGeometry>();
    sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
    D2D1_FIGURE_BEGIN begin = closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW;
    D2D1_FIGURE_END end = closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN;
    sink->BeginFigure(D2D1::Point2F((float)p[0].x, (float)p[0].y), begin);
    for (int i = 1; i < count; i++)
        sink->AddLine(D2D1::Point2F((float)p[i].x, (float)p[i].y));
    sink->EndFigure(end);
    for (int j = 0; j < hole_count; j++) {
        if (ccount[j] < 3)
            continue;
        sink->BeginFigure(D2D1::Point2F((float)holes[j][0].x, (float)holes[j][0].y), begin);
        for (int i = 1; i < ccount[j]; i++)
            sink->AddLine(D2D1::Point2F((float)holes[j][i].x, (float)holes[j][i].y));
        sink->EndFigure(end);
    }
    sink->Close();
    return geo;
}

void DrawLines(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count) {
    if (count < 2 || !Ensure(gr))
        return;
    ID2D1SolidColorBrush *brush = Brush(gc);
    if (gc->stroke) {
        ComPtr<ID2D1PathGeometry> geo = Path(p, count, false, 0, NULL, NULL);
        if (geo)
            g.ctx->DrawGeometry(geo.Get(), brush, gc->width, gc->stroke.Get());
        return;
    }
    // separate segments with round caps: cheaper than a path per polyline and the caps make round joins
    for (int i = 0; i + 1 < count; i++)
        g.ctx->DrawLine(D2D1::Point2F((float)p[i].x, (float)p[i].y), D2D1::Point2F((float)p[i + 1].x,
                        (float)p[i + 1].y), brush, gc->width, g.round.Get());
}

void DrawPolygon(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count) {
    if (count < 3 || !Ensure(gr))
        return;
    ComPtr<ID2D1PathGeometry> geo = Path(p, count, true, 0, NULL, NULL);
    if (geo)
        g.ctx->FillGeometry(geo.Get(), Brush(gc));
}

void DrawPolygonWithHoles(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int count,
                          int hole_count, int *ccount, struct point **holes) {
    if (count < 3 || !Ensure(gr))
        return;
    ComPtr<ID2D1PathGeometry> geo = Path(p, count, true, hole_count, ccount, holes);
    if (geo)
        g.ctx->FillGeometry(geo.Get(), Brush(gc));
}

void DrawRectangle(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int w, int h) {
    if (!Ensure(gr))
        return;
    g.ctx->FillRectangle(D2D1::RectF((float)p->x, (float)p->y, (float)(p->x + w), (float)(p->y + h)), Brush(gc));
}

void DrawCircle(struct graphics_priv *gr, struct graphics_gc_priv *gc, struct point *p, int r) {
    if (!Ensure(gr))
        return;
    g.ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((float)p->x, (float)p->y), r / 2.0f, r / 2.0f), Brush(gc),
                       gc->width);
}

// --- layers ---

void DrawDrag(struct graphics_priv *gr, struct point *p) {
    struct point v = {0, 0};
    if (p)
        v = *p;
    if (gr->root) {
        gr->scroll_x = v.x;
        gr->scroll_y = v.y;
        Compose();
    } else {
        gr->x = v.x;
        gr->y = v.y;
        RequestCompose();
    }
}

void BackgroundGc(struct graphics_priv *gr, struct graphics_gc_priv *gc) {
    gr->background_gc = gc;
}

void DrawMode(struct graphics_priv *gr, enum draw_mode_num mode) {
    switch (mode) {
    case draw_mode_begin_clear:
    case draw_mode_begin:
        gr->use_count++;
        if (Ensure(gr) && (mode == draw_mode_begin_clear || !gr->root) && gr->use_count == 1)
            g.ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
        break;
    case draw_mode_end:
        if (--gr->use_count < 0)
            gr->use_count = 0;
        if (gr->use_count == 0) {
            if (g_current == gr)
                EndCurrent();
            RequestCompose();
        }
        break;
    }
}

void OverlayDisable(struct graphics_priv *gr, int disable) {
    gr->disable = disable;
    RequestCompose();
}

void Place(struct graphics_priv *gr, struct point *p, int wraparound) {
    gr->x = p->x;
    gr->y = p->y;
    if (wraparound && g_root) {
        if (gr->x < 0)
            gr->x += g_root->w;
        if (gr->y < 0)
            gr->y += g_root->h;
    }
}

void OverlayResize(struct graphics_priv *gr, struct point *p, int w, int h, int wraparound) {
    Place(gr, p, wraparound);
    if (gr->w != w || gr->h != h) {
        if (g_current == gr)
            EndCurrent();
        MakeTarget(gr, w, h);
    }
    RequestCompose();
}

int FullscreenNoop(struct window *win, int on) {
    return 1;
}

void DisableSuspend(struct window *win) {
    if (navit_wp81_keep_screen_on_hook)
        navit_wp81_keep_screen_on_hook();
}

void *GetData(struct graphics_priv *gr, const char *type) {
    if (!strcmp(type, "window")) {
        struct window *win = g_new0(struct window, 1);
        win->priv = gr;
        win->fullscreen = FullscreenNoop;
        win->disable_suspend = DisableSuspend;
        callback_list_call_attr_2(gr->cbl, attr_resize, GINT_TO_POINTER(gr->w), GINT_TO_POINTER(gr->h));
        return win;
    }
    return NULL;
}

navit_float GetDpi(struct graphics_priv *gr) {
    return (navit_float)g.dpi;
}

void Destroy(struct graphics_priv *gr) {
    if (g_current == gr)
        EndCurrent();
    if (gr->parent) {
        std::vector<struct graphics_priv *> &v = gr->parent->overlays;
        for (size_t i = 0; i < v.size(); i++)
            if (v[i] == gr) {
                v.erase(v.begin() + i);
                break;
            }
    }
    if (gr == g_root)
        g_root = NULL;
    delete gr;
}

struct graphics_priv *OverlayNew(struct graphics_priv *gr, struct graphics_methods *meth, struct point *p, int w,
                                 int h, int wraparound);

struct graphics_methods g_methods = {
    Destroy, DrawMode, DrawLines, DrawPolygon, DrawRectangle, DrawCircle, DrawLabel, DrawImage,
    NULL,  // draw_image_warp
    DrawDrag, FontNew, GcNew, BackgroundGc, OverlayNew, ImageNew, GetData, ImageFree, GetTextBbox, OverlayDisable,
    OverlayResize,
    NULL,  // set_attr
    NULL,  // show_native_keyboard
    NULL,  // hide_native_keyboard
    GetDpi, DrawPolygonWithHoles,
};

struct graphics_priv *NewPriv(bool root, struct graphics_priv *parent, struct callback_list *cbl) {
    struct graphics_priv *gr = new graphics_priv;
    gr->root = root;
    gr->parent = parent;
    gr->x = gr->y = gr->w = gr->h = 0;
    gr->scroll_x = gr->scroll_y = 0;
    gr->disable = 0;
    gr->wraparound = 0;
    gr->use_count = 0;
    gr->background_gc = NULL;
    gr->cbl = cbl;
    return gr;
}

struct graphics_priv *OverlayNew(struct graphics_priv *gr, struct graphics_methods *meth, struct point *p, int w,
                                 int h, int wraparound) {
    struct graphics_priv *o = NewPriv(false, gr, gr->cbl);
    *meth = g_methods;
    o->wraparound = wraparound;
    Place(o, p, wraparound);
    MakeTarget(o, w, h);
    gr->overlays.push_back(o);
    return o;
}

// Without Navit's internal GUI nobody asks for the "window", so tell Navit the size once the event loop runs
// (by then navit_set_graphics has added its resize callback).
void AnnounceSize(struct graphics_priv *gr) {
    if (gr == g_root)
        callback_list_call_attr_2(gr->cbl, attr_resize, GINT_TO_POINTER(gr->w), GINT_TO_POINTER(gr->h));
}

struct graphics_priv *GraphicsNew(struct navit *nav, struct graphics_methods *meth, struct attr **attrs,
                                  struct callback_list *cbl) {
    if (!g.ctx || !g.swap) {
        dbg(lvl_error, "no drawing surface: navit_wp81_gfx_init was not called");
        return NULL;
    }
    if (!event_request_system("wp81", "graphics_d2d"))
        return NULL;
    *meth = g_methods;
    struct graphics_priv *gr = NewPriv(true, NULL, cbl);
    if (!MakeBackbuffer() || !MakeTarget(gr, g.w, g.h)) {
        delete gr;
        return NULL;
    }
    g_root = gr;
    event_add_timeout(0, 0, callback_new_1(callback_cast(AnnounceSize), gr));
    return gr;
}

struct ResizeTask {
    int w, h;
};

void ResizeOnNavit(void *data) {
    ResizeTask *t = (ResizeTask *)data;
    int w = t->w, h = t->h;
    delete t;
    if (w <= 0 || h <= 0 || (w == g.w && h == g.h && g_root && g_root->w == w))
        return;
    EndCurrent();
    g.ctx->SetTarget(NULL);
    g.backbuffer.Reset();
    HRESULT hr = g.swap->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) {
        dbg(lvl_error, "ResizeBuffers failed 0x%08x", (unsigned)hr);
        return;
    }
    g.w = w;
    g.h = h;
    MakeBackbuffer();
    if (g_root) {
        MakeTarget(g_root, w, h);
        callback_list_call_attr_2(g_root->cbl, attr_resize, GINT_TO_POINTER(w), GINT_TO_POINTER(h));
    }
}

struct InputTask {
    int kind;  // 0 button, 1 motion
    int pressed, button;
    struct point p;
};

void InputOnNavit(void *data) {
    InputTask *t = (InputTask *)data;
    if (g_root) {
        if (t->kind == 0)
            callback_list_call_attr_3(g_root->cbl, attr_button, GINT_TO_POINTER(t->pressed), GINT_TO_POINTER(t->button),
                                      (void *)&t->p);
        else
            callback_list_call_attr_1(g_root->cbl, attr_motion, (void *)&t->p);
    }
    delete t;
}

}  // namespace

extern "C" {

void navit_wp81_gfx_init(void *d2d_context, void *swap_chain, void *dwrite, void *wic, int width, int height,
                         float dpi) {
    g.ctx = (ID2D1DeviceContext *)d2d_context;
    g.swap = (IDXGISwapChain1 *)swap_chain;
    g.dwrite = (IDWriteFactory *)dwrite;
    g.wic = (IWICImagingFactory *)wic;
    g.w = width;
    g.h = height;
    g.dpi = dpi;
    ComPtr<ID2D1Factory> f;
    g.ctx->GetFactory(f.GetAddressOf());
    f.As(&g.factory);
    D2D1_STROKE_STYLE_PROPERTIES1 p = D2D1::StrokeStyleProperties1(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    g.factory->CreateStrokeStyle(p, NULL, 0, g.round.GetAddressOf());
}

void navit_wp81_gfx_resize(int width, int height) {
    ResizeTask *t = new ResizeTask;
    t->w = width;
    t->h = height;
    navit_wp81_post(ResizeOnNavit, t);
}

void navit_wp81_input_button(int pressed, int button, int x, int y) {
    InputTask *t = new InputTask;
    t->kind = 0;
    t->pressed = pressed;
    t->button = button;
    t->p.x = x;
    t->p.y = y;
    navit_wp81_post(InputOnNavit, t);
}

void navit_wp81_input_motion(int x, int y) {
    InputTask *t = new InputTask;
    t->kind = 1;
    t->pressed = 0;
    t->button = 0;
    t->p.x = x;
    t->p.y = y;
    navit_wp81_post(InputOnNavit, t);
}

unsigned char *navit_wp81_gfx_capture(int *width, int *height) {
    if (!g_root || !g_root->target)
        return NULL;
    EndCurrent();
    int w = g_root->w, h = g_root->h;
    ComPtr<ID2D1Bitmap1> cpu;
    HRESULT hr = g.ctx->CreateBitmap(D2D1::SizeU(w, h), NULL, 0,
                                     TargetProps(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW),
                                     cpu.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = cpu->CopyFromBitmap(NULL, g_root->target.Get(), NULL);
    D2D1_MAPPED_RECT m;
    if (SUCCEEDED(hr))
        hr = cpu->Map(D2D1_MAP_OPTIONS_READ, &m);
    if (FAILED(hr)) {
        dbg(lvl_error, "capture failed 0x%08x", (unsigned)hr);
        return NULL;
    }
    unsigned char *out = (unsigned char *)malloc((size_t)w * h * 4);
    if (out)
        for (int y = 0; y < h; y++)
            memcpy(out + (size_t)y * w * 4, m.bits + (size_t)y * m.pitch, (size_t)w * 4);
    cpu->Unmap();
    *width = w;
    *height = h;
    return out;
}

void module_graphics_d2d_init(void) {
    plugin_register_category_graphics("d2d", GraphicsNew);
}

}  // extern "C"
