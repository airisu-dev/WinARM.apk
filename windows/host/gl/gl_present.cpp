// gl_present.cpp -- see gl_present.h.

#include "gl/gl_present.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <d3d11_4.h>
#include <dcomp.h>
#include <dxgi1_3.h>

#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "gl/gl_surface.h"
#include "win32_window.h"

namespace winapk::host {

namespace {

constexpr EGLint kDeviceExt = 0x322C, kD3D11Device = 0x33A1;  // EGL_DEVICE_EXT, EGL_D3D11_DEVICE_ANGLE

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

struct Layer {
    IDXGISwapChain1* chain = nullptr;
    IDCompositionVisual* visual = nullptr;
    int w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool bottom_up = false;  // GL's row order
    RECT placed{};
    int shown_w = 0, shown_h = 0;  // placed as a frame of this size (0: its own)
    // Signalled when Present would not block (frame latency 1); waited on outside g_lock.
    HANDLE ready = nullptr;
    uint64_t id = 0;           // the chain `ready` belongs to
    int credits = 0;           // counts of `ready` waited for, not yet spent on a Present
    bool owed = false;         // the back buffer holds a frame not yet presented
    ULONGLONG owed_since = 0;  // GetTickCount64
};

// A stalled GPU must not hold the render threads: past this the frame is owed.
constexpr DWORD kReadyWaitMs = 100;
// An owed frame older than this is presented anyway: the waitable object is only a throttle.
constexpr ULONGLONG kOwedMaxMs = 1000;

std::mutex g_lock;
uint64_t g_chain_ids = 0;
bool g_tried = false, g_active = false;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGIFactory2* g_factory = nullptr;
IDCompositionDevice* g_dcomp = nullptr;
IDCompositionTarget* g_target = nullptr;
IDCompositionVisual* g_root = nullptr;
Layer g_gl{nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, true, {}}, g_tree;
RECT g_clip{};
int g_mirror = 0;  // the client width the root is mirrored across
// The textures opened for pbuffer share handles; gl_present_forget drops one, the cap in source() bounds the rest.
std::vector<std::pair<void*, ID3D11Texture2D*>> g_sources;
std::vector<uint32_t> g_premul, g_tree_last;
ID3D11Texture2D* g_tree_tex = nullptr;
int g_tree_tex_w = 0, g_tree_tex_h = 0;
IDCompositionVisual *g_below = nullptr, *g_above = nullptr;  // surface layers under / over the tree
struct Surface {
    Layer layer;
    bool on_top = false;
    bool attached = false;
};
std::map<int, Surface> g_surfaces;
bool g_gl_base = false;  // the GL layer is gl_present_base's black

bool fail(const char* what, HRESULT hr = S_OK) {
    std::fprintf(stderr, "[win.apk] GL present: %s (0x%08lx); frames are read back\n", what,
                 static_cast<unsigned long>(hr));
    return false;
}

void drop_chain(Layer& l) {
    release(l.chain);
    if (l.ready) CloseHandle(l.ready);
    l.ready = nullptr;
    l.credits = 0;
    l.owed = false;
}

void release_all();

// Once, under g_lock. A failure releases all it made: it is not tried again.
bool setup() {
    if (g_tried) return g_dcomp != nullptr;
    g_tried = true;
    HWND hwnd = static_cast<HWND>(window_handle());
    EGLDisplay display = gl_shared_display();
    if (!hwnd || !display) return fail("no window or display");
    auto query_display =
        reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(eglGetProcAddress("eglQueryDisplayAttribEXT"));
    auto query_device = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(eglGetProcAddress("eglQueryDeviceAttribEXT"));
    EGLAttrib device = 0, d3d = 0;
    if (!query_display || !query_device || !query_display(display, kDeviceExt, &device) ||
        !query_device(reinterpret_cast<EGLDeviceEXT>(device), kD3D11Device, &d3d) || !d3d) {
        return fail("ANGLE is not on Direct3D 11");
    }
    g_device = reinterpret_cast<ID3D11Device*>(d3d);
    g_device->AddRef();
    g_device->GetImmediateContext(&g_ctx);
    // The view tree's layer is written from another thread than ANGLE's.
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(g_ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    HRESULT hr = g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) {
        // WARP (Microsoft Basic Render Driver) copies on the CPU anyway.
        DXGI_ADAPTER_DESC desc{};
        if (!gl_present_forced() && SUCCEEDED(adapter->GetDesc(&desc)) && desc.VendorId == 0x1414 &&
            desc.DeviceId == 0x8c) {
            release(adapter);
            release(dxgi);
            release_all();
            return fail("a software renderer");
        }
        hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&g_factory));
    }
    if (SUCCEEDED(hr)) {
        hr = DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&g_dcomp));
    }
    release(adapter);
    release(dxgi);
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateTargetForHwnd(hwnd, FALSE, &g_target);
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateVisual(&g_root);
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateVisual(&g_gl.visual);
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateVisual(&g_tree.visual);
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateVisual(&g_below);
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateVisual(&g_above);
    if (SUCCEEDED(hr)) hr = g_root->AddVisual(g_gl.visual, TRUE, nullptr);
    // Bottom to top, as AOSP stacks a window.
    if (SUCCEEDED(hr)) hr = g_root->AddVisual(g_below, TRUE, g_gl.visual);
    if (SUCCEEDED(hr)) hr = g_root->AddVisual(g_tree.visual, TRUE, g_below);
    if (SUCCEEDED(hr)) hr = g_root->AddVisual(g_above, TRUE, g_tree.visual);
    if (SUCCEEDED(hr)) hr = g_target->SetRoot(g_root);
    if (FAILED(hr)) {
        release_all();
        return fail("DirectComposition", hr);
    }
    g_gl.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    g_tree.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    return true;
}

