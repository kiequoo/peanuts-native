// Platform layer, bare Linux side (glibc, arm64): the services plat.h names, on Linux system calls. linux/PORTING.md maps each Apple call to what replaces it here.
// Based on plat_android.c (Bionic) with the following differences:
//   - Thread slots use a local-exec __thread block instead of Bionic's TLS_SLOT_APP (see "Guest thread slots" below).
//   - plat_device_model reads /etc/os-release instead of Android system properties.
//   - plat_thread_stack does not subtract the guard page: glibc's pthread_getattr_np already excludes it (Bionic does not).
//   - sigaltstack is set up explicitly per-thread (glibc does not do this automatically; the guest rewriter parks values below sp).
//   - mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL) is not called (Bionic-only).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../dm.h"
#include <dirent.h>
#include <ftw.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <malloc.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include "plat_guestmap.h"
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/xattr.h>
#include <unistd.h>

#define RED_ONCE(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) LOG("[RED] " __VA_ARGS__); } while (0)

// ---- Object file conventions ----
#define TABLE(n) extern const char __start_##n[] __attribute__((weak, visibility("hidden"))), __stop_##n[] __attribute__((weak, visibility("hidden")));
TABLE(swhle) TABLE(swforce) TABLE(swnvn)
const void *plat_table(const char *name, unsigned long *size) {
    const char *b = NULL, *e = NULL;
    if (!strcmp(name, "swhle")) { b = __start_swhle; e = __stop_swhle; }
    else if (!strcmp(name, "swforce")) { b = __start_swforce; e = __stop_swforce; }
    else if (!strcmp(name, "swnvn")) { b = __start_swnvn; e = __stop_swnvn; }
    *size = b && e > b ? (unsigned long)(e - b) : 0;
    return *size ? b : NULL;
}

