// dmhost-linux: game launcher for arm64 Linux (Wayland + SDL2 gamepad input).
// Opens a Wayland window, passes it to the NVN → Vulkan replay, starts the
// game host on a background thread, then pumps SDL events for input.
//
// Usage:  dmhost-linux <pack-dir>  [DM_ROMFS=<romfs> DM_SAVE=<save> DM_TEMP=<tmp>]
//   The pack-dir must contain *.patched.bin / *.fixups.tsv / pack.tsv / game.tsv
//   and shaders/v-<chain>/dmsh_vk.lib.
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "../../dm/host/android/input_android.h"
#include "../../dm/host/android/input_map.h"

// plat_win_t (vka_present.c / plat_win.h): {wl_display*, wl_surface*, w, h}
typedef struct { void *display; void *surface; int width, height; } plat_win_t;

// nxhost API (vk_android.h, via dmhost)
void nxhost_set_window(void *w);
int  nxhost_main(int argc, char **argv);

// Global definitions from dm/host/main.c (included in the dmhost-linux link)
// g_flag and g_t0_ns are defined in main.c, which is linked alongside this file.
// (They are NOT defined here — main.c is in HOST_MAIN_C for this target.)

// ---- Gamehost thread -------------------------------------------------------
static struct { int argc; char **argv; int rc; } g_host;
static void *host_thread(void *arg) {
    (void)arg;
    g_host.rc = nxhost_main(g_host.argc, g_host.argv);
    SDL_Event quit; quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
    return NULL;
}

// ---- SDL2 → Android keycode / axis mapping ---------------------------------
// SDL uses an Xbox-layout controller; Android's BUTTON_A/B/X/Y are also Xbox-
// positioned, so the mapping is 1-to-1 for position-based pads.
static int sdl_button_to_ak(SDL_GameControllerButton b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A:             return AK_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_B:             return AK_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_X:             return AK_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_Y:             return AK_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_BACK:          return AK_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_GUIDE:         return AK_BUTTON_MODE;
    case SDL_CONTROLLER_BUTTON_START:         return AK_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return AK_BUTTON_THUMBL;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return AK_BUTTON_THUMBR;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return AK_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AK_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return AK_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return AK_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return AK_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return AK_DPAD_RIGHT;
    default:                                   return -1;
    }
}

// SDL2 axis → (Android axis id, normalised value -1..1 or 0..1 for triggers)
static void sdl_axis_send(int device, SDL_GameControllerAxis a, int16_t raw) {
    int aa; float v;
    switch (a) {
    case SDL_CONTROLLER_AXIS_LEFTX:        aa = AA_X;        v = raw / 32767.0f; break;
    case SDL_CONTROLLER_AXIS_LEFTY:        aa = AA_Y;        v = raw / 32767.0f; break;
    // Right stick: Android expects Z/RZ for a gamepad (input_map.h comment)
    case SDL_CONTROLLER_AXIS_RIGHTX:       aa = AA_Z;        v = raw / 32767.0f; break;
    case SDL_CONTROLLER_AXIS_RIGHTY:       aa = AA_RZ;       v = raw / 32767.0f; break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  aa = AA_LTRIGGER; v = raw / 32767.0f; break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: aa = AA_RTRIGGER; v = raw / 32767.0f; break;
    default: return;
    }
    nxhost_input_axis(device, aa, v);
}

// ---- Controller registry (SDL joystick id → host device id) ----------------
#define MAX_CTRLS 8
static struct { SDL_GameController *gc; SDL_JoystickID jid; int device; } g_ctrl[MAX_CTRLS];

static int ctrl_slot_for_jid(SDL_JoystickID jid) {
    for (int i = 0; i < MAX_CTRLS; i++)
        if (g_ctrl[i].gc && g_ctrl[i].jid == jid) return i;
    return -1;
}

static void ctrl_added(int idx) {
    if (!SDL_IsGameController(idx)) return;
    SDL_GameController *gc = SDL_GameControllerOpen(idx);
    if (!gc) return;
    SDL_JoystickID jid = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    // find a free slot
    for (int i = 0; i < MAX_CTRLS; i++) {
        if (!g_ctrl[i].gc) {
            g_ctrl[i].gc = gc;
            g_ctrl[i].jid = jid;
            int player = nxhost_input_device_added(jid, SDL_GameControllerName(gc), 0);
            g_ctrl[i].device = jid;
            (void)player;
            fprintf(stderr, "[dmhost] controller %d connected: %s (host device %d)\n",
                    i, SDL_GameControllerName(gc), (int)jid);
            return;
        }
    }
    SDL_GameControllerClose(gc);
}

