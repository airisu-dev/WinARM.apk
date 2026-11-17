// gl_present.h -- GL frames to the window without a readback (on by default; WINAPK_GL_PRESENT, GpuPresent).
//
// The GL frame (the content GL view's or a native window's pbuffer) is copied
// on the GPU into a DirectComposition swap chain; the view tree and each
// SurfaceView are layers of their own.

#pragma once

#include <cstdint>

namespace winapk::host {

bool gl_present_wanted();
// WINAPK_GL_PRESENT=gpu: on even on a software renderer.
bool gl_present_forced();
bool gl_present_active();
void gl_present_stop();

// The bottom-left w x h of the current pbuffer (its D3D11 share handle), placed
// as a frame of shown_w x shown_h.
bool gl_present_frame(void* share, int w, int h, int shown_w, int shown_h);

// The view tree's frame, straight alpha ARGB. No-op while inactive.
void gl_present_tree(const uint32_t* argb, int w, int h);
void gl_present_tree_clear();

// A black GL layer, for content that is not a GL view.
bool gl_present_base(int w, int h);

// A SurfaceView's lockCanvas frame, opaque ARGB.
bool gl_present_surface(int key, const uint32_t* argb, int w, int h);
bool gl_present_has_surface(int key);

// In the view tree's pixels; layers not listed are dropped.
struct SurfacePlace {
    int key, x, y, w, h;
    bool on_top, shown;
};
void gl_present_surfaces_place(const SurfacePlace* places, int n, int tree_w, int tree_h);

}  // namespace winapk::host