bool ensure_chain(Layer& l, int w, int h, DXGI_FORMAT format, bool alpha, bool* changed) {
    if (l.chain && l.w == w && l.h == h && l.format == format) return true;
    // ResizeBuffers must keep the chain's flags.
    constexpr UINT kFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (l.chain && l.format == format && SUCCEEDED(l.chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, kFlags))) {
        l.placed = RECT{};
        l.w = w;
        l.h = h;
        l.owed = false;  // the buffers are new; the caller writes one
        return true;
    }
    drop_chain(l);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = alpha ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = kFlags;
    HRESULT hr = g_factory->CreateSwapChainForComposition(g_device, &desc, nullptr, &l.chain);
    if (FAILED(hr)) return fail("CreateSwapChainForComposition", hr);
    IDXGISwapChain2* chain2 = nullptr;
    if (SUCCEEDED(l.chain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&chain2)))) {
        chain2->SetMaximumFrameLatency(1);
        l.ready = chain2->GetFrameLatencyWaitableObject();
        chain2->Release();
    }
    l.id = ++g_chain_ids;
    l.visual->SetContent(l.chain);
    l.w = w;
    l.h = h;
    l.format = format;
    l.placed = RECT{};
    *changed = true;
    return true;
}

void place(Layer& l, bool* changed) {
    int d[4], b[4];
    const int sw = l.shown_w > 0 ? l.shown_w : l.w, sh = l.shown_h > 0 ? l.shown_h : l.h;
    if (!window_layer_rect(sw, sh, d, b)) return;
    const RECT dst{d[0], d[1], d[2], d[3]}, box{b[0], b[1], b[2], b[3]};
    // A mirrored (RTL) window mirrors its layers; the root mirrors them back.
    int cw = 0;
    const int mirror = window_mirrored(&cw) ? cw : 0;
    if (mirror != g_mirror) {
        g_mirror = mirror;
        D2D_MATRIX_3X2_F m{};
        m._11 = mirror ? -1.0f : 1.0f;
        m._22 = 1.0f;
        m._31 = static_cast<float>(mirror);
        g_root->SetTransform(m);
        *changed = true;
    }
    if (std::memcmp(&box, &g_clip, sizeof box) != 0) {
        g_clip = box;
        g_root->SetClip(D2D_RECT_F{static_cast<float>(box.left), static_cast<float>(box.top),
                                   static_cast<float>(box.right), static_cast<float>(box.bottom)});
        *changed = true;
    }
    if (std::memcmp(&dst, &l.placed, sizeof dst) == 0) return;
    l.placed = dst;
    D2D_MATRIX_3X2_F m{};
    m._11 = static_cast<float>(dst.right - dst.left) / static_cast<float>(l.w);
    const float sy = static_cast<float>(dst.bottom - dst.top) / static_cast<float>(l.h);
    m._22 = l.bottom_up ? -sy : sy;
    m._31 = static_cast<float>(dst.left);
    m._32 = static_cast<float>(l.bottom_up ? dst.bottom : dst.top);
    l.visual->SetTransform(m);
    *changed = true;
}

