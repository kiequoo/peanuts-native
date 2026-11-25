// Front end on Linux (front.h). Mirrors front_android.c with Bionic-only items removed.
// The guest runs on the calling thread; the Wayland window is passed to the NVN → Vulkan replay with nxhost_set_window (dm/gfx/vk_android).
// PEANUTS: The front end prepares cold Vulkan pipelines before entering the guest.
#include "../dm.h"
#include "../front.h"
#include "../../gfx/vk_android/vk_android.h"   // PEANUTS: prepare cold pipelines before guest entry
#include <stdatomic.h>
void front_run(void (*guest)(void)) {
    vka_pso_prepare_launch();   // PEANUTS: startup waits for the cold pipeline build when a seed ships
    LOG("front end: the guest runs on this thread; presents go to the launcher's window (nxhost_set_window)");
    guest();
}
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 0; }
int front_screen_variable_refresh(void) { return 0; }
void front_screen_match(int fps) { (void)fps; }
void front_progress(const char *title, long done, long total) { (void)done; (void)total; if (title) { static _Atomic int once; if (!atomic_exchange(&once, 1)) LOG("[RED] front end: no progress screen on Linux yet (%s)", title); } }
// Software keyboard: no native on-screen keyboard on bare Linux; return -1 (the caller's no-window path / cancel)
extern int g_swkbd_min_chars;
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap; return -1;
}
// User selector: no picker; return -2 (the host takes the preselected user)
int front_user_select(const char *const *names, const int *offered, int n, int def) { (void)names; (void)offered; (void)n; (void)def; return -2; }
// Controller applet: no join screen; return -2 (the host answers at once with whatever controllers are connected)
int front_controller_support(const void *arg, int *count) { (void)arg; (void)count; return -2; }
int main(int argc, char **argv);
// Called by the launcher (front_linux_main.c) on its main thread. No heap tagging to disable on glibc.
__attribute__((visibility("default"))) int nxhost_main(int argc, char **argv) {
    return main(argc, argv);
}
