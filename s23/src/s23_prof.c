/* s23_prof.c -- A SAMPLING PROFILER FOR TESTS (our fork). $S23_PROFILE=1: a thread stops every other thread of this process about
 * 1000 times a second and notes where it was (its instruction pointer, relative to the program's image); at exit, profile.txt beside
 * the game: "thread-id rva count" per spot, plus "ext" for time outside the program (drivers, the C library). tools: gunwars/tools/prof.py
 * maps the addresses to function names (the unstripped build's symbol table). Windows only. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
__declspec(dllimport) UINT WINAPI timeBeginPeriod(UINT);   /* (winmm; mmsystem.h clashes with the build's defines) */

#define NSPOT 65536
typedef struct { DWORD tid; uint32_t rva; uint32_t n; } spot;
static spot *spots; static int nspots;
static volatile int running;
static uint64_t img_lo, img_hi, total, ext_n;
static DWORD me;

typedef struct { DWORD tid; uint64_t mod; uint32_t n; } ext_spot;
static ext_spot exts[512]; static int nexts;
static void add(DWORD tid, uint64_t ip)
{
    total++;
    if (ip < img_lo || ip >= img_hi) {                              /* outside the program: which DLL */
        ext_n++;
        HMODULE m = NULL; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)ip, &m);
        const uint64_t mb = (uint64_t)(uintptr_t)m;
        int i = 0;
        for (; i < nexts; i++) if (exts[i].tid == tid && exts[i].mod == mb) { exts[i].n++; break; }
        if (i == nexts && nexts < 512) { exts[nexts].tid = tid; exts[nexts].mod = mb; exts[nexts].n = 1; nexts++; }
        ip = 0;
    }
    const uint32_t rva = (uint32_t)(ip ? ip - img_lo : 0);
    uint32_t h = (rva * 2654435761u ^ tid) & (NSPOT - 1);
    for (int k = 0; k < NSPOT; k++, h = (h + 1) & (NSPOT - 1)) {
        if (spots[h].n == 0) { spots[h].tid = tid; spots[h].rva = rva; spots[h].n = 1; nspots++; return; }
        if (spots[h].tid == tid && spots[h].rva == rva) { spots[h].n++; return; }
    }
}
static DWORD WINAPI sampler(LPVOID arg)
{
    (void)arg;
    timeBeginPeriod(1);
    DWORD tids[64]; int nt = 0; DWORD listed = 0;
    while (running) {
        const DWORD now = GetTickCount();
        if (now - listed > 500) {                                   /* the threads, twice a second */
            listed = now; nt = 0;
            HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te; te.dwSize = sizeof te;
            if (s != INVALID_HANDLE_VALUE) { if (Thread32First(s, &te)) do { if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != me && nt < 64) tids[nt++] = te.th32ThreadID; } while (Thread32Next(s, &te)); CloseHandle(s); }
        }
        for (int i = 0; i < nt; i++) {
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[i]);
            if (!t) continue;
            if (SuspendThread(t) != (DWORD)-1) {
                CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(t, &c)) add(tids[i], c.Rip);
                ResumeThread(t);
            }
            CloseHandle(t);
        }
        Sleep(1);
    }
    return 0;
}
void s23_prof_dump(void);
void s23_prof_start(void)
{
    if (!getenv("S23_PROFILE") || spots) return;
    spots = calloc(NSPOT, sizeof *spots);
    HMODULE m = GetModuleHandleA(NULL);
    img_lo = (uint64_t)(uintptr_t)m;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    img_hi = img_lo + nt->OptionalHeader.SizeOfImage;
    running = 1;
    CreateThread(NULL, 0, sampler, NULL, 0, &me);
    atexit(s23_prof_dump);                                          /* every way out (the window's close too) */
    fprintf(stderr, "[PROF] sampling every thread ~1000 times a second -> profile.txt at exit\n");
}
void s23_prof_dump(void)
{
    static int done; if (!spots || done) return; done = 1;
    running = 0; Sleep(5);
    FILE *f = fopen("profile.txt", "w");
    if (!f) return;
    fprintf(f, "# samples %llu, outside the program %llu, image base %llX\n", (unsigned long long)total, (unsigned long long)ext_n, (unsigned long long)img_lo);
    for (int h = 0; h < NSPOT; h++) if (spots[h].n) fprintf(f, "%lu %X %u\n", (unsigned long)spots[h].tid, spots[h].rva, spots[h].n);
    for (int i = 0; i < nexts; i++) {                               /* "ext <thread> <dll> <count>" */
        char nm[MAX_PATH] = "?"; if (exts[i].mod) GetModuleFileNameA((HMODULE)(uintptr_t)exts[i].mod, nm, sizeof nm);
        const char *b = strrchr(nm, '\\'); fprintf(f, "ext %lu %s %u\n", (unsigned long)exts[i].tid, b ? b + 1 : nm, exts[i].n);
    }
    fclose(f);
    fprintf(stderr, "[PROF] %llu samples in profile.txt\n", (unsigned long long)total);
}
#else
void s23_prof_start(void) {}
void s23_prof_dump(void) {}
#endif