ID3D11Texture2D* source(void* share) {
    for (auto& [handle, tex] : g_sources) {
        if (handle == share) return tex;
    }
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = g_device->OpenSharedResource(share, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    if (FAILED(hr)) {
        fail("OpenSharedResource", hr);
        return nullptr;
    }
    if (g_sources.size() >= 4) {
        g_sources.front().second->Release();
        g_sources.erase(g_sources.begin());
    }
    g_sources.emplace_back(share, tex);
    return tex;
}

void activate() {
    if (g_active) return;
    g_active = true;
    std::fprintf(stderr, "[win.apk] GL present: frames go to the window on the GPU (DirectComposition)\n");
}

void drop_surface(std::map<int, Surface>::iterator it) {
    Surface& s = it->second;
    if (s.attached) (s.on_top ? g_above : g_below)->RemoveVisual(s.layer.visual);
    drop_chain(s.layer);
    release(s.layer.visual);
    g_surfaces.erase(it);
}

void stop_locked() {
    if (!g_active) return;
    g_active = false;
    g_gl.visual->SetContent(nullptr);
    g_tree.visual->SetContent(nullptr);
    drop_chain(g_gl);
    drop_chain(g_tree);
    release(g_tree_tex);
    g_tree_last.clear();
    while (!g_surfaces.empty()) drop_surface(g_surfaces.begin());
    g_gl_base = false;
    g_gl.w = g_gl.h = g_tree.w = g_tree.h = 0;
    g_dcomp->Commit();
}

// Everything setup() and the frames made; safe on a partly done setup().
void release_all() {
    g_active = false;
    while (!g_surfaces.empty()) drop_surface(g_surfaces.begin());
    for (auto& [handle, tex] : g_sources) tex->Release();
    g_sources.clear();
    drop_chain(g_gl);
    drop_chain(g_tree);
    release(g_tree_tex);
    g_tree_last.clear();
    g_gl_base = false;
    g_gl.w = g_gl.h = g_tree.w = g_tree.h = 0;
    if (g_target) g_target->SetRoot(nullptr);
    if (g_dcomp) g_dcomp->Commit();
    release(g_above);
    release(g_below);
    release(g_tree.visual);
    release(g_gl.visual);
    release(g_root);
    release(g_target);
    release(g_dcomp);
    release(g_factory);
    release(g_ctx);
    release(g_device);
}

// A lost device (a driver reset) is not made again: the rest of the run is read back.
void lose(HRESULT hr) {
    fail("the GPU device was lost", hr);
    release_all();
}

// False when the frame did not reach the window: the caller reads it back.
bool present(IDXGISwapChain1* chain) {
    const HRESULT hr = chain->Present(0, 0);
    if (SUCCEEDED(hr)) return true;  // DXGI_STATUS_OCCLUDED and the like too
    const HRESULT reason = g_device->GetDeviceRemovedReason();
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || FAILED(reason)) {
        lose(FAILED(reason) ? reason : hr);
        return false;
    }
    static bool told = false;
    if (!told) fail("Present", hr);
    told = true;
    return false;
}

// await_ready's answer: chain `id` can take a frame (ok), or the wait timed out.
struct Ready {
    uint64_t id = 0;
    bool ok = false;
};

// Waits (bounded, without g_lock) on a duplicate of the layer's handle, so the
// chain may be released meanwhile. `find` runs under g_lock. No wait when the
// layer already holds a count.
template <typename Find>
Ready await_ready(Find find) {
    Ready r;
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const Layer* l = find();
        if (!l || !l->ready || l->credits > 0) return r;
        if (!DuplicateHandle(GetCurrentProcess(), l->ready, GetCurrentProcess(), &h, SYNCHRONIZE, FALSE, 0)) return r;
        r.id = l->id;
    }
    r.ok = WaitForSingleObject(h, kReadyWaitMs) == WAIT_OBJECT_0;
    CloseHandle(h);
    return r;
}

// Right after taking g_lock: keeps the count a wait took until a Present spends
// it, so an early return does not lose it.
void bank(Layer* l, const Ready& r) {
    if (l && r.ok && r.id == l->id && l->chain) ++l->credits;
}

