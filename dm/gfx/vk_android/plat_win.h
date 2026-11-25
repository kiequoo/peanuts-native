// Platform window handle abstraction: the opaque object nxhost_set_window accepts and the present layer uses to create a VkSurfaceKHR.
// Android: ANativeWindow (reference-counted by the platform). Linux arm64: plat_win_t (a Wayland display + surface + size, owned by the caller).
// The helpers below isolate the two cases so the rest of vka_present.c can be platform-agnostic.
#pragma once
#if defined(__ANDROID__)
#include <android/native_window.h>
typedef ANativeWindow plat_win_t;
static inline void plat_win_retain(plat_win_t *w) { ANativeWindow_acquire(w); }
static inline void plat_win_release(plat_win_t *w) { ANativeWindow_release(w); }
static inline int plat_win_width(plat_win_t *w) { return ANativeWindow_getWidth(w); }
static inline int plat_win_height(plat_win_t *w) { return ANativeWindow_getHeight(w); }
#elif defined(__linux__) && defined(__aarch64__)
#include <wayland-client.h>
// Passed by the Linux launcher (front_linux_main.c) after creating the Wayland window via SDL2.
// The launcher fills width/height from the SDL window size and updates them on resize before the next nxhost_set_window call.
typedef struct { struct wl_display *display; struct wl_surface *surface; int width, height; } plat_win_t;
static inline void plat_win_retain(plat_win_t *w) { (void)w; }   // no reference counting; the launcher owns the struct
static inline void plat_win_release(plat_win_t *w) { (void)w; }
static inline int plat_win_width(plat_win_t *w) { return w ? w->width : 0; }
static inline int plat_win_height(plat_win_t *w) { return w ? w->height : 0; }
#else
// Mac uses Metal (nvn_mtl.m), not this Vulkan replay; a stub keeps the header includable
typedef void plat_win_t;
static inline void plat_win_retain(plat_win_t *w) { (void)w; }
static inline void plat_win_release(plat_win_t *w) { (void)w; }
static inline int plat_win_width(plat_win_t *w) { (void)w; return 0; }
static inline int plat_win_height(plat_win_t *w) { (void)w; return 0; }
#endif