// ---- Time ----
uint64_t plat_clock_ns(clockid_t c) { struct timespec ts; clock_gettime(c, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
uint64_t plat_abs_time(void) { return plat_clock_ns(CLOCK_MONOTONIC); }
void plat_timebase(plat_timebase_t *tb) { tb->numer = tb->denom = 1; }
void plat_wait_until(uint64_t abs) {
    struct timespec ts = {(time_t)(abs / 1000000000ull), (long)(abs % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
int64_t plat_process_age_ns(void) {
    char b[1024]; int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return -1; b[n] = 0;
    char *p = strrchr(b, ')'); if (!p) return -1;
    unsigned long long start = 0; int f = 2;
    for (char *t = strtok(p + 1, " "); t; t = strtok(NULL, " ")) if (++f == 22) { start = strtoull(t, NULL, 10); break; }
    long hz = sysconf(_SC_CLK_TCK); if (f != 22 || hz <= 0) return -1;
    return (int64_t)plat_clock_ns(CLOCK_BOOTTIME) - (int64_t)(start * (1000000000ull / (unsigned long long)hz));
}

// ---- Memory ----
static int vm_copy(uint64_t addr, void *local, size_t len, int wr) {
    if (!len) return 0;
    struct iovec l = {local, len}, r = {(void *)(uintptr_t)addr, len};
    ssize_t n = wr ? process_vm_writev(getpid(), &l, 1, &r, 1, 0) : process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (n == (ssize_t)len) return 0;
    if (n < 0 && (errno == ENOSYS || errno == EPERM)) {
        if (wr) return -1;
        int p[2]; if (pipe2(p, O_CLOEXEC)) return -1;
        size_t done = 0;
        while (done < len) { size_t k = len - done > 4096 ? 4096 : len - done;
            if (write(p[1], (const uint8_t *)(uintptr_t)addr + done, k) != (ssize_t)k || read(p[0], (uint8_t *)local + done, k) != (ssize_t)k) break;
            done += k; }
        close(p[0]); close(p[1]); return done == len ? 0 : -1;
    }
    return -1;
}
int plat_mem_read(uint64_t addr, void *out, size_t len) { return vm_copy(addr, out, len, 0); }
int plat_mem_write(uint64_t addr, const void *src, size_t len) { return vm_copy(addr, (void *)src, len, 1); }
int plat_mem_region(uint64_t addr, uint64_t *start, uint64_t *size, int *prot) {
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return -1;
    char line[512]; int r = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a, b; char perm[5] = "";
        if (sscanf(line, "%llx-%llx %4s", &a, &b, perm) != 3 || b <= addr) continue;
        *start = a; *size = b - a; *prot = (perm[0] == 'r' ? PROT_READ : 0) | (perm[1] == 'w' ? PROT_WRITE : 0) | (perm[2] == 'x' ? PROT_EXEC : 0);
        r = 0; break;
    }
    fclose(f); return r;
}
void *plat_mem_alias(void *addr, size_t len) { (void)addr; (void)len; RED_ONCE("a writable alias of guest pages is not yet on Linux (needs memfd-backed modules, linux/PORTING.md)"); return NULL; }
void plat_icache_invalidate(void *p, size_t len) { __builtin___clear_cache((char *)p, (char *)p + len); }
size_t plat_page_size(void) { return (size_t)getpagesize(); }
#define GUEST_VA_END (1ull << 39)
#define GUEST_ALIGN 0x200000ull
static void *reserve_at(uint64_t at, size_t size) {
    void *p = mmap((void *)(uintptr_t)at, size, PROT_NONE, PLAT_GUEST_MAP | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if ((uint64_t)(uintptr_t)p != at) { munmap(p, size); return NULL; }
    return p;
}
void *plat_mem_reserve(size_t size) {
    const char *e = getenv("DM_GUEST_BASE");
    uint64_t want = e && *e ? strtoull(e, NULL, 16) : 0x4000000000ull, step = 0x1000000000ull;
    void *p = NULL; uint64_t at = 0;
    if (want & (GUEST_ALIGN - 1)) die("DM_GUEST_BASE %s is not 2 MiB-aligned", e);
    for (uint64_t a = want; !p && a + size <= GUEST_VA_END; a += step) if ((p = reserve_at(a, size))) at = a;
    for (uint64_t a = want & ~(step - 1); !p && a >= 2 * step; a -= step) if ((p = reserve_at(a - step, size))) at = a - step;
    if (!p) {
        uint8_t *raw = mmap(NULL, size + GUEST_ALIGN, PROT_NONE, PLAT_GUEST_MAP, -1, 0);
        if (raw == MAP_FAILED) return NULL;
        p = (void *)(((uintptr_t)raw + GUEST_ALIGN - 1) & ~(uintptr_t)(GUEST_ALIGN - 1));
        if ((uint8_t *)p > raw) munmap(raw, (size_t)((uint8_t *)p - raw));
        munmap((uint8_t *)p + size, (size_t)(raw + GUEST_ALIGN - (uint8_t *)p));
        LOG("[RED] guest block: no room at a fixed address below %#llx; the kernel gave %p", GUEST_VA_END, p);
    } else if (at != want) LOG("guest block: %#llx was taken, reserved at %#llx", (unsigned long long)want, (unsigned long long)at);
    if ((uint64_t)(uintptr_t)p + size > GUEST_VA_END) LOG("[RED] guest block %p+%#zx ends above %#llx (the 39-bit user space)", p, size, GUEST_VA_END);
    plat_mem_name_guest(p, size);
    return p;
}
void plat_mem_name_guest(void *p, size_t n) { prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, p, n, "nxnative guest"); }
int plat_maps_count(int *max) {
    static _Atomic int mx;
    if (max) {
        int m = atomic_load(&mx);
        if (!m) { char b[32]; int fd = open("/proc/sys/vm/max_map_count", O_RDONLY | O_CLOEXEC); ssize_t k = fd >= 0 ? read(fd, b, sizeof b - 1) : -1; if (fd >= 0) close(fd);
                  m = k > 0 ? (b[k] = 0, atoi(b)) : 0; if (m <= 0) m = 65530; atomic_store(&mx, m); }
        *max = m;
    }
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    char b[16384]; ssize_t k; int n = 0;
    while ((k = read(fd, b, sizeof b)) > 0) for (ssize_t i = 0; i < k; i++) n += b[i] == '\n';
    close(fd);
    return k < 0 ? -1 : n;
}
int64_t plat_footprint_bytes(void) {
    char b[128]; int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return -1; b[n] = 0;
    unsigned long long total, rss; if (sscanf(b, "%llu %llu", &total, &rss) != 2) return -1;
    return (int64_t)(rss * (unsigned long long)getpagesize());
}

struct plat_zone { char unused; };
static struct plat_zone g_zone;
plat_zone_t *plat_zone_create(const char *name) { (void)name; return &g_zone; }
void *plat_zone_malloc(plat_zone_t *z, size_t n) { (void)z; return malloc(n); }
void *plat_zone_memalign(plat_zone_t *z, size_t align, size_t n) { (void)z; void *p = NULL; return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, n) ? NULL : p; }
void *plat_zone_realloc(plat_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
void plat_zone_free(plat_zone_t *z, void *p) { (void)z; free(p); }
size_t plat_malloc_size(const void *p) { return malloc_usable_size(p); }
size_t plat_malloc_good_size(size_t n) { return n; }

struct plat_ptrmap { const void **k; void **v; size_t cap, used; };
#define TOMB ((const void *)1)
static size_t ph(const void *k, size_t cap) { uint64_t x = (uint64_t)(uintptr_t)k; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return (size_t)x & (cap - 1); }
plat_ptrmap_t *plat_ptrmap_create(void) {
    plat_ptrmap_t *m = calloc(1, sizeof *m); if (!m) return NULL;
    m->cap = 64; m->k = calloc(m->cap, sizeof *m->k); m->v = calloc(m->cap, sizeof *m->v);
    if (!m->k || !m->v) die("pointer map allocation failed");
    return m;
}
void *plat_ptrmap_get(plat_ptrmap_t *m, const void *k) {
    for (size_t i = ph(k, m->cap), n = 0; n < m->cap && m->k[i]; i = (i + 1) & (m->cap - 1), n++) if (m->k[i] == k) return m->v[i];
    return NULL;
}
static void pm_grow(plat_ptrmap_t *m) {
    plat_ptrmap_t o = *m; m->cap *= 2; m->used = 0;
    m->k = calloc(m->cap, sizeof *m->k); m->v = calloc(m->cap, sizeof *m->v);
    if (!m->k || !m->v) die("pointer map allocation failed");
    for (size_t i = 0; i < o.cap; i++) if (o.k[i] && o.k[i] != TOMB) plat_ptrmap_set(m, o.k[i], o.v[i]);
    free(o.k); free(o.v);
}
void plat_ptrmap_set(plat_ptrmap_t *m, const void *k, void *v) {
    if ((m->used + 1) * 4 > m->cap * 3) pm_grow(m);
    size_t i = ph(k, m->cap), tomb = SIZE_MAX;
    for (; m->k[i]; i = (i + 1) & (m->cap - 1)) {
        if (m->k[i] == k) { m->v[i] = v; return; }
        if (m->k[i] == TOMB && tomb == SIZE_MAX) tomb = i;
    }
    if (tomb != SIZE_MAX) i = tomb; else m->used++;
    m->k[i] = k; m->v[i] = v;
}
void plat_ptrmap_remove(plat_ptrmap_t *m, const void *k) {
    for (size_t i = ph(k, m->cap), n = 0; n < m->cap && m->k[i]; i = (i + 1) & (m->cap - 1), n++) if (m->k[i] == k) { m->k[i] = TOMB; m->v[i] = NULL; return; }
}

// ---- Guest thread slots ----
// Linux: a local-exec __thread struct (plat.h "Guest thread slots"). tpidr_el0 is the glibc thread pointer; &g_slots_linux has a constant offset from it (the TPREL) across
// all threads (local-exec TLS model, resolved at link time). plat_slot_key_entry measures that offset at runtime: the first call fixes it for all subsequent threads.
// The rewriter emits a single "mrs tpidr_el0" (--target linux, like --target apple with tpidrro_el0); the key page entry is the TPREL + field offset within the struct.
// Signal safety: __thread storage is always valid once the thread exists; no allocation is needed in the signal path (unlike the Android path, which may allocate on first use).
__thread plat_slots_t g_slots_linux;   // zero-initialized; plat.h declares this as extern for the dm_slot_* macros
static uint64_t slots_tprel(void) {
    uint64_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return (uint64_t)(uintptr_t)&g_slots_linux - tp;
}
// Per-thread alternate signal stack: the rewriter parks temporaries 24 bytes below sp; a signal arriving in guest code must run on a different stack (SA_ONSTACK in signal
// handlers), which glibc does not set up automatically for new threads (unlike Android's ART). 8 KB is enough for the host's signal handlers (crash.c, hle_thread.c probe)
#define SIGSTK_SIZE (8 * 1024)
void plat_ensure_sigaltstack(void) {
    stack_t ss; if (!sigaltstack(NULL, &ss) && ss.ss_sp) return;   // already set up for this thread
    void *p = malloc(SIGSTK_SIZE); if (!p) { RED_ONCE("cannot allocate signal stack (sigaltstack)"); return; }
    ss = (stack_t){.ss_sp = p, .ss_size = SIGSTK_SIZE, .ss_flags = 0};
    sigaltstack(&ss, NULL);
}
void plat_slots_init(void) {
    plat_ensure_sigaltstack();
    uint64_t tprel = slots_tprel();
    LOG("thread slots (Linux __thread): tpidr_el0 + %#llx → K18 +0 KTLS +8 KTLR +0x10 KGATE +0x18", (unsigned long long)tprel);
}
uint64_t plat_slot_key_entry(unsigned slot) { return slots_tprel() + (uint64_t)slot * 8; }
void plat_slot_check(uint64_t off) {
    dm_slot_set(18, (void *)0x1234);
    uint64_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if (*(uint64_t *)(tp + off) != 0x1234) die("thread slot layout mismatch (key page entry %#llx)", (unsigned long long)off);
    dm_slot_set(18, 0);
}

// ---- Threads ----
/* pthread_gettid_np requires glibc headers ≥ 2.30; use gettid() — callers always pass pthread_self() */
plat_thread_t plat_thread_of(pthread_t t) { (void)t; return (plat_thread_t)gettid(); }
plat_thread_t plat_thread_self(void) { return (plat_thread_t)gettid(); }
void plat_thread_self_release(plat_thread_t t) { (void)t; }
void plat_thread_set_name(const char *name) { char b[16]; snprintf(b, sizeof b, "%s", name); pthread_setname_np(pthread_self(), b); }
// glibc's pthread_getattr_np already excludes the guard page from the stack range (unlike Bionic, which includes it). No subtraction needed here
void plat_thread_stack(uint64_t *top, uint64_t *size) {
    pthread_attr_t a; void *lo = NULL; size_t n = 0;
    if (!pthread_getattr_np(pthread_self(), &a)) { pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); }
    *top = (uint64_t)(uintptr_t)lo + n; *size = n;
}

static struct { pthread_mutex_t mu; _Atomic int state; _Atomic pid_t tid; plat_regs_t regs; } g_susp = {PTHREAD_MUTEX_INITIALIZER, 0, 0, {{0}}};
enum { SU_IDLE, SU_ASKED, SU_STOPPED, SU_RESUME, SU_LEFT };
static int susp_sig(void) { return SIGRTMIN + 2; }   // glibc's SIGRTMIN already accounts for NPTL's reservation (unlike Bionic where SIGRTMIN is the raw value)
static void futex_wait(_Atomic int *a, int v, const struct timespec *rel) { syscall(SYS_futex, a, FUTEX_WAIT_PRIVATE, v, rel, NULL, 0); }
static void futex_wake(_Atomic int *a) { syscall(SYS_futex, a, FUTEX_WAKE_PRIVATE, INT32_MAX, NULL, NULL, 0); }
static void on_susp(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; int e = errno; ucontext_t *uc = uc_;
    if (atomic_load(&g_susp.tid) != gettid()) { errno = e; return; }
    for (int i = 0; i < 29; i++) g_susp.regs.x[i] = PLAT_UC_X(uc, i);
    g_susp.regs.fp = PLAT_UC_FP(uc); g_susp.regs.lr = PLAT_UC_LR(uc); g_susp.regs.sp = PLAT_UC_SP(uc); g_susp.regs.pc = PLAT_UC_PC(uc); g_susp.regs.cpsr = PLAT_UC_CPSR(uc);
    int want = SU_ASKED;
    if (atomic_compare_exchange_strong(&g_susp.state, &want, SU_STOPPED)) {
        futex_wake(&g_susp.state);
        int s; while ((s = atomic_load(&g_susp.state)) != SU_RESUME) futex_wait(&g_susp.state, s, NULL);
        atomic_store(&g_susp.state, SU_LEFT); futex_wake(&g_susp.state);
    }
    errno = e;
}
static void susp_install(void) {
    struct sigaction sa = {.sa_sigaction = on_susp, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigemptyset(&sa.sa_mask); sigaction(susp_sig(), &sa, NULL);
}
static pthread_once_t g_susp_once = PTHREAD_ONCE_INIT;
int plat_thread_suspend(plat_thread_t t) {
    if (!t || (pid_t)t == gettid()) return -1;
    pthread_once(&g_susp_once, susp_install);
    pthread_mutex_lock(&g_susp.mu);
    atomic_store(&g_susp.state, SU_ASKED); atomic_store(&g_susp.tid, (pid_t)t);
    if (syscall(SYS_tgkill, getpid(), (pid_t)t, susp_sig())) { atomic_store(&g_susp.tid, 0); atomic_store(&g_susp.state, SU_IDLE); pthread_mutex_unlock(&g_susp.mu); return -1; }
    uint64_t dl = plat_abs_time() + 500000000ull;
    while (atomic_load(&g_susp.state) == SU_ASKED) {
        uint64_t now = plat_abs_time(); if (now >= dl) break;
        struct timespec rel = {0, (long)((dl - now) < 10000000ull ? dl - now : 10000000ull)}; futex_wait(&g_susp.state, SU_ASKED, &rel);
    }
    int want = SU_ASKED;
    if (atomic_compare_exchange_strong(&g_susp.state, &want, SU_IDLE)) { atomic_store(&g_susp.tid, 0); pthread_mutex_unlock(&g_susp.mu); return -1; }
    return 0;
}
int plat_thread_get_regs(plat_thread_t t, plat_regs_t *r) {
    if (atomic_load(&g_susp.tid) != (pid_t)t || atomic_load(&g_susp.state) != SU_STOPPED) return -1;
    *r = g_susp.regs; return 0;
}
void plat_thread_resume(plat_thread_t t) {
    if (atomic_load(&g_susp.tid) != (pid_t)t || atomic_load(&g_susp.state) != SU_STOPPED) return;
    atomic_store(&g_susp.state, SU_RESUME); futex_wake(&g_susp.state);
    int s; while ((s = atomic_load(&g_susp.state)) != SU_LEFT) futex_wait(&g_susp.state, s, NULL);
    atomic_store(&g_susp.tid, 0); atomic_store(&g_susp.state, SU_IDLE);
    pthread_mutex_unlock(&g_susp.mu);
}
static int task_stat(pid_t tid, char *b, size_t n) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", (int)tid);
    int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t k = read(fd, b, n - 1); close(fd); if (k <= 0) return -1; b[k] = 0; return 0;
}
int plat_thread_run_state(plat_thread_t t) {
    char b[512]; if (task_stat((pid_t)t, b, sizeof b)) return -1;
    const char *p = strrchr(b, ')'); if (!p || !p[1] || !p[2]) return -1;
    switch (p[2]) { case 'R': return 1; case 'T': case 't': return 2; case 'S': return 3; case 'D': return 4; case 'Z': case 'X': return 5; default: return -1; }
}
int64_t plat_thread_cpu_us(plat_thread_t t) {
    struct timespec ts; clockid_t c = (clockid_t)((~(unsigned)t << 3) | 4 | 2);
    if (clock_gettime(c, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
int64_t plat_thread_self_system_ns(void) {
    struct rusage ru; if (getrusage(RUSAGE_THREAD, &ru)) return 0;
    return (int64_t)ru.ru_stime.tv_sec * 1000000000ll + (int64_t)ru.ru_stime.tv_usec * 1000ll;
}
int plat_threads(plat_thread_t **list, unsigned *n) {
    DIR *d = opendir("/proc/self/task"); if (!d) return -1;
    unsigned cap = 64, k = 0; plat_thread_t *l = malloc(cap * sizeof *l);
    for (struct dirent *e; l && (e = readdir(d));) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        if (k == cap) { plat_thread_t *m = realloc(l, (cap *= 2) * sizeof *l); if (!m) { free(l); l = NULL; break; } l = m; }
        l[k++] = (plat_thread_t)strtoul(e->d_name, NULL, 10);
    }
    closedir(d); if (!l) return -1;
    *list = l; *n = k; return 0;
}
void plat_threads_release(plat_thread_t *list, unsigned n) { (void)n; free(list); }
static int qos_nice(plat_qos_t q) { return q == PLAT_QOS_USER_INTERACTIVE ? -4 : q == PLAT_QOS_UTILITY ? 10 : 0; }
int plat_qos_set_self(plat_qos_t q) { return setpriority(PRIO_PROCESS, (id_t)gettid(), qos_nice(q)) ? -1 : 0; }
plat_qos_t plat_qos_of(pthread_t t) {
    (void)t; errno = 0; int n = getpriority(PRIO_PROCESS, 0); /* 0 = calling thread; callers always pass pthread_self() */
    if (errno) return PLAT_QOS_UNSPECIFIED;
    return n < 0 ? PLAT_QOS_USER_INTERACTIVE : n > 0 ? PLAT_QOS_UTILITY : PLAT_QOS_USER_INITIATED;
}
void *plat_qos_override_start(pthread_t t, plat_qos_t q) { (void)t; (void)q; return NULL; }
void plat_qos_override_end(void *ov) { (void)ov; }

struct plat_sem { sem_t s; };
plat_sem_t *plat_sem_create(long value) { plat_sem_t *s = malloc(sizeof *s); if (s && sem_init(&s->s, 0, (unsigned)value)) { free(s); s = NULL; } return s; }
void plat_sem_signal(plat_sem_t *s) { sem_post(&s->s); }
int plat_sem_wait_ns(plat_sem_t *s, int64_t ns) {
    if (ns < 0) { while (sem_wait(&s->s)) if (errno != EINTR) return -1; return 0; }
    uint64_t dl = plat_clock_ns(CLOCK_MONOTONIC) + (uint64_t)ns; struct timespec ts = {(time_t)(dl / 1000000000ull), (long)(dl % 1000000000ull)};
    while (sem_clockwait(&s->s, CLOCK_MONOTONIC, &ts)) if (errno != EINTR) return -1;
    return 0;
}
void plat_sem_destroy(plat_sem_t *s) { if (s) { sem_destroy(&s->s); free(s); } }
int plat_cond_timedwait_rel(pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *rel) {
    uint64_t dl = plat_clock_ns(CLOCK_MONOTONIC) + (uint64_t)rel->tv_sec * 1000000000ull + (uint64_t)rel->tv_nsec;
    struct timespec ts = {(time_t)(dl / 1000000000ull), (long)(dl % 1000000000ull)};
    return pthread_cond_clockwait(cv, mu, CLOCK_MONOTONIC, &ts);
}

typedef struct qjob { void (*fn)(void *); void *ctx; struct qjob *next; } qjob;
struct plat_queue { pthread_mutex_t mu; pthread_cond_t cv; qjob *head, *tail; char name[16]; };
static void *queue_run(void *p) {
    plat_queue_t *q = p; plat_thread_set_name(q->name);
    for (;;) {
        pthread_mutex_lock(&q->mu);
        while (!q->head) pthread_cond_wait(&q->cv, &q->mu);
        qjob *j = q->head; q->head = j->next; if (!q->head) q->tail = NULL;
        pthread_mutex_unlock(&q->mu);
        j->fn(j->ctx); free(j);
    }
    return NULL;
}
plat_queue_t *plat_queue_create(const char *name) {
    plat_queue_t *q = calloc(1, sizeof *q); if (!q) die("work queue allocation failed");
    pthread_mutex_init(&q->mu, NULL); pthread_cond_init(&q->cv, NULL); snprintf(q->name, sizeof q->name, "%s", name);
    pthread_t t; if (pthread_create(&t, NULL, queue_run, q)) die("work queue %s: cannot start its thread", name);
    pthread_detach(t); return q;
}
void plat_queue_async(plat_queue_t *q, void (*fn)(void *), void *ctx) {
    qjob *j = malloc(sizeof *j); if (!j) die("work queue job allocation failed");
    *j = (qjob){fn, ctx, NULL};
    pthread_mutex_lock(&q->mu); if (q->tail) q->tail->next = j; else q->head = j; q->tail = j; pthread_cond_signal(&q->cv); pthread_mutex_unlock(&q->mu);
}
static void drain_mark(void *s) { sem_post(s); }
void plat_queue_drain(plat_queue_t *q) { sem_t s; sem_init(&s, 0, 0); plat_queue_async(q, drain_mark, &s); while (sem_wait(&s) && errno == EINTR) {} sem_destroy(&s); }
typedef struct { uint64_t ns; void (*fn)(void *); void *ctx; } after_job;
static void *after_run(void *p) { after_job j = *(after_job *)p; free(p); plat_wait_until(plat_abs_time() + j.ns); j.fn(j.ctx); return NULL; }
void plat_after_ns(uint64_t ns, void (*fn)(void *), void *ctx) {
    after_job *j = malloc(sizeof *j); pthread_t t; if (!j) return;
    *j = (after_job){ns, fn, ctx};
    if (pthread_create(&t, NULL, after_run, j)) { free(j); return; }
    pthread_detach(t);
}
static struct { int sig; void (*fn)(void *); void *ctx; } g_sigs[8]; static _Atomic int g_nsigs; static int g_sigpipe[2] = {-1, -1};
static void on_sig_pipe(int sig) { int e = errno; uint8_t b = (uint8_t)sig; if (write(g_sigpipe[1], &b, 1)) {} errno = e; }
static void *sig_run(void *p) {
    (void)p; plat_thread_set_name("dm signals");
    for (uint8_t b; read(g_sigpipe[0], &b, 1) == 1 || errno == EINTR;)
        for (int i = 0; i < atomic_load(&g_nsigs); i++) if (g_sigs[i].sig == b) g_sigs[i].fn(g_sigs[i].ctx);
    return NULL;
}
void plat_on_signal(int sig, void (*fn)(void *), void *ctx) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu);
    if (g_sigpipe[0] < 0) { pthread_t t; if (pipe2(g_sigpipe, O_CLOEXEC) || pthread_create(&t, NULL, sig_run, NULL)) die("signal thread: cannot start"); pthread_detach(t); }
    int n = atomic_load(&g_nsigs); if (n == 8) die("too many signal handlers");
    g_sigs[n].sig = sig; g_sigs[n].fn = fn; g_sigs[n].ctx = ctx; atomic_store(&g_nsigs, n + 1);
    struct sigaction sa = {.sa_handler = on_sig_pipe, .sa_flags = SA_RESTART | SA_ONSTACK}; sigemptyset(&sa.sa_mask); sigaction(sig, &sa, NULL);
    pthread_mutex_unlock(&mu);
}

// ---- Signal contexts ----
static struct _aarch64_ctx *uc_record(ucontext_t *uc, uint32_t magic) {
    uint8_t *p = (uint8_t *)uc->uc_mcontext.__reserved, *end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)p;
        if (!h->magic || !h->size) return NULL;
        if (h->magic == magic) return h;
        p += h->size;
    }
    return NULL;
}
struct fpsimd_context *plat_uc_fpsimd(ucontext_t *uc) {
    static struct fpsimd_context none;
    struct fpsimd_context *f = (struct fpsimd_context *)uc_record(uc, FPSIMD_MAGIC);
    return f ? f : &none;
}
uint64_t plat_uc_esr(ucontext_t *uc) { struct esr_context *e = (struct esr_context *)uc_record(uc, ESR_MAGIC); return e ? e->esr : 0; }

// ---- Files ----
int plat_fd_path(int fd, char *out, size_t n) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t k = readlink(p, out, n - 1); if (k <= 0) return -1;
    out[k] = 0; return 0;
}
static int copy_file(const char *from, const char *to, const struct stat *st) {
    int in = open(from, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st->st_mode & 07777); if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    uint8_t b[65536]; ssize_t k; int r = 0;
    while ((k = read(in, b, sizeof b)) != 0) {
        if (k < 0) { if (errno == EINTR) continue; r = -1; break; }
        for (ssize_t o = 0; o < k;) { ssize_t w = write(out, b + o, (size_t)(k - o)); if (w < 0) { if (errno == EINTR) continue; r = -1; break; } o += w; }
        if (r) break;
    }
    struct timespec t[2] = {st->st_atim, st->st_mtim}; futimens(out, t);
    int e = errno; close(in); if (close(out)) r = -1; errno = e; return r;
}
static void copy_xattrs(const char *from, const char *to) {
    char names[1024]; ssize_t n = llistxattr(from, names, sizeof names);
    for (ssize_t o = 0; n > 0 && o < n; o += (ssize_t)strlen(names + o) + 1) {
        char v[1024]; ssize_t k = lgetxattr(from, names + o, v, sizeof v);
        if (k >= 0) lsetxattr(to, names + o, v, (size_t)k, 0);
    }
}
int plat_clone_tree(const char *from, const char *to) {
    struct stat st; if (lstat(from, &st)) return -1;
    if (S_ISLNK(st.st_mode)) { char l[4096]; ssize_t k = readlink(from, l, sizeof l - 1); if (k < 0) return -1; l[k] = 0; return symlink(l, to); }
    if (!S_ISDIR(st.st_mode)) { if (copy_file(from, to, &st)) return -1; copy_xattrs(from, to); return 0; }
    if (mkdir(to, st.st_mode & 07777)) return -1;
    DIR *d = opendir(from); if (!d) return -1;
    int r = 0;
    for (struct dirent *e; !r && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[4096], b[4096];
        if (snprintf(a, sizeof a, "%s/%s", from, e->d_name) >= (int)sizeof a || snprintf(b, sizeof b, "%s/%s", to, e->d_name) >= (int)sizeof b) { errno = ENAMETOOLONG; r = -1; break; }
        r = plat_clone_tree(a, b);
    }
    int e = errno; closedir(d); copy_xattrs(from, to);
    struct timespec t[2] = {st.st_atim, st.st_mtim}; utimensat(AT_FDCWD, to, t, AT_SYMLINK_NOFOLLOW);
    errno = e; return r;
}
static int swap_rm_cb(const char *p, const struct stat *st, int f, struct FTW *w) { (void)st; (void)f; (void)w; return remove(p); }
static int swap_rm_tree(const char *p) { return nftw(p, swap_rm_cb, 16, FTW_DEPTH | FTW_PHYS); }
static int swap_sync_cb(const char *p, const struct stat *st, int f, struct FTW *w) {
    (void)st; (void)w; if (f != FTW_F && f != FTW_D && f != FTW_DP) return 0;
    int fd = open(p, O_RDONLY | O_CLOEXEC | (f == FTW_F ? 0 : O_DIRECTORY)); if (fd < 0) return -1;
    int r = fsync(fd), e = errno; close(fd);
    if (r && (e == EINVAL || e == EROFS || e == ENOSYS || e == ENOTSUP)) r = 0;
    errno = e; return r;
}
static int swap_sync_tree(const char *p) { return nftw(p, swap_sync_cb, 16, FTW_DEPTH | FTW_PHYS); }
static int swap_stage1(const char *a, const char *b) {
    DIR *d = opendir(b); if (!d) return errno == ENOENT ? 0 : -1;
    int r = 0;
    for (struct dirent *e; !r && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char pa[4096], pb[4096];
        if (snprintf(pa, sizeof pa, "%s/%s", a, e->d_name) >= (int)sizeof pa || snprintf(pb, sizeof pb, "%s/%s", b, e->d_name) >= (int)sizeof pb) { r = -1; errno = ENAMETOOLONG; break; }
        struct stat sa, sb;
        if (lstat(pb, &sb)) continue;
        if (lstat(pa, &sa)) r = swap_rm_tree(pb);
        else if (S_ISDIR(sb.st_mode) && S_ISDIR(sa.st_mode)) r = swap_stage1(pa, pb);
        else if (S_ISDIR(sb.st_mode) != S_ISDIR(sa.st_mode)) r = swap_rm_tree(pb);
    }
    int er = errno; closedir(d); errno = er; return r;
}
static int swap_stage2(const char *a, const char *b) {
    DIR *d = opendir(a); if (!d) return errno == ENOENT ? 0 : -1;
    int r = 0;
    for (struct dirent *e; !r && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char pa[4096], pb[4096];
        if (snprintf(pa, sizeof pa, "%s/%s", a, e->d_name) >= (int)sizeof pa || snprintf(pb, sizeof pb, "%s/%s", b, e->d_name) >= (int)sizeof pb) { r = -1; errno = ENAMETOOLONG; break; }
        struct stat sa; if (lstat(pa, &sa)) continue;
        if (S_ISDIR(sa.st_mode)) { if (mkdir(pb, sa.st_mode & 07777) && errno != EEXIST) r = -1; else if (!(r = swap_stage2(pa, pb))) rmdir(pa); }
        else r = rename(pa, pb);
    }
    int er = errno; closedir(d); errno = er; return r;
}
static int swap_marker(const char *m, const char *a) {
    int fd = open(m, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); if (fd < 0) return -1;
    size_t n = strlen(a); int r = write(fd, a, n) == (ssize_t)n ? fsync(fd) : -1; int e = errno; close(fd); errno = e; return r;
}
static void swap_dir_sync(const char *b) {
    char d[4200]; snprintf(d, sizeof d, "%s", b); char *s = strrchr(d, '/'); if (s) { if (s == d) s[1] = 0; else *s = 0; } else strcpy(d, ".");
    int fd = open(d, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (fd >= 0) { fsync(fd); close(fd); }
}
static int swap_has_files(const char *a) {
    DIR *d = opendir(a); if (!d) return 0;
    int r = 0;
    for (struct dirent *e; !r && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[4096]; struct stat st;
        if (snprintf(p, sizeof p, "%s/%s", a, e->d_name) >= (int)sizeof p || lstat(p, &st)) { r = 1; break; }
        r = S_ISDIR(st.st_mode) ? swap_has_files(p) : 1;
    }
    closedir(d); return r;
}
static int swap_apply(const char *a, const char *b, int stage1) {
    char m1[4200], m2[4200]; snprintf(m1, sizeof m1, "%s.pend1", b); snprintf(m2, sizeof m2, "%s.pend2", b);
    if (stage1) {
        if (swap_stage1(a, b)) return -1;
        if (swap_marker(m2, a) || unlink(m1)) return -1;
    } else if (unlink(m1) && errno != ENOENT) return -1;
    swap_dir_sync(b);
    if (swap_stage2(a, b)) return -1;
    swap_sync_tree(b);
    unlink(m2); swap_dir_sync(b); return 0;
}
int plat_rename_swap(const char *a, const char *b) {
    if (swap_sync_tree(a)) return -1;
    if (!syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, 2 /* RENAME_EXCHANGE */)) { swap_dir_sync(b); return 0; }
    if (errno != EINVAL && errno != ENOSYS && errno != EXDEV && errno != ENOTSUP) return -1;
    char m1[4200]; if (snprintf(m1, sizeof m1, "%s.pend1", b) >= (int)sizeof m1) { errno = ENAMETOOLONG; return -1; }
    if (swap_marker(m1, a)) return -1;
    return swap_apply(a, b, 1);
}
static int swap_marker_ok(const char *a, const char *b) {
    const char *sa = strrchr(a, '/'), *sb = strrchr(b, '/');
    const char *na = sa ? sa + 1 : a, *nb = sb ? sb + 1 : b;
    size_t lb = strlen(nb);
    if (strncmp(na, nb, lb) || (strcmp(na + lb, ".new") && strcmp(na + lb, ".re") && strcmp(na + lb, ".restore"))) return 0;
    char pa[4096], pb[4096], ra[PATH_MAX], rb[PATH_MAX];
    snprintf(pa, sizeof pa, "%.*s", sa ? (int)(sa - a) : 1, sa ? a : ".");
    snprintf(pb, sizeof pb, "%.*s", sb ? (int)(sb - b) : 1, sb ? b : ".");
    if (!pa[0]) strcpy(pa, "/");
    if (!pb[0]) strcpy(pb, "/");
    return realpath(pa, ra) && realpath(pb, rb) && !strcmp(ra, rb);
}
void plat_swap_recover(const char *b) {
    char m[4200], a[4096];
    {
        char o[4200]; struct stat st; snprintf(o, sizeof o, "%s.old", b);
        if (!lstat(o, &st)) { if (lstat(b, &st)) rename(o, b); else swap_rm_tree(o); }
    }
    for (int k = 2; k >= 1; k--) {
        snprintf(m, sizeof m, "%s.pend%d", b, k);
        int fd = open(m, O_RDONLY | O_CLOEXEC); if (fd < 0) continue;
        ssize_t n = read(fd, a, sizeof a - 1); close(fd);
        if (n <= 0) { unlink(m); continue; }
        a[n] = 0;
        while (n > 0 && (a[n - 1] == '\n' || a[n - 1] == '\r' || a[n - 1] == ' ')) a[--n] = 0;
        struct stat st;
        if (!swap_marker_ok(a, b)) { LOG("[RED] save swap journal %s names %s, not %s.new / .re / .restore: refused, the save is left as it is", m, a, b); return; }
        if (k == 1 && (lstat(a, &st) || !S_ISDIR(st.st_mode))) { LOG("[RED] save swap journal %s names %s, which is gone: stage 1 refused (it would empty the save), the save is left as it is", m, a); return; }
        if (k == 1 && !swap_has_files(a) && swap_has_files(b)) {
            LOG("[RED] save swap journal %s names %s, which holds no files while %s does: stage 1 refused (it would empty the save); the marker and the empty folder go, the save stays", m, a, b);
            unlink(m); swap_dir_sync(b); swap_rm_tree(a); return;
        }
        if (!swap_apply(a, b, k == 1)) swap_rm_tree(a);
        return;
    }
}
static const char *xname(const char *name, char *b, size_t n) { snprintf(b, n, "user.%s", name); return b; }
static int xattr_absent(int r) { if (r < 0 && errno == EOPNOTSUPP) errno = PLAT_ENOATTR; return r; }
ssize_t plat_getxattr(const char *path, const char *name, void *buf, size_t n) { char x[256]; ssize_t r = lgetxattr(path, xname(name, x, sizeof x), buf, n); xattr_absent((int)r); return r; }
int plat_setxattr(const char *path, const char *name, const void *buf, size_t n) { char x[256]; return lsetxattr(path, xname(name, x, sizeof x), buf, n, 0); }
int plat_removexattr(const char *path, const char *name) { char x[256]; return xattr_absent(lremovexattr(path, xname(name, x, sizeof x))); }