// Presents the back buffer, or owes it while the chain is full; false only when
// Present failed. Each Present spends one count (a new chain starts with one),
// except an owed frame past kOwedMaxMs.
bool show(Layer& l) {
    const ULONGLONG now = GetTickCount64();
    bool go = !l.ready;
    if (!go && l.credits > 0) {
        --l.credits;
        go = true;
    }
    if (!go) go = WaitForSingleObject(l.ready, 0) == WAIT_OBJECT_0 || (l.owed && now - l.owed_since > kOwedMaxMs);
    if (!go) {
        if (!l.owed) l.owed_since = now;
        l.owed = true;
        return true;
    }
    l.owed = false;
    return present(l.chain);
}

}  // namespace

// WINAPK_GL_PRESENT=readback turns the GPU layers off, =on on (the launcher
// sets either from an app's own choice, library\<name>.gpu), =gpu on even on
// a software renderer; otherwise the Settings switch (GpuPresent, default on).
// On a software renderer they stay off unless forced (setup).
bool gl_present_forced() {
    static const bool yes = [] {
        const char* e = std::getenv("WINAPK_GL_PRESENT");
        return e && std::strcmp(e, "gpu") == 0;
    }();
    return yes;
}

bool gl_present_wanted() {
    static const bool on = [] {
        const char* e = std::getenv("WINAPK_GL_PRESENT");
        if (e && std::strcmp(e, "readback") == 0) return false;
        if (e && (std::strcmp(e, "on") == 0 || std::strcmp(e, "gpu") == 0)) return true;
        DWORD v = 1, size = sizeof v;
        return !(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Win.apk", L"GpuPresent", RRF_RT_REG_DWORD, nullptr,
                              &v, &size) == ERROR_SUCCESS &&
                 v == 0);
    }();
    return on;
}

bool gl_present_active() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_active;
}

bool gl_present_frame(void* share, int w, int h, int shown_w, int shown_h) {
    // Without a current context glFlush does nothing and the copy may read a half-drawn frame.
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        static bool told = false;
        if (!told) std::fprintf(stderr, "[win.apk] GL present: no current context; frames are read back\n");
        told = true;
        return false;
    }
    const Ready ready = await_ready([] { return &g_gl; });
    std::lock_guard<std::mutex> hold(g_lock);
    bank(&g_gl, ready);
    if (!share || w <= 0 || h <= 0 || !setup()) return false;
    ID3D11Texture2D* src = source(share);
    if (!src) return false;
    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        static bool told = false;
        if (!told) fail("the pbuffer's format", desc.Format);
        told = true;
        return false;
    }
    if (static_cast<UINT>(w) > desc.Width || static_cast<UINT>(h) > desc.Height) return false;
    bool changed = false;
    if (!ensure_chain(g_gl, w, h, desc.Format, false, &changed)) return false;
    glFlush();
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_gl.chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
    if (desc.Width == static_cast<UINT>(w) && desc.Height == static_cast<UINT>(h)) {
        g_ctx->CopyResource(back, src);
    } else {
        // GL's rows are bottom-up: the frame is the texture's first h rows.
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(w), static_cast<UINT>(h), 1};
        g_ctx->CopySubresourceRegion(back, 0, 0, 0, 0, src, 0, &box);
    }
    back->Release();
    if (!show(g_gl)) return false;
    g_gl_base = false;
    activate();
    if (g_gl.shown_w != shown_w || g_gl.shown_h != shown_h) {
        g_gl.shown_w = shown_w;
        g_gl.shown_h = shown_h;
        g_gl.placed = RECT{};
    }
    place(g_gl, &changed);
    if (g_tree.chain) place(g_tree, &changed);
    if (changed) g_dcomp->Commit();
    return true;
}

void gl_present_tree_clear() {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_tree.chain) return;
    g_tree.visual->SetContent(nullptr);
    drop_chain(g_tree);
    release(g_tree_tex);
    g_tree_last.clear();
    g_tree.w = g_tree.h = 0;
    g_dcomp->Commit();
}

