/*
 * s23_savestate.c -- SAVE STATES FOR TESTS (our fork): run once to a frame and save, then start as many runs as needed from there.
 *
 *   S23_SAVESTATE=<file>:<frame>   save at the first interrupt the main CPU takes at or after that frame (the run goes on)
 *   S23_LOADSTATE=<file>           start from it (the run starts normally and is replaced at its frame 2; the same build only)
 *
 * WHAT IS SAVED: all the game's own static data -- between the markers of src/s23_ss_begin.c and src/s23_ss_end.c, which the build links
 * first and last of the game's objects: the main RAM, the register file, the H8s, the GMEN board, the chips, the timers, the static locals.
 * Not SDL's, not the C library's, not this file's (linked after the end marker).
 * WHERE: the main CPU's interrupt entry (src/s23_cpu.c take_exception: EPC and Status set, the vector next). The lifted code keeps its call
 * chain on the C stack; a loaded run starts at the vector with an empty shadow stack, and the handler's eret then JUMPS back to EPC (the
 * runtime already treats a return with no frame as a jump).
 * POINTERS: a word that held a pointer in the saving run (it pointed into memory that run had: heap, image, stack) keeps the loading
 * run's value -- the ROMs, buffers, threads and strings of this process stay valid.
 * DEBUG SWITCHES read once into a static (S23_PRESS, S23_SHOTLIST, ...) register with S23_SS_ENV(var) before they parse; a load puts them
 * back to their first value, so the LOADING run's switches apply.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#ifdef _WIN32
#include <windows.h>
#define S23_IMAGE_BASE ((uintptr_t)GetModuleHandleA(NULL))   /* (S23_SSLOG: an address as an offset in the .exe) */
#else
#define S23_IMAGE_BASE ((uintptr_t)0)                         /* (Linux builds: the address itself) */
#endif

extern char s23_ss_data_begin[], s23_ss_data_end[], s23_ss_bss_begin[], s23_ss_bss_end[];
extern uint32_t rr_frame;
void s23_render_wait_idle(void);
int rr_ss_cont(uint32_t *cont);
extern int rr_ss_resumed; extern uint32_t rr_ss_after_irq;

jmp_buf s23_ss_jb;
int s23_ss_jb_set;
uint32_t s23_ss_resume_pc;

#define MAGIC "S23SAVE2"
static uint8_t *const sec_lo[2] = { (uint8_t *)s23_ss_data_begin, (uint8_t *)s23_ss_bss_begin };
static uint8_t *const sec_hi[2] = { (uint8_t *)s23_ss_data_end, (uint8_t *)s23_ss_bss_end };

/* ---- the switches' statics (S23_SS_ENV) ---- */
typedef struct { uint32_t sec, off, n; uint8_t *first; } env_var;
static env_var envs[96]; static int nenv;
void s23_ss_env(void *p, size_t n)
{
    uint8_t *a = p;
    for (int s = 0; s < 2; s++) if (a >= sec_lo[s] && a + n <= sec_hi[s]) {
        const uint32_t off = (uint32_t)(a - sec_lo[s]);
        for (int i = 0; i < nenv; i++) if (envs[i].sec == (uint32_t)s && envs[i].off == off) return;
        if (nenv == 96) return;
        envs[nenv].sec = (uint32_t)s; envs[nenv].off = off; envs[nenv].n = (uint32_t)n;
        envs[nenv].first = malloc(n); memcpy(envs[nenv].first, a, n); nenv++;
        return;
    }
}

/* ---- THIS PROCESS'S OWN values inside the saved state (s23_ss_keep): a load leaves them as they are -- the backup file's name and
 * whether it is written, the software renderer on / off (a headless load of a window's capture crashed writing a file with no name) ---- */
typedef struct { uint8_t *p; size_t n; uint8_t *tmp; } keep_var;
static keep_var keeps[32]; static int nkeep;
void s23_ss_keep(void *p, size_t n)
{
    for (int i = 0; i < nkeep; i++) if (keeps[i].p == p) return;
    if (nkeep < 32) { keeps[nkeep].p = p; keeps[nkeep].n = n; keeps[nkeep].tmp = malloc(n); nkeep++; }
}