// ---- Device ----
// Read PRETTY_NAME from /etc/os-release (e.g. "Armada 20261109 (Fedora bootc 41)"); fall back to kernel release
void plat_device_model(char *out, size_t n) {
    FILE *f = fopen("/etc/os-release", "re");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "PRETTY_NAME=", 12)) continue;
            char *v = line + 12; size_t l = strlen(v);
            while (l && (v[l - 1] == '\n' || v[l - 1] == '\r' || v[l - 1] == '"')) v[--l] = 0;
            if (*v == '"') { v++; l--; }
            snprintf(out, n, "%s", v); fclose(f); return;
        }
        fclose(f);
    }
    struct utsname u; if (!uname(&u)) snprintf(out, n, "Linux %s", u.release); else snprintf(out, n, "Linux");
}
void plat_build_commit(char *out, size_t n) { const char *c = getenv("DM_BUILD_COMMIT"); if (c && *c) snprintf(out, n, "%s", c); }

// ---- SHA-256 (FIPS 180-4) ----
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; unsigned nbuf; } sha_state;
_Static_assert(sizeof(sha_state) <= sizeof(plat_sha256_t), "plat_sha256_t holds the state");
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(sha_state *s, const uint8_t *p) {
    uint32_t w[64], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3, s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i], t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