// Uploads only the rectangle that changed, into a texture the buffer is copied from.
bool gl_present_tree(const uint32_t* argb, int w, int h) {
    const Ready ready = await_ready([] { return &g_tree; });
    std::lock_guard<std::mutex> hold(g_lock);
    bank(&g_tree, ready);
    if (!g_active || w <= 0 || h <= 0) return false;
    bool changed = false;
    if (!ensure_chain(g_tree, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, true, &changed)) return false;
    const size_t n = static_cast<size_t>(w) * h;
    int l = 0, t = 0, r = w, b = h;
    if (!g_tree_tex || g_tree_tex_w != w || g_tree_tex_h != h || changed) {
        release(g_tree_tex);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_tree_tex))) return false;
        g_tree_tex_w = w;
        g_tree_tex_h = h;
        g_tree_last.assign(argb, argb + n);
    } else {
        const size_t row = static_cast<size_t>(w) * sizeof(uint32_t);
        auto same = [&](int y) {
            const size_t at = static_cast<size_t>(y) * w;
            return std::memcmp(argb + at, g_tree_last.data() + at, row) == 0;
        };
        while (t < h && same(t)) ++t;
        if (t == h) {
            // The window may have moved since; a failed show may have released everything.
            if (g_tree.owed && !show(g_tree)) return false;
            place(g_tree, &changed);
            if (changed) g_dcomp->Commit();
            return true;
        }
        while (same(b - 1)) --b;
        l = w;
        r = 0;
        for (int y = t; y < b; ++y) {
            const uint32_t* now = argb + static_cast<size_t>(y) * w;
            const uint32_t* was = g_tree_last.data() + static_cast<size_t>(y) * w;
            int x = 0;
            while (x < l && now[x] == was[x]) ++x;
            l = x;
            x = w;
            while (x > r && now[x - 1] == was[x - 1]) --x;
            r = x;
            std::memcpy(g_tree_last.data() + static_cast<size_t>(y) * w, now, row);
        }
    }
    const int rw = r - l, rh = b - t;
    g_premul.resize(static_cast<size_t>(rw) * rh);
    for (int y = 0; y < rh; ++y) {
        const uint32_t* src = argb + static_cast<size_t>(t + y) * w + l;
        uint32_t* dst = g_premul.data() + static_cast<size_t>(y) * rw;
        for (int x = 0; x < rw; ++x) {
            const uint32_t p = src[x], a = p >> 24;
            if (a == 255 || a == 0) {
                dst[x] = a ? p : 0;
                continue;
            }
            uint32_t out = a << 24;
            for (int sh = 0; sh <= 16; sh += 8) out |= ((((p >> sh) & 0xff) * a + 127) / 255) << sh;
            dst[x] = out;
        }
    }
    const D3D11_BOX box{static_cast<UINT>(l), static_cast<UINT>(t), 0, static_cast<UINT>(r), static_cast<UINT>(b), 1};
    g_ctx->UpdateSubresource(g_tree_tex, 0, &box, g_premul.data(), static_cast<UINT>(rw) * 4, 0);
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_tree.chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
    g_ctx->CopyResource(back, g_tree_tex);
    back->Release();
    if (!show(g_tree)) return false;
    place(g_tree, &changed);
    if (changed) g_dcomp->Commit();
    return true;
}