/* ---- the memory this process has (a word pointing into it was a pointer) ---- */
typedef struct { uint64_t lo, hi; } region;
static region *map_of(int *n)
{
    region *r = NULL; int cap = 0; *n = 0;
#ifdef _WIN32
    uint8_t *a = NULL; MEMORY_BASIC_INFORMATION mb;
    while (VirtualQuery(a, &mb, sizeof mb) == sizeof mb) {
        if (mb.State == MEM_COMMIT || mb.State == MEM_RESERVE) {
            if (*n == cap) { cap = cap ? cap * 2 : 1024; r = realloc(r, (size_t)cap * sizeof *r); }
            r[*n].lo = (uint64_t)(uintptr_t)mb.BaseAddress; r[*n].hi = r[*n].lo + mb.RegionSize; (*n)++;
        }
        uint8_t *next = (uint8_t *)mb.BaseAddress + mb.RegionSize;
        if (next <= a) break;
        a = next;
    }
#endif
    return r;
}
static int in_map(const region *r, int n, uint64_t v)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) { const int m = (lo + hi) / 2; if (v < r[m].lo) hi = m - 1; else if (v >= r[m].hi) lo = m + 1; else return 1; }
    return 0;
}

static int wr(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }
static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static void save(const char *path)
{
    s23_render_wait_idle();
    int nr; region *r = map_of(&nr);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[SAVE] cannot write %s\n", path); free(r); return; }
    uint64_t len[2] = { (uint64_t)(sec_hi[0] - sec_lo[0]), (uint64_t)(sec_hi[1] - sec_lo[1]) };
    uint32_t cont = 0; rr_ss_cont(&cont);
    uint32_t hdr[4] = { (uint32_t)nr, (uint32_t)nenv, rr_frame, cont };
    int ok = wr(f, MAGIC, 8) && wr(f, len, sizeof len) && wr(f, hdr, sizeof hdr) && wr(f, r, (size_t)nr * sizeof *r);
    for (int i = 0; i < nenv && ok; i++) ok = wr(f, &envs[i], 3 * sizeof(uint32_t)) && wr(f, envs[i].first, envs[i].n);
    for (int s = 0; s < 2 && ok; s++) ok = wr(f, sec_lo[s], (size_t)len[s]);
    fclose(f); free(r);
    fprintf(stderr, "[SAVE] frame %u saved to %s (%llu + %llu bytes)%s\n", rr_frame, path, (unsigned long long)len[0], (unsigned long long)len[1], ok ? "" : " -- WRITE FAILED");
}

