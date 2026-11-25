// Linux stub for H.264/AVC movie decoding (hle_movie_avc.h). No software decoder on Linux yet (Stage 2+: add FFmpeg).
// Games that play H.264 cutscenes (Kirby and the Forgotten Land: opening and world-entry FMVs) show black video but continue normally.
// The host's movie path treats mavc_decode returning -1 as a decode error and skips the picture; mavc_take returning 0 means no picture — both are tolerated.
// Include paths are set by CMakeLists.txt (HOST = dm/host is on the include path)
#include "dm.h"
#include "hle_movie_avc.h"
#include <stdlib.h>
#include <stdatomic.h>

#define RED_ONCE(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) LOG("[RED] " __VA_ARGS__); } while (0)

struct mavc { int dummy; };

mavc *mavc_new(void) { return calloc(1, sizeof(mavc)); }
void mavc_free(mavc *a) { free(a); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; RED_ONCE("H.264 AVC decoder: not yet on Linux (cutscenes show black; linux/PORTING.md)"); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) { (void)a; (void)p; (void)n; (void)pts_us; return -1; }
int mavc_take(mavc *a, int flush, uint8_t *nv12, int w, int h, int64_t *pts_us) { (void)a; (void)flush; (void)nv12; (void)w; (void)h; (void)pts_us; return 0; }
int mavc_queued(const mavc *a) { (void)a; return 0; }
void mavc_set_rgba(mavc *a) { (void)a; }
void mavc_drop(mavc *a) { (void)a; }
