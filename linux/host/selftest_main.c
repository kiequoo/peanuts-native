// Stage 1 selftest launcher: open a Wayland window via SDL2, hand it to the NVN → Vulkan replay, and let NXHOST_VK_SELFTEST run.
// Set NXHOST_VK_SELFTEST=600 (600 frames, ~10 s at 60 fps) to trigger the self-test thread automatically on the first nxhost_set_window call.
// The test clears a texture, draws a triangle, runs a compute dispatch, reads back texels, and presents the result for the given number of frames.
// It logs "[vk] selftest ... ended: passed" or "FAILED" at the end (search for "selftest" in the output).
//
// Usage:
//   NXHOST_VK_SELFTEST=600 ./selftest_launcher
//   NXHOST_VK_SELFTEST=600 DM_VK_DRIVER=/path/to/turnip.so ./selftest_launcher   (custom driver)
//
// The process exits when the window is closed or after NXHOST_VK_SELFTEST+30 frames of idle time.
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// plat_win_t from vka_present.c's plat_win.h (defined for arm64 Linux as a Wayland display+surface+dimensions struct)
// Forward-declare it here to avoid pulling in Wayland headers directly (SDL2 provides them indirectly)
typedef struct { void *display; void *surface; int width, height; } plat_win_t;
// Host API (vk_android.h, via dmhost)
void nxhost_set_window(void *w);

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: NXHOST_VK_SELFTEST=<frames> %s\n", argv0);
    fprintf(stderr, "  Opens a Wayland window and runs the Vulkan replay self-test for <frames> frames.\n");
    fprintf(stderr, "  Search the log for '[vk] selftest' lines. Exit: close the window or wait.\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *frames_env = getenv("NXHOST_VK_SELFTEST");
    int frames = frames_env ? atoi(frames_env) : 0;
    if (frames <= 0) { usage(argv[0]); fprintf(stderr, "NXHOST_VK_SELFTEST is not set or is 0: set it to run the self-test (e.g. NXHOST_VK_SELFTEST=600)\n"); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    // Request Wayland (not XWayland) explicitly; fall back to whatever SDL chooses if Wayland is unavailable
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland");
    SDL_Window *win = SDL_CreateWindow("dmhost selftest",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!win) {
        // Vulkan flag failed (no VK_KHR_swapchain? or XWayland SDL?): try without it
        SDL_ClearError();
        win = SDL_CreateWindow("dmhost selftest", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    }
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(win, &wm)) {
        fprintf(stderr, "SDL_GetWindowWMInfo: %s\nIs the compositor Wayland?\n", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }
    if (wm.subsystem != SDL_SYSWM_WAYLAND) {
        fprintf(stderr, "Window is not on Wayland (subsystem %d). Set SDL_VIDEODRIVER=wayland.\n", wm.subsystem);
        SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }

    plat_win_t pw;
    pw.display = wm.info.wl.display;
    pw.surface = wm.info.wl.surface;
    SDL_GetWindowSize(win, &pw.width, &pw.height);
    fprintf(stderr, "[selftest_launcher] Wayland window %dx%d, display=%p surface=%p\n",
            pw.width, pw.height, pw.display, pw.surface);

    // This call triggers nxhost_vk_selftest on its own thread when NXHOST_VK_SELFTEST is set
    nxhost_set_window(&pw);

    // Run the event loop: dispatch Wayland events until the window is closed or the selftest has had time to finish
    // The selftest runs 'frames' frames at whatever rate the GPU allows; add generous headroom
    int keep_going = 1;
    while (keep_going) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) {
                keep_going = 0;
            }
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                // Update the window size for the next nxhost_set_window call if the swapchain needs recreation
                SDL_GetWindowSize(win, &pw.width, &pw.height);
            }
        }
        SDL_Delay(16);   // 60 Hz event loop; Vulkan presents happen on the replay's present thread
    }

    nxhost_set_window(NULL);   // release the window from the replay
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