static int load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[LOAD] cannot read %s\n", path); return 0; }
    char m[8]; uint64_t len[2]; uint32_t hdr[4];
    if (!rd(f, m, 8) || memcmp(m, MAGIC, 8) || !rd(f, len, sizeof len) || !rd(f, hdr, sizeof hdr)) { fprintf(stderr, "[LOAD] %s: not a save state\n", path); fclose(f); return 0; }
    for (int s = 0; s < 2; s++) if (len[s] != (uint64_t)(sec_hi[s] - sec_lo[s])) {
        fprintf(stderr, "[LOAD] %s was saved by another build (its state is %llu bytes, this one's %llu): not loaded\n", path, (unsigned long long)len[s], (unsigned long long)(sec_hi[s] - sec_lo[s]));
        fclose(f); return 0; }
    const int nr = (int)hdr[0], nev = (int)hdr[1];
    region *r = malloc((size_t)(nr ? nr : 1) * sizeof *r);
    int ok = rd(f, r, (size_t)nr * sizeof *r);
    env_var *ev = calloc((size_t)(nev ? nev : 1), sizeof *ev);
    for (int i = 0; i < nev && ok; i++) { ok = rd(f, &ev[i], 3 * sizeof(uint32_t)); if (ok) { ev[i].first = malloc(ev[i].n); ok = rd(f, ev[i].first, ev[i].n); } }
    s23_render_wait_idle();
    for (int i = 0; i < nkeep; i++) memcpy(keeps[i].tmp, keeps[i].p, keeps[i].n);
    unsigned long kept = 0;
    for (int s = 0; s < 2 && ok; s++) {
        uint8_t *buf = malloc((size_t)len[s]);
        if (!buf || !rd(f, buf, (size_t)len[s])) { ok = 0; free(buf); break; }
        uint8_t *base = sec_lo[s]; size_t i = 0;
        for (; i < (size_t)len[s] && ((uintptr_t)(base + i) & 7); i++) base[i] = buf[i];   /* (up to the first aligned word) */
        for (; i + 8 <= (size_t)len[s]; i += 8) {
            uint64_t cur, sav; memcpy(&cur, base + i, 8); memcpy(&sav, buf + i, 8);
            if (cur == sav) continue;
            if (sav >= 0x100000000ull && in_map(r, nr, sav)) { kept++; if (getenv("S23_SSLOG")) fprintf(stderr, "[LOAD] kept %s+%zX (image +%llX): %016llX (saved %016llX)\n", s ? "bss" : "data", i, (unsigned long long)((uintptr_t)(base + i) - S23_IMAGE_BASE), (unsigned long long)cur, (unsigned long long)sav); continue; }
            memcpy(base + i, &sav, 8);
        }
        for (; i < (size_t)len[s]; i++) base[i] = buf[i];
        free(buf);
    }
    fclose(f);
    if (!ok) { fprintf(stderr, "[LOAD] %s: cut short -- this run is now broken\n", path); exit(3); }
    /* the debug switches: back to their first value (both runs' lists), so this run's own are read again */
    for (int i = 0; i < nev; i++) if (ev[i].sec < 2 && ev[i].off + ev[i].n <= (uint64_t)(sec_hi[ev[i].sec] - sec_lo[ev[i].sec])) memcpy(sec_lo[ev[i].sec] + ev[i].off, ev[i].first, ev[i].n);
    for (int i = 0; i < nenv; i++) memcpy(sec_lo[envs[i].sec] + envs[i].off, envs[i].first, envs[i].n);
    for (int i = 0; i < nkeep; i++) memcpy(keeps[i].p, keeps[i].tmp, keeps[i].n);
    for (int i = 0; i < nev; i++) free(ev[i].first);
    free(ev); free(r);
    rr_ss_after_irq = hdr[3];
    fprintf(stderr, "[LOAD] %s: frame %u (%lu pointers kept), carries on at %08X after the interrupt\n", path, rr_frame, kept, hdr[3]);
    return 1;
}

static int sframe = -1, saved; static char spath[512];
/* a save at the next interrupt (the window's F9, src/s23_window.c) */
void s23_ss_request(const char *path) { snprintf(spath, sizeof spath, "%s", path); sframe = 0; saved = 0; }
/* the main CPU is about to take an interrupt (src/s23_cpu.c take_exception, its vector next): save or load here */
void s23_ss_irq_point(uint32_t vec)
{
    static int init, loaded; static char lpath[512];
    if (!init) {
        init = 1;
        const char *e = getenv("S23_SAVESTATE");
        if (e && sframe < 0) { snprintf(spath, sizeof spath, "%s", e); char *c = strrchr(spath, ':'); if (c && c > spath + 1) { *c = 0; sframe = atoi(c + 1); } else sframe = -1; }
        e = getenv("S23_LOADSTATE"); if (e) snprintf(lpath, sizeof lpath, "%s", e);
    }
    { uint32_t c; if (sframe >= 0 && !saved && (int)rr_frame >= sframe && rr_ss_cont(&c)) { saved = 1; save(spath); } }   /* (at a poll whose continuation decodes) */
    if (lpath[0] && !loaded && rr_frame >= 2 && s23_ss_jb_set) {
        loaded = 1;
        if (!load(lpath)) return;
        rr_ss_resumed = 1;                           /* the saved shadow stack stays: returns into it unwind to the host (src/s23_cpu.c) */
        s23_ss_resume_pc = vec;
        longjmp(s23_ss_jb, 1);
    }
}