static void ctrl_removed(SDL_JoystickID jid) {
    int s = ctrl_slot_for_jid(jid);
    if (s < 0) return;
    nxhost_input_device_removed(jid);
    SDL_GameControllerClose(g_ctrl[s].gc);
    g_ctrl[s].gc = NULL;
    fprintf(stderr, "[dmhost] controller disconnected\n");
}

// ---- main ------------------------------------------------------------------
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pack-dir>\n"
                        "  DM_ROMFS=<romfs-dir>  (the game's RomFS)\n"
                        "  DM_SAVE=<save-dir>    (writable save directory)\n"
                        "  DM_TEMP=<temp-dir>    (writable temp directory)\n", argv[0]);
        return 1;
    }

    // Change into the pack directory so the host finds pack.tsv / shaders/ / game.tsv
    if (chdir(argv[1]) != 0) { perror(argv[1]); return 1; }
    // Build a new argv with pack-dir replaced by "." for nxhost_main
    char *new_argv[64]; int new_argc = 0;
    new_argv[new_argc++] = argv[0];
    new_argv[new_argc++] = (char *)".";
    for (int i = 2; i < argc && new_argc < 63; i++) new_argv[new_argc++] = argv[i];
    new_argv[new_argc] = NULL;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland");
    SDL_Window *win = SDL_CreateWindow("Kirby and the Forgotten Land",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!win) {
        SDL_ClearError();
        win = SDL_CreateWindow("dmhost-linux",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    }
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(win, &wm) || wm.subsystem != SDL_SYSWM_WAYLAND) {
        fprintf(stderr, "SDL_GetWindowWMInfo: %s — not on Wayland?\n", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }

    plat_win_t pw;
    pw.display = wm.info.wl.display;
    pw.surface = wm.info.wl.surface;
    SDL_GetWindowSize(win, &pw.width, &pw.height);
    fprintf(stderr, "[dmhost] window %dx%d\n", pw.width, pw.height);

    nxhost_set_window(&pw);

    // Enumerate already-connected controllers
    SDL_GameControllerEventState(SDL_ENABLE);
    for (int i = 0; i < SDL_NumJoysticks(); i++) ctrl_added(i);

    // Start the host on a background thread (it runs the entire game)
    g_host.argc = new_argc;
    g_host.argv = new_argv;
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);  // 8 MiB, as Android
    pthread_t th; pthread_create(&th, &attr, host_thread, NULL);
    pthread_attr_destroy(&attr);

    // SDL event loop — input pump for the host
    SDL_Event ev;
    while (SDL_WaitEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            goto done;

        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                SDL_GetWindowSize(win, &pw.width, &pw.height);
                // Re-notify the replay of the new size
                nxhost_set_window(&pw);
            }
            break;

        case SDL_CONTROLLERDEVICEADDED:
            ctrl_added(ev.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            ctrl_removed(ev.cdevice.which);
            break;

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            int slot = ctrl_slot_for_jid(ev.cbutton.which);
            if (slot < 0) break;
            int ak = sdl_button_to_ak((SDL_GameControllerButton)ev.cbutton.button);
            if (ak >= 0) nxhost_input_key(g_ctrl[slot].device, ak,
                                           ev.type == SDL_CONTROLLERBUTTONDOWN);
            break;
        }

        case SDL_CONTROLLERAXISMOTION: {
            int slot = ctrl_slot_for_jid(ev.caxis.which);
            if (slot < 0) break;
            sdl_axis_send(g_ctrl[slot].device,
                          (SDL_GameControllerAxis)ev.caxis.axis, ev.caxis.value);
            break;
        }

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            // Keyboard fallback: map common keys to controller buttons on device 0
            if (ev.key.repeat) break;
            switch (ev.key.keysym.sym) {
            case SDLK_RETURN: nxhost_input_key(0, AK_BUTTON_A, ev.type==SDL_KEYDOWN); break;
            case SDLK_ESCAPE: nxhost_input_key(0, AK_BUTTON_B, ev.type==SDL_KEYDOWN); break;
            case SDLK_UP:     nxhost_input_key(0, AK_DPAD_UP,    ev.type==SDL_KEYDOWN); break;
            case SDLK_DOWN:   nxhost_input_key(0, AK_DPAD_DOWN,  ev.type==SDL_KEYDOWN); break;
            case SDLK_LEFT:   nxhost_input_key(0, AK_DPAD_LEFT,  ev.type==SDL_KEYDOWN); break;
            case SDLK_RIGHT:  nxhost_input_key(0, AK_DPAD_RIGHT, ev.type==SDL_KEYDOWN); break;
            case SDLK_q:      goto done;
            default: break;
            }
            break;
        }
    }

done:
    nxhost_set_window(NULL);
    pthread_join(th, NULL);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return g_host.rc;
}