bool gl_present_base(int w, int h) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (w <= 0 || h <= 0 || !setup()) return false;
    bool changed = false;
    if (!g_gl_base || !g_gl.chain) {
        if (!ensure_chain(g_gl, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, false, &changed)) return false;
        ID3D11Texture2D* back = nullptr;
        if (FAILED(g_gl.chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
        const uint32_t black = 0xff000000u;
        g_ctx->UpdateSubresource(back, 0, nullptr, &black, 4, 0);
        back->Release();
        if (!show(g_gl)) return false;
        g_gl_base = true;
    }
    activate();
    if (g_gl.shown_w != w || g_gl.shown_h != h) {
        g_gl.shown_w = w;
        g_gl.shown_h = h;
        g_gl.placed = RECT{};
    }
    place(g_gl, &changed);
    if (g_tree.chain) place(g_tree, &changed);
    if (changed) g_dcomp->Commit();
    return true;
}

bool gl_present_has_surface(int key) {
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_surfaces.find(key);
    return it != g_surfaces.end() && it->second.layer.chain;
}

bool gl_present_surface(int key, const uint32_t* argb, int w, int h) {
    const Ready ready = await_ready([key]() -> const Layer* {
        auto it = g_surfaces.find(key);
        return it == g_surfaces.end() ? nullptr : &it->second.layer;
    });
    std::lock_guard<std::mutex> hold(g_lock);
    if (auto it = g_surfaces.find(key); it != g_surfaces.end()) bank(&it->second.layer, ready);
    if (!g_active || w <= 0 || h <= 0) return false;
    Surface& s = g_surfaces[key];
    if (!s.layer.visual) {
        if (FAILED(g_dcomp->CreateVisual(&s.layer.visual))) {
            g_surfaces.erase(key);
            return false;
        }
        s.layer.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    bool changed = false;
    if (!ensure_chain(s.layer, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, false, &changed)) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(s.layer.chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
    g_ctx->UpdateSubresource(back, 0, nullptr, argb, static_cast<UINT>(w) * 4, 0);
    back->Release();
    if (!show(s.layer)) return false;
    if (changed) g_dcomp->Commit();
    return true;
}

void gl_present_surfaces_place(const SurfacePlace* places, int n, int tree_w, int tree_h) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_active) return;
    bool changed = false;
    for (auto it = g_surfaces.begin(); it != g_surfaces.end();) {
        bool listed = false;
        for (int i = 0; i < n && !listed; ++i) listed = places[i].key == it->first;
        if (listed) {
            ++it;
            continue;
        }
        drop_surface(it++);
        changed = true;
    }
    int d[4], b[4];
    if (tree_w <= 0 || tree_h <= 0 || !window_layer_rect(tree_w, tree_h, d, b)) {
        // Dropped visuals stay on screen until a commit.
        if (changed) g_dcomp->Commit();
        return;
    }
    const float sx = static_cast<float>(d[2] - d[0]) / static_cast<float>(tree_w);
    const float sy = static_cast<float>(d[3] - d[1]) / static_cast<float>(tree_h);
    for (int i = 0; i < n; ++i) {
        const SurfacePlace& p = places[i];
        auto it = g_surfaces.find(p.key);
        if (it == g_surfaces.end() || !it->second.layer.chain) continue;
        Surface& s = it->second;
        const bool shown = p.shown && p.w > 0 && p.h > 0;
        if (s.attached && (!shown || s.on_top != p.on_top)) {
            (s.on_top ? g_above : g_below)->RemoveVisual(s.layer.visual);
            s.attached = false;
            changed = true;
        }
        if (!shown) continue;
        if (!s.attached) {
            s.on_top = p.on_top;
            (s.on_top ? g_above : g_below)->AddVisual(s.layer.visual, TRUE, nullptr);
            s.attached = true;
            changed = true;
        }
        const RECT dst{static_cast<LONG>(d[0] + p.x * sx), static_cast<LONG>(d[1] + p.y * sy),
                       static_cast<LONG>(d[0] + (p.x + p.w) * sx), static_cast<LONG>(d[1] + (p.y + p.h) * sy)};
        if (std::memcmp(&dst, &s.layer.placed, sizeof dst) == 0) continue;
        s.layer.placed = dst;
        D2D_MATRIX_3X2_F m{};
        m._11 = static_cast<float>(dst.right - dst.left) / static_cast<float>(s.layer.w);
        m._22 = static_cast<float>(dst.bottom - dst.top) / static_cast<float>(s.layer.h);
        m._31 = static_cast<float>(dst.left);
        m._32 = static_cast<float>(dst.top);
        s.layer.visual->SetTransform(m);
        changed = true;
    }
    if (changed) g_dcomp->Commit();
}

void gl_present_forget(void* share) {
    if (!share) return;
    std::lock_guard<std::mutex> hold(g_lock);
    for (auto it = g_sources.begin(); it != g_sources.end(); ++it) {
        if (it->first != share) continue;
        it->second->Release();
        g_sources.erase(it);
        return;
    }
}

void gl_present_retry() {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_active) return;
    bool ok = !g_gl.owed || show(g_gl);
    if (ok && g_tree.owed) ok = show(g_tree);
    // A failed Present may have lost the device and emptied g_surfaces.
    for (auto it = g_surfaces.begin(); ok && it != g_surfaces.end(); ++it) {
        if (it->second.layer.owed) ok = show(it->second.layer);
    }
    if (!ok) stop_locked();
}

void gl_present_stop() {
    std::lock_guard<std::mutex> hold(g_lock);
    stop_locked();
}

}  // namespace winapk::host