void plat_sha256_init(plat_sha256_t *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    sha_state *s = (sha_state *)c; memcpy(s->h, iv, sizeof iv); s->len = 0; s->nbuf = 0;
}
void plat_sha256_update(plat_sha256_t *c, const void *data, size_t n) {
    sha_state *s = (sha_state *)c; const uint8_t *p = data; s->len += n;
    if (s->nbuf) { size_t k = 64 - s->nbuf < n ? 64 - s->nbuf : n; memcpy(s->buf + s->nbuf, p, k); s->nbuf += (unsigned)k; p += k; n -= k; if (s->nbuf == 64) { sha_block(s, s->buf); s->nbuf = 0; } }
    for (; n >= 64; p += 64, n -= 64) sha_block(s, p);
    if (n) { memcpy(s->buf, p, n); s->nbuf = (unsigned)n; }
}
void plat_sha256_final(uint8_t out[32], plat_sha256_t *c) {
    sha_state *s = (sha_state *)c; uint64_t bits = s->len * 8; uint8_t pad[72] = {0x80};
    size_t k = s->nbuf < 56 ? 56 - s->nbuf : 120 - s->nbuf;
    for (int i = 0; i < 8; i++) pad[k + i] = (uint8_t)(bits >> (56 - 8 * i));
    plat_sha256_update(c, pad, k + 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}
void plat_sha256(const void *p, size_t n, uint8_t out[32]) { plat_sha256_t c; plat_sha256_init(&c); plat_sha256_update(&c, p, n); plat_sha256_final(out, &c); }

// ---- Sorting ----
static void msort(uint8_t *a, uint8_t *t, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    if (n < 2) return;
    size_t h = n / 2; msort(a, t, h, sz, cmp); msort(a + h * sz, t, n - h, sz, cmp);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) { if (cmp(a + j * sz, a + i * sz) < 0) memcpy(t + k++ * sz, a + j++ * sz, sz); else memcpy(t + k++ * sz, a + i++ * sz, sz); }
    while (i < h) memcpy(t + k++ * sz, a + i++ * sz, sz);
    while (j < n) memcpy(t + k++ * sz, a + j++ * sz, sz);
    memcpy(a, t, n * sz);
}
int plat_sort_stable(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    uint8_t *t = malloc(n * size ? n * size : 1); if (!t) return -1;
    msort(base, t, n, size, cmp); free(t); return 0;
}
