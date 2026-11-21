/* link_host.c -- see link_host.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link_host.h"
#include "link_net.h"
#include "lan_via.h"
#include "lan139.h"
#include "eng_timing.h"
#include "eng_party.h"
#include "eng_tile.h"
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#endif

bool g_link_mute;

static lnk_cfg cfg = { .id = -1, .nodes = 0, .base_port = 22220, .gather_ms = 60000 };
static const link_host_game *G;
static bool on;
static int  skew_ppm;                                                  /* TEST (--link-test D:J:L:S): this copy's clock S ppm fast (negative: slow) */
static uint32_t down_frames, down_now, up_run;                         /* frames the GAME saw its link down (counted after 2 s of a good link) */
static bool was_up;
static SDL_Window *win;                                                /* the window, for the title's warnings */
static char base_title[256];                                           /* the title after the start-up gathering */
static uint32_t dup_shown;                                             /* the clash last warned about (bit per cabinet) */
static bool from_cmdline;                                              /* --link on the command line (a one-PC test launcher): link.cfg is not used */
static char **argv0;                                                   /* the command line, for a restart */
static void put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void disc_poll(void);
static void disc_start_responder(int base_port, const char *title);
static void restart_all_now(void);
static int  file_port(void);
static void disc_find(void);
static char dup_msg[128], setup_msg_shown[160];                        /* the title's warnings: two cabinets with one game ID; set-ups that differ */
static void set_title(void);
static void setup_msg(char *m, size_t n);

bool link_host_on(void) { return on; }
int  link_host_id(void) { return cfg.id; }


bool link_host_arg(int argc, char **argv, int *i)
{
    const char *a = argv[*i];
    bool v = *i + 1 < argc;
    if (!strcmp(a, "--link") && v) cfg.id = atoi(argv[++*i]) - 1;
    else if (!strcmp(a, "--link-cabinets") && v) cfg.nodes = atoi(argv[++*i]);
    else if (!strcmp(a, "--link-peers") && v) cfg.peers = argv[++*i];
    else if (!strcmp(a, "--link-port") && v) cfg.base_port = atoi(argv[++*i]);
    else if (!strcmp(a, "--link-wait") && v) cfg.gather_ms = atoi(argv[++*i]) * 1000;
    else if (!strcmp(a, "--link-test") && v) sscanf(argv[++*i], "%d:%d:%d:%d:%d:%d", &cfg.delay_ms, &cfg.jitter_ms, &cfg.loss_pct, &skew_ppm,
                                                    &cfg.burst_every_ms, &cfg.burst_hold_ms);
    else if (!strcmp(a, "--mute")) g_link_mute = true;
    else if (!strcmp(a, "--party") || !strcmp(a, "--party-taskbar")) {}                                 /* (read by link_host_prescan) */
    else if (!strcmp(a, "--pad") && v) ++*i;
    else if (!strcmp(a, "--timing") && v) ++*i;                       /* (read by link_host_prescan) */
    else return false;
    return true;
}

/* THIS PROGRAM: a fingerprint of the file (every copy of the same build has the same) and its date, told to the others (version check) */
static void own_build(uint32_t *crc, uint32_t *when)
{
    char path[1024] = "";
#ifdef _WIN32
    wchar_t w[1024]; const DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (n > 0 && n < 1024) WideCharToMultiByte(CP_UTF8, 0, w, -1, path, sizeof path, NULL, NULL);
    FILE *f = _wfopen(w, L"rb");
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1); if (n > 0) path[n] = 0;
    FILE *f = fopen(path, "rb");
#endif
    uLong c = crc32(0L, Z_NULL, 0);
    if (f) {
        static unsigned char buf[1 << 16]; size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) c = crc32(c, buf, (uInt)got);
        fclose(f);
    }
    *crc = (uint32_t)c ? (uint32_t)c : 1;
    { const char *e = getenv("LINK_TEST_BUILD"); if (e && *e) *crc = (uint32_t)strtoul(e, NULL, 16); }   /* TEST: pretend to be another build */
    struct stat st; *when = stat(path, &st) == 0 ? (uint32_t)st.st_mtime : 0;
    char d[32] = "?"; time_t tt = (time_t)*when; struct tm *tm = localtime(&tt); if (tm) strftime(d, sizeof d, "%d %b %H:%M", tm);
    fprintf(stderr, "[LINK] this program: build %08X, dated %s\n", *crc, d);
}

bool link_host_open(const link_host_game *g)
{
    if (cfg.id < 0) return true;                                       /* not linked */
    G = g;
    lan139_set_per_frame(g->rx_per_frame);
    cfg.tag = g->tag;
    if (!cfg.nodes) {                                                  /* the set's size: --link-cabinets, else the addresses given, else 4 */
        cfg.nodes = 4;
        if (cfg.peers) { cfg.nodes = 1; for (const char *c = cfg.peers; *c; c++) cfg.nodes += *c == ','; }
    }
    if (g->max_cabinets && cfg.nodes > g->max_cabinets) {            /* more than the game can number: two cabinets would share a number */
        fprintf(stderr, "[LINK] %d cabinets set, but this game links at most %d: using %d\n", cfg.nodes, g->max_cabinets, g->max_cabinets);
        cfg.nodes = g->max_cabinets;
    }
    if (cfg.id >= cfg.nodes) { fprintf(stderr, "[LINK] cabinet %d: this game links at most %d cabinets: not linked\n", cfg.id + 1, cfg.nodes); cfg.id = -1; return true; }
    own_build(&cfg.build_crc, &cfg.build_time);
    if (!lnk_open(&cfg)) return false;
    atexit(lnk_close);                                                 /* however the game ends, the others hear it go */
    on = true;
    return true;
}

/* the copies of a set on one PC (no addresses given): their windows in a grid by cabinet number, so none has to be dragged apart --
 * while a window is dragged Windows holds its game still, and a cabinet that stops for more than a tenth of a second drops out */
static bool party_taskbar;                                             /* --party-taskbar: the split screen stops above the task bar */
static bool party;                                                     /* --party: the copies on one PC as a split screen, a player at each */

/* PARTY SYNC. Copies on ONE PC share its graphics chip: when one of them stutters (new scenery, a texture upload) the others run on,
 * and a cabinet that is 8 frames late is dropped by the games' link until everything restarts. So the copies of a party keep in step:
 * each writes how many frames it has run into a small piece of shared memory, and one that is more than a frame ahead of another
 * waits for it (hellos still going out). A stutter then holds every picture for a moment instead of losing a player. A copy that
 * stops altogether (closed, crashed) is waited for 0.3 s once and then ignored -- the link's own rules take over. One PC only:
 * real cabinets are never held up by each other. */
typedef struct { volatile uint32_t frame[LNK_MAX_NODES], beat[LNK_MAX_NODES]; } party_shm;
static party_shm *psh;
static uint32_t party_waited_ms, party_gave_up;
static uint32_t sys_ms(void)                                           /* one clock for every copy (SDL's starts with each program) */
{
#ifdef _WIN32
    return GetTickCount();
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
#endif
}
static void party_open(void)
{
    char name[64]; snprintf(name, sizeof name, "n22_party_%d", cfg.base_port);
#ifdef _WIN32
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(party_shm), name);
    if (h) psh = (party_shm *)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(party_shm));
#else
    char path[96]; snprintf(path, sizeof path, "/tmp/%s", name);
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd >= 0 && ftruncate(fd, sizeof(party_shm)) == 0) { void *m = mmap(NULL, sizeof(party_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); if (m != MAP_FAILED) psh = m; }
    if (fd >= 0) close(fd);
#endif
    if (psh) { psh->frame[cfg.id] = 0; psh->beat[cfg.id] = 0; }
    fprintf(stderr, "[LINK] party: the copies on this PC keep in step%s\n", psh ? "" : " -- NOT available (no shared memory)");
}
static void party_sync(void)
{
    if (!psh || lnk_state() != LNK_LINKED) return;
    static uint32_t n;
    psh->frame[cfg.id] = ++n; psh->beat[cfg.id] = sys_ms();
    const uint32_t t0 = sys_ms();
    for (;;) {
        const uint32_t now = sys_ms(); bool ahead = false;
        for (int m = 0; m < cfg.nodes; m++)
            if (m != cfg.id && (lnk_members() >> m & 1) && psh->beat[m] && now - psh->beat[m] < 500 && (int32_t)(n - psh->frame[m]) > 1) ahead = true;
        if (!ahead) break;
        if (now - t0 > 300) { party_gave_up++; break; }
        lnk_idle();
        SDL_Delay(1);
        psh->beat[cfg.id] = sys_ms();
    }
    party_waited_ms += sys_ms() - t0;
    if (n % 1800 == 0) {
        if (party_waited_ms || party_gave_up) fprintf(stderr, "[LINK] party: waited %u ms for the other copies in the last 30 s%s\n", party_waited_ms, party_gave_up ? " (one was not waited for: it had stopped)" : "");
        party_waited_ms = party_gave_up = 0;
    }
}
static void place_window(SDL_Window *w)
{
    SDL_Rect b;
    if (cfg.peers || SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(w), &b) != 0) return;
    if (party) {                                                       /* no frames, side by side (2) or a 2 x 2 grid: together they fill the screen */
        if (!party_taskbar) {                                          /* the WHOLE screen, over the task bar (kept on top of it): --party-taskbar leaves it in view */
            SDL_Rect full;
            if (SDL_GetDisplayBounds(SDL_GetWindowDisplayIndex(w), &full) == 0) b = full;
            SDL_SetWindowAlwaysOnTop(w, SDL_TRUE);
        }
        const int pc = cfg.nodes <= 1 ? 1 : 2, pr = (cfg.nodes + pc - 1) / pc;
        const int tw = b.w / pc, th = b.h / pr;
        SDL_SetWindowBordered(w, SDL_FALSE);
        SDL_SetWindowSize(w, tw, th);
        SDL_SetWindowPosition(w, b.x + (cfg.id % pc) * tw, b.y + (cfg.id / pc) * th);
        if (eng_party_pad() == 0) SDL_RaiseWindow(w);                  /* the keyboard's copy: in front, so the keys reach it */
        return;
    }
    eng_tile_window(w, cfg.id, cfg.nodes);                             /* the copies of a test edge to edge in a grid (engine/eng_tile.h) */
}

/* "Hears 1 (192.168.1.163), 3 (192.168.1.164) | Can't hear 4 | Not heard by 3" -- who this one hears, from where, who hears it */
static void hearing(char *buf, size_t n, bool addresses)
{
    const uint32_t h = lnk_hear_mask(), by = lnk_heard_by_mask();
    size_t k = 0; char not_h[40] = "", not_by[40] = ""; int kn = 0, kb = 0;
    k += (size_t)snprintf(buf + k, n - k, "Hears");
    bool any = false;
    for (int c = 0; c < cfg.nodes; c++) {
        if (c == cfg.id) continue;
        char a[16]; int ago; uint32_t pk;
        if (h >> c & 1) {
            if (addresses && lnk_peer_info(c, a, &ago, &pk)) k += (size_t)snprintf(buf + k, n - k, "%s %d (%s)", any ? "," : "", c + 1, a);
            else k += (size_t)snprintf(buf + k, n - k, "%s %d", any ? "," : "", c + 1);
            any = true;
            if (!(by >> c & 1)) kb += snprintf(not_by + kb, sizeof not_by - (size_t)kb, "%s%d", kb ? ", " : "", c + 1);
        } else kn += snprintf(not_h + kn, sizeof not_h - (size_t)kn, "%s%d", kn ? ", " : "", c + 1);
        if (k >= n) return;
    }
    if (!any) k += (size_t)snprintf(buf + k, n - k, " nobody");
    if (*not_h && k < n) k += (size_t)snprintf(buf + k, n - k, " | Can't hear %s", not_h);
    if (*not_by && k < n) snprintf(buf + k, n - k, " | Not heard by %s", not_by);
}

static char auto_mine[16];                                             /* automatic numbering: this PC's address, when it is not in the list */
static bool auto_fail;

/* THE LINK'S QUALITY to each cabinet (the round trip of the hellos, messages lost), in words. The games give a message about 0.13 s
 * to go round the whole ring: a path whose worst round trip reaches 100 ms, or that loses 2% or more, is POOR. */
static const char *quality_word(int rtt_max, int loss)
{
    return rtt_max >= 100 || loss >= 2 ? "POOR" : rtt_max >= 40 || loss >= 1 ? "fair" : "good";
}
static void quality_text(char *b, size_t n)
{
    size_t k = 0; b[0] = 0;
    for (int c = 0; c < cfg.nodes && k < n; c++) {
        int ra, rm, lp;
        if (c == cfg.id || !lnk_peer_quality(c, &ra, &rm, &lp)) continue;
        char loss[16] = ""; if (lp) snprintf(loss, sizeof loss, " %d%% lost", lp);
        k += (size_t)snprintf(b + k, n - k, "%s%d: %d ms%s %s", k ? ", " : "", c + 1, ra, loss, quality_word(rm, lp));
    }
}

/* THE WAITING SCREEN while the cabinets look for each other: who is here, who is not yet, the countdown, the warnings. The lines are
 * made here; screen() (the game's host) draws them over a black picture. */
static void wait_screen(void (*screen)(const char *const *lines, int n), const char *title, uint32_t here, uint32_t deaf, int left,
                        const char *sm)
{
    static char L[20][100]; const char *lp[20]; int n = 0;
    const int dots = (int)(SDL_GetTicks64() / 400 % 4);
    snprintf(L[n++], sizeof L[0], "%s", title);
    snprintf(L[n++], sizeof L[0], "LINK PLAY: LOOKING FOR THE OTHER CABINETS");
    snprintf(L[n++], sizeof L[0], " ");
    bool all = true;
    for (int c = 0; c < cfg.nodes && n < 14; c++) {
        const bool in = here >> c & 1;
        all &= in;
        char q[48] = ""; int ra, rm, lp;
        if (in && c != cfg.id && lnk_peer_quality(c, &ra, &rm, &lp)) {
            char loss[16] = ""; if (lp) snprintf(loss, sizeof loss, ", %d%% lost", lp);
            snprintf(q, sizeof q, "   %d ms%s%s", ra, loss, quality_word(rm, lp)[0] == 'P' ? " - POOR" : "");
        }
        snprintf(L[n++], sizeof L[0], "Cabinet %d:  %s%.*s%s", c + 1, c == cfg.id ? "this PC" : in ? "found" : "waiting", in || c == cfg.id ? 0 : dots, "...", q);
    }
    snprintf(L[n++], sizeof L[0], " ");
    if (all) snprintf(L[n++], sizeof L[0], "All cabinets found: starting");
    else snprintf(L[n++], sizeof L[0], "Starting in %d s, or as soon as all are here", left);
    if (deaf) {
        char d[40] = ""; int kd = 0;
        for (int c = 0; c < cfg.nodes; c++) if (deaf >> c & 1) kd += snprintf(d + kd, sizeof d - (size_t)kd, "%s%d", kd ? ", " : "", c + 1);
        snprintf(L[n++], sizeof L[0], "Cabinet%s %s can't hear this PC", strchr(d, ',') ? "s" : "", d);
    }
    if (*sm) snprintf(L[n++], sizeof L[0], "%s", sm);
    snprintf(L[n++], sizeof L[0], " ");
    snprintf(L[n++], sizeof L[0], "Press S, or Start on a controller, to play without the link");
    for (int i = 0; i < n; i++) lp[i] = L[i];
    screen(lp, n);
}

void link_host_gather(const char *title, SDL_Window *w, void (*hint)(const char *text, int frames),
                      void (*screen)(const char *const *lines, int n))
{
    disc_start_responder(on ? cfg.base_port : file_port(), title);           /* answer "which PCs run this game?" (Find cabinets) */
    if (auto_fail) {                                                   /* This cabinet = Auto, and this PC is not in the list: not linked */
        char t[256];
        snprintf(t, sizeof t, "%s - NOT LINKED: this PC (%s) is not in the address list", title, *auto_mine ? auto_mine : "no address");
        fprintf(stderr, "[LINK] NOT linked: \"This cabinet\" is Auto, but this PC's address (%s) is not in the Network page's address list\n",
                *auto_mine ? auto_mine : "none found");
        if (w) SDL_SetWindowTitle(w, t);
        if (hint) hint("NOT LINKED: THIS PC IS NOT IN THE ADDRESS LIST", 900);
        return;
    }
    if (!on) return;
    if (party) party_open();
    if (w) place_window(w);
    uint32_t shown = 0xFFFFFFFFu, shown_deaf = 0; int shown_s = -1;
    bool skip = false; uint64_t skip_at = 0;
    { const char *e = getenv("LINK_TEST_SKIP"); if (e && *e) { skip_at = SDL_GetTicks64() + (uint64_t)atoi(e) * 1000 + 1;   /* TEST: as if S were pressed after E s */
#ifdef _WIN32
          _putenv("LINK_TEST_SKIP=");
#else
          unsetenv("LINK_TEST_SKIP");
#endif
      } }
    while (!skip && lnk_gather() == LNK_GATHERING) {
        disc_poll();
        if (lnk_restart_request()) { fprintf(stderr, "[LINK] asked to restart while looking for the cabinets: restarting\n"); restart_all_now(); }
        SDL_Event e;
        while (w && SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { lnk_close(); exit(0); }
            if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_s) skip = true;   /* S: play without the link */
            if (e.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(e.cdevice.which);        /* (a pad's buttons arrive once it is open) */
            if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_START) skip = true;   /* ... or a pad's Start */
        }
        if (skip_at && SDL_GetTicks64() >= skip_at) skip = true;
        if (skip) break;
        uint32_t here = lnk_present_mask(); int left = (lnk_gather_left_ms() + 999) / 1000;
        const uint32_t deaf = lnk_hear_mask() & ~lnk_heard_by_mask();   /* cabinets this one hears that cannot hear it */
        char sm[160]; setup_msg(sm, sizeof sm);
        if (strcmp(sm, setup_msg_shown)) { if (*sm) fprintf(stderr, "[LINK] %s\n", sm);
                                           snprintf(setup_msg_shown, sizeof setup_msg_shown, "%s", sm); shown_s = -1; }
        static uint64_t next_draw;
        if (screen && SDL_GetTicks64() >= next_draw) { next_draw = SDL_GetTicks64() + 100; wait_screen(screen, title, here, deaf, left, sm); }
        if (here != shown || left != shown_s || deaf != shown_deaf) {
            char miss[40] = "", t[256], d[64] = ""; int k = 0, kd = 0;
            for (int n = 0; n < cfg.nodes; n++) if (!(here >> n & 1)) k += snprintf(miss + k, sizeof miss - (size_t)k, "%s%d", k ? ", " : "", n + 1);
            for (int n = 0; n < cfg.nodes; n++) if (deaf >> n & 1) kd += snprintf(d + kd, sizeof d - (size_t)kd, "%s%d", kd ? ", " : "", n + 1);
            if (*miss) snprintf(t, sizeof t, "%s - Waiting for Cabinet%s %s (%d s)", title, strchr(miss, ',') ? "s" : "", miss, left);
            else snprintf(t, sizeof t, "%s - All cabinets found: starting", title);
            if (*d) { size_t tl = strlen(t); snprintf(t + tl, sizeof t - tl, " - Cabinet%s %s can't hear this PC", strchr(d, ',') ? "s" : "", d); }
            if (*sm) { size_t tl = strlen(t); snprintf(t + tl, sizeof t - tl, " - %s", sm); }
            if (w) SDL_SetWindowTitle(w, t);
            if (here != shown || deaf != shown_deaf) {
                char hb[200]; hearing(hb, sizeof hb, true);
                fprintf(stderr, "[LINK] %s  [%s]\n", t + strlen(title) + 3, hb);
            }
            shown = here; shown_s = left; shown_deaf = deaf;
        }
        SDL_Delay(5);
    }
    char t[256];
    if (skip) {                        /* this run only: the others hear it go (as if switched off) and carry on without it */
        fprintf(stderr, "[LINK] S pressed while looking for the cabinets: playing without the link (the next start links as usual)\n");
        lnk_close(); on = false; setup_msg_shown[0] = 0;
        snprintf(t, sizeof t, "%s - Link skipped (S): playing without the link; start the game again to link", title);
        if (hint) hint("LINK SKIPPED: PLAYING WITHOUT THE LINK", 600);
    } else if (lnk_state() == LNK_ALONE) {
        snprintf(t, sizeof t, "%s - NOT LINKED (started late): restart all cabinets to link", title);
        if (hint) hint("NOT LINKED: STARTED AFTER THE OTHERS", 600);          /* (full screen: the title bar is not seen) */
    } else if (lnk_members() == 1u << cfg.id) {
        snprintf(t, sizeof t, "%s - No other cabinets found: playing solo", title);
        if (hint) hint("NO OTHER CABINETS FOUND: PLAYING SOLO", 600);
    } else {
        char who[40] = ""; int k = 0;
        for (int n = 0; n < cfg.nodes; n++) if (lnk_members() >> n & 1) k += snprintf(who + k, sizeof who - (size_t)k, "%s%d", k ? "+" : "", n + 1);
        snprintf(t, sizeof t, "%s - Linked: %s", title, who);
        if (party && hint) {                                               /* a split screen has no title bars: say whose picture this is */
            char ph[64];
            if (eng_party_pad() > 0) snprintf(ph, sizeof ph, "PLAYER %d: CONTROLLER %d", cfg.id + 1, eng_party_pad());
            else if (eng_party_pad() == 0) snprintf(ph, sizeof ph, "PLAYER %d: KEYBOARD (CLICK HERE FIRST)", cfg.id + 1);
            else snprintf(ph, sizeof ph, "PLAYER %d", cfg.id + 1);
            hint(ph, 600);
        }
    }
    win = w; snprintf(base_title, sizeof base_title, "%s", t);
    set_title();
}

/* two members whose games use the same ID: the game cannot tell them apart (the operator sets each one's ID in its test menu) */
static void check_ids(void)
{
    if (!G->game_id || lnk_state() != LNK_LINKED) return;
    lnk_set_game_id(G->game_id());
    uint32_t clash = 0; const uint32_t on = lnk_alive_mask();
    for (int n = 0; n < LNK_MAX_NODES; n++) if ((on >> n & 1) && lnk_peer_game_id(n) < 0) return;   /* wait until every member's ID is known */
    for (int a = 0; a < LNK_MAX_NODES; a++) for (int b = a + 1; b < LNK_MAX_NODES; b++)
        if ((on >> a & 1) && (on >> b & 1) && lnk_peer_game_id(a) >= 0 && lnk_peer_game_id(a) == lnk_peer_game_id(b)) clash |= 1u << a | 1u << b;
    if (clash == dup_shown) return;
    dup_shown = clash;
    if (clash) {
        char who[40] = ""; int k = 0;
        for (int n = 0; n < LNK_MAX_NODES; n++) if (clash >> n & 1) k += snprintf(who + k, sizeof who - (size_t)k, "%s%d", k ? ", " : "", n + 1);
        snprintf(dup_msg, sizeof dup_msg, "Cabinets %s have the same game ID (change it in the test menu)", who);
        fprintf(stderr, "[LINK] WARNING: cabinets %s have the same game ID (test menu): the game cannot tell them apart\n", who);
    } else {
        dup_msg[0] = 0;
        fprintf(stderr, "[LINK] every cabinet's game ID is different now\n");
    }
    set_title();
}

/* SET-UPS THAT DIFFER: every cabinet of a set needs the same "Cabinets in the set" and the same address list (each its own number).
 * A cabinet with another size waits for cabinets that are not there, or sends to the wrong ones; so it is said plainly. */
static void setup_msg(char *m, size_t n)
{
    m[0] = 0;
    for (int c = 0; c < cfg.nodes; c++) {                              /* another build: every cabinet needs the same copy of the game */
        uint32_t crc, when;
        if (!lnk_peer_build(c, &crc, &when) || crc == cfg.build_crc) continue;
        char a[24] = "?", b[24] = "?"; time_t ta = (time_t)when, tb = (time_t)cfg.build_time; struct tm *tm;
        if ((tm = localtime(&ta))) strftime(a, sizeof a, "%d %b %H:%M", tm);
        if ((tm = localtime(&tb))) strftime(b, sizeof b, "%d %b %H:%M", tm);
        if (strcmp(a, b)) snprintf(m, n, "DIFFERENT VERSION: Cabinet %d's game is dated %s, this PC's %s", c + 1, a, b);
        else snprintf(m, n, "DIFFERENT VERSION: Cabinet %d runs a different build of the game", c + 1);
        return;
    }
    for (int c = 0; c < cfg.nodes; c++) {
        bool same = true; const int nodes = lnk_peer_setup(c, &same);
        if (!nodes) continue;
        if (nodes != cfg.nodes) { snprintf(m, n, "SET-UP DIFFERS: Cabinet %d is set for %d cabinets, this PC for %d", c + 1, nodes, cfg.nodes); return; }
        if (!same) { snprintf(m, n, "SET-UP DIFFERS: Cabinet %d has a different address list", c + 1); return; }
    }
}
static void check_setup(void)
{
    char m[160]; setup_msg(m, sizeof m);
    if (!strcmp(m, setup_msg_shown)) return;
    if (*m) fprintf(stderr, "[LINK] %s\n", m);
    else if (*setup_msg_shown) fprintf(stderr, "[LINK] every cabinet's set-up matches now\n");
    snprintf(setup_msg_shown, sizeof setup_msg_shown, "%s", m);
    set_title();
}
static void set_title(void)
{
    if (!win) return;
    char t[640]; size_t k = (size_t)snprintf(t, sizeof t, "%s", base_title);
    if (*setup_msg_shown && k < sizeof t) k += (size_t)snprintf(t + k, sizeof t - k, " - %s", setup_msg_shown);
    if (*dup_msg && k < sizeof t) snprintf(t + k, sizeof t - k, " - WARNING: %s", dup_msg);
    SDL_SetWindowTitle(win, t);
}

void link_host_slice(void)
{
    if (!on) return;
    for (int k = 0; k < 8 && lan139_tick(k == 0); k++) {                /* a frame is taken in and forwarded within the slice */
        G->raise_irq();
        G->deliver_irqs();
    }
}

static void pace(void)                                                 /* headless linked copies: real time, like a window */
{
    static uint64_t next; const uint64_t f = SDL_GetPerformanceFrequency(), step = (uint64_t)((double)f / (59.906 * (1.0 + skew_ppm / 1e6)));
    uint64_t t = SDL_GetPerformanceCounter();
    if (!next || t > next + 10 * step) next = t;
    next += step;
    while ((t = SDL_GetPerformanceCounter()) < next) { uint64_t left_ms = (next - t) * 1000 / f; if (left_ms > 2) SDL_Delay((Uint32)(left_ms - 1)); }
}

static void report(uint32_t frame)
{
    const lnk_stats *n = lnk_get_stats(); const lan139_stats *c = lan139_get_stats();
    char g[200] = "";
    if (G->report) G->report(g, sizeof g);
    char ids[64] = ""; int k = 0;
    for (int n = 0; n < LNK_MAX_NODES; n++) { int id = lnk_peer_game_id(n); if (id >= 0) k += snprintf(ids + k, sizeof ids - (size_t)k, " %d:%d", n + 1, id + 1); }
    fprintf(stderr, "[LINK] frame %u  down %u (now %u)  cabinets on %02X (next %d)  game IDs%s  chip tx %u rx %u queue-drop %u unacked %u queue %.1f (max %u)  net sent %u recv %u lost %u bad %u  | %s\n",
            frame, down_frames, down_now, lnk_alive_mask(), lnk_next_hop() + 1, ids, c->tx, c->rx, c->rx_dropped, c->rx_unacked, c->q_n ? (double)c->q_sum / c->q_n : 0.0, c->q_max, n->sent, n->recv, n->lost_seq, n->bad, g);
    { lan139_stats *w = (lan139_stats *)c; w->q_sum = w->q_n = w->q_max = 0; }
}

/* THE SAFETY NET: a linked game that has lost its link for good restarts, as if its cabinet were switched off and on, so it is not left
 * with a battle nobody else is in (enemies that stand still: the cabinet that ran them has gone). When: after the link had worked,
 * every other cabinet gone for 5 s, or the game itself without its link for 10 s. First "LINK LOST - RESTARTING IN 5" on the screen;
 * if the link comes back meanwhile, nothing happens. After the restart it looks for the others as at power-on: any still playing
 * leave it out (it plays on its own); if they were all restarted too, it links with them. Network page: "If the link is lost". */
static bool lost_carry_on;                                             /* the page's choice: carry on instead (set from link.cfg) */
static void safety_net(uint32_t frame, bool headless)
{
    static uint32_t alone_for, gdown_for; static int count = -1; static bool told;
    if (lnk_state() != LNK_LINKED || !was_up || lnk_members() == 1u << cfg.id) { alone_for = gdown_for = 0; return; }   /* (a set of one: nothing to lose) */
    alone_for = (lnk_alive_mask() & ~(1u << cfg.id)) ? 0 : alone_for + 1;
    gdown_for = (G->good && !G->good()) ? gdown_for + 1 : 0;
    const char *why = alone_for >= 300 ? "every other cabinet has gone" : gdown_for >= 600 ? "the game has had no link for 10 seconds" : NULL;
    if (!why) {
        if (count >= 0) { fprintf(stderr, "[LINK] frame %u: SAFETY NET: the link is back: not restarting\n", frame); if (G->hint) G->hint("", 0); }
        count = -1; told = false;
        return;
    }
    if (lost_carry_on) {
        if (!told) fprintf(stderr, "[LINK] frame %u: SAFETY NET: %s; carrying on (Network page: If the link is lost = Carry on)\n", frame, why);
        told = true; count = -1;
        return;
    }
    if (count < 0) { count = 300; fprintf(stderr, "[LINK] frame %u: SAFETY NET: %s: restarting this cabinet in 5 s\n", frame, why); }
    if (count % 60 == 0 && count > 0 && G->hint) { char t[64]; snprintf(t, sizeof t, "LINK LOST - RESTARTING IN %d", count / 60); G->hint(t, 75); }
    if (count-- > 0) return;
    if (headless) {                                                    /* the tests: no restart, the log says it would */
        fprintf(stderr, "[LINK] frame %u: SAFETY NET: (a test run: it would restart now)\n", frame);
        alone_for = gdown_for = 0; count = -1; was_up = false;
        return;
    }
    fprintf(stderr, "[LINK] frame %u: SAFETY NET: restarting now\n", frame);
    if (G->before_restart) G->before_restart();
    link_host_relaunch();
}

/* start this program again with the same command line (the log continues), then end this copy */
void link_host_relaunch(void)
{
    lnk_close();                                                       /* its port free for the new copy; the others hear it go */
    fflush(NULL);
#ifdef _WIN32
    SetEnvironmentVariableA("N22_LOG_APPEND", "1");
    static wchar_t exe[32768], cl[32768];
    const DWORD n = GetModuleFileNameW(NULL, exe, 32768);
    wcsncpy(cl, GetCommandLineW(), 32767);
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (n > 0 && n < 32768 && CreateProcessW(exe, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    else fprintf(stderr, "[LINK] the restart failed (error %lu)\n", (unsigned long)GetLastError());
#else
    setenv("N22_LOG_APPEND", "1", 1);
    if (argv0) execv("/proc/self/exe", argv0);
    perror("[LINK] the restart failed");
#endif
    exit(0);
}

/* RESTART ALL CABINETS (Network page): this one tells every cabinet of the set, then all restart -- the whole set switched off and on,
 * so they look for each other again and link as at power-on. A second on the screen first, so everyone sees why. */
static int restart_in = -1;                                            /* frames until this cabinet restarts (-1 = not asked) */
static void restart_all_check(void)
{
    const int from = lnk_restart_request();
    if (from && restart_in < 0) {
        fprintf(stderr, "[LINK] Cabinet %d asked every cabinet to restart: restarting\n", from);
        if (G && G->hint) G->hint("RESTARTING ALL CABINETS", 120);
        restart_in = 60;
    }
}
static void restart_all_now(void)
{
    fprintf(stderr, "[LINK] restarting (restart all cabinets)\n");
    if (G && G->before_restart) G->before_restart();
    link_host_relaunch();
}

/* CATCHING UP (our fork). A window paces itself by its screen: a frame that took too long (a texture load, a busy PC) is lost for good,
 * and a screen that refreshes a little slower than another's loses a frame every few seconds. In a race the games keep their cabinets
 * together themselves, but their attract modes run alone, so the set drifted apart (one cabinet's demo a few frames behind the
 * others'). Each cabinet tells the others how many frames it has run (engine/link_net.c, lnk_frames_behind); one that is 2 or more
 * behind the furthest ahead runs frames without drawing them (link_host_catch_up: no picture, no wait for the screen), at most two in
 * a row, until it is less than one behind. Only the one behind does anything; the others never wait. LINK_CATCHUP=0: off. */
static bool cu_on = true, cu_state, cu_now, cu_host; static int cu_run; static uint32_t cu_total;   /* cu_host: the window code asks (link_host_catch_up) */
static void catch_up_frame(uint32_t frame, bool headless)
{
    lnk_set_frame(frame);
    cu_now = false;
    if (headless || !cu_on || !cu_host || frame < 300) return;                     /* (headless copies keep to the wall clock already; start-up loads) */
    const double b = lnk_frames_behind();
    if (!cu_state && b >= 2.0 && b < 600.0) cu_state = true;
    else if (cu_state && (b < 1.0 || b >= 600.0)) cu_state = false;
    if (cu_state && cu_run < 2) { cu_now = true; cu_run++; cu_total++; }
    else cu_run = 0;
    if (frame % 600 == 0 && cu_total) { fprintf(stderr, "[LINK] frame %u: caught up %u frames on the others (ran them without drawing)\n", frame, cu_total); cu_total = 0; }
}
bool link_host_catch_up(void) { cu_host = true; return on && cu_now; }

void link_host_frame(uint32_t frame, bool headless)
{
    { static int once; if (!once) { once = 1; const char *e = getenv("LINK_CATCHUP"); if (e && *e == '0') cu_on = false; } }
    disc_poll();
    { static long find_at = -2, restart_at = -2;                        /* TEST: LINK_TEST_FIND_AT / LINK_TEST_RESTART_AT = <frame>: the page's
                                                                         * Find cabinets / Restart all (twice) at that frame, without a menu */
      if (find_at == -2) { const char *e = getenv("LINK_TEST_FIND_AT"); find_at = e ? atol(e) : -1; }
      if (restart_at == -2) { const char *e = getenv("LINK_TEST_RESTART_AT"); restart_at = e ? atol(e) : -1; }
      if (find_at >= 0 && frame == (uint32_t)find_at) disc_find();
      if (restart_at >= 0 && frame == (uint32_t)restart_at && on) {
#ifdef _WIN32
          SetEnvironmentVariableA("LINK_TEST_RESTART_AT", NULL);        /* (the restarted copies must not do it again) */
#else
          unsetenv("LINK_TEST_RESTART_AT");
#endif
          fprintf(stderr, "[LINK] this cabinet asked every cabinet to restart (test)\n");
          lnk_send_restart(); restart_in = 60;
      } }
    if (!on) return;
    lan139_frame();
    restart_all_check();
    if (restart_in >= 0 && restart_in-- == 0) restart_all_now();
    if (headless) pace();
    if (party) party_sync();
    catch_up_frame(frame, headless);
    if (frame == 180 && win && !cfg.peers) { int x, y, ww, wh, t = 0, l = 0, bo = 0, r = 0; SDL_GetWindowPosition(win, &x, &y); SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetWindowBordersSize(win, &t, &l, &bo, &r); fprintf(stderr, "[HOST] window now: picture %d,%d %d x %d, frame %d,%d - %d,%d\n", x, y, ww, wh, x - l, y - t, x + ww + r, y + wh + bo); }
    { static long st = -2; if (st == -2) { const char *e = getenv("LINK_STAMP"); st = e ? atol(e) : -1; }   /* LINK_STAMP=<frame> (test): when this cabinet ran it */
      if (st >= 0 && frame == (uint32_t)st) fprintf(stderr, "[STAMP] frame %u at %u ms\n", frame, (unsigned)sys_ms()); }
    if (G->good) {
        if (G->good()) { if (++up_run >= 120) was_up = true; down_now = 0; }
        else if ((up_run = 0), was_up) { down_frames++; if (++down_now == 1) fprintf(stderr, "[LINK] frame %u: the game lost the link\n", frame); }
    }
    if (frame % 30 == 0 && frame >= 300) check_ids();              /* the games take a moment to read their own ID from their settings: 0 until then */
    if (frame % 30 == 0) check_setup();
    safety_net(frame, headless);
    { static char poor_shown[LNK_MAX_NODES];
      for (int c = 0; c < cfg.nodes; c++) {
          int ra, rm, lp; const bool poor = c != cfg.id && lnk_peer_quality(c, &ra, &rm, &lp) && quality_word(rm, lp)[0] == 'P';
          if (poor && !poor_shown[c]) fprintf(stderr, "[LINK] frame %u: the path to Cabinet %d is POOR: round trip %d ms (worst %d), %d%% lost\n", frame, c + 1, ra, rm, lp);
          poor_shown[c] = poor;
      } }
    if (frame % 1800 == 0) { char qb[200]; quality_text(qb, sizeof qb); if (*qb) fprintf(stderr, "[LINK] frame %u  quality: %s\n", frame, qb); }
    if (frame % 300 == 0) {
        report(frame);
        char hb[240]; hearing(hb, sizeof hb, true);
        static char last[240];
        if (strcmp(hb, last) || frame % 3600 == 0) { fprintf(stderr, "[LINK] frame %u  %s\n", frame, hb); snprintf(last, sizeof last, "%s", hb); }
    }
}

/* ---- THE SAVED SETTINGS (link.cfg beside the program) and the menu's Network page -----------------------------------------------
 * The file is what the Network page edits:  link=on|off  cabinet=N  cabinets=M  address1=...  address8=...  wait=S  port=P
 * Used when the command line has no --link (a cabinet started by double-clicking the game); --link and friends override it. A change
 * takes effect the next time the game starts: the boards only look for each other when they are switched on. */
#define CFG_FILE "link.cfg"
/* cabinet_auto: "This cabinet" = Auto, the number is where this PC's own address is in the list (last_auto: the one found last time;
 * when it changes the game's own ID follows). lights: 0 off, else where (LINK_LIGHTS_*). */
/* find_auto: "Find others" = Automatic (the standard): no address list, every PC only needs its own number -- the link finds the others
 * on the network (engine/link_net.c, "*" addresses). Off = the typed / found address list below. */
static struct { bool on, set_id, carry_on, cab_auto, classic, author, arcade, record, find_auto; int cabinet, cabinets, wait, port, lights, last_auto; char addr[LNK_MAX_NODES][16]; }
    F = { false, false, false, false, false, false, true, true, true, 1, 2, 60, 22220, LINK_LIGHTS_BOTTOM_LEFT, 0, { "" } };
static const char *const light_key[7] = { "off", "bottom-left", "bottom-centre", "bottom-right", "top-left", "top-centre", "top-right" };
static const char *const light_name[7] = { "Off", "Bottom left", "Bottom centre", "Bottom right", "Top left", "Top centre", "Top right" };
static char peers_buf[LNK_MAX_NODES * 17];
static int file_port(void) { return F.port; }
static int  edit_row = -1;                                             /* the address row being typed, -1 = none */
static char edit_buf[16];

static void cfg_load(void)
{
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return;
    char line[128]; bool saw_find = false, any_addr = false;
    while (fgets(line, sizeof line, f)) {
        char k[32], v[64]; int n;
        if (sscanf(line, " %31[^= ] = %63s", k, v) != 2) continue;
        if (!strcmp(k, "link")) F.on = !strcmp(v, "on");
        else if (!strcmp(k, "cabinet")) { F.cab_auto = !strcmp(v, "auto"); if (!F.cab_auto) F.cabinet = atoi(v); }
        else if (!strcmp(k, "last_auto")) F.last_auto = atoi(v);
        else if (!strcmp(k, "cabinets")) F.cabinets = atoi(v);
        else if (!strcmp(k, "wait")) F.wait = atoi(v);
        else if (!strcmp(k, "port")) F.port = atoi(v);
        else if (!strcmp(k, "set_game_id")) F.set_id = atoi(v) != 0;
        else if (!strcmp(k, "lights")) { F.lights = !strcmp(v, "on") ? 1 : 0; for (int i = 1; i < 7; i++) if (!strcmp(v, light_key[i])) F.lights = i; }
        else if (!strcmp(k, "if_lost")) F.carry_on = !strcmp(v, "carry_on");
        else if (!strcmp(k, "timing")) F.classic = !strcmp(v, "classic");
        else if (!strcmp(k, "arcade_mode")) F.arcade = strcmp(v, "off") != 0;
        else if (!strcmp(k, "record")) F.record = strcmp(v, "off") != 0;
        else if (!strcmp(k, "method")) F.author = !strcmp(v, "modern") || !strcmp(v, "author");   /* ("author" / "ours": the first names) */
        else if (!strcmp(k, "find")) { saw_find = true; F.find_auto = strcmp(v, "list") != 0; }
        else if (sscanf(k, "address%d", &n) == 1 && n >= 1 && n <= LNK_MAX_NODES) { snprintf(F.addr[n - 1], sizeof F.addr[0], "%.15s", v); any_addr = true; }
    }
    fclose(f);
    if (!saw_find) F.find_auto = !any_addr;                            /* (a file from before "Find others": its list is kept) */
    if (F.find_auto && F.cab_auto) { F.cab_auto = false; if (F.last_auto) F.cabinet = F.last_auto; }   /* Auto numbering needs a list */
    if (F.cabinets < 2) F.cabinets = 2;
    if (F.cabinets > LNK_MAX_NODES) F.cabinets = LNK_MAX_NODES;
    if (F.cabinet < 1) F.cabinet = 1;
    if (F.cabinet > F.cabinets) F.cabinet = F.cabinets;
    if (F.wait < 10) F.wait = 10;
}

static bool method_at_start;                                           /* the method this run started with */
static bool modern(void) { return F.author && !F.arcade; }             /* Modern multiplayer: only with arcade mode off */
bool link_host_author_method(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], "--link", 6)) return false;   /* a one-PC test launcher: ours */
    cfg_load();
    return method_at_start = modern();
}
bool link_host_arcade_mode(void) { return F.arcade; }
bool link_host_record_sessions(void) { cfg_load(); return F.record; }   /* record= in link.cfg (on unless it says off) */

static void cfg_save(void)
{
    FILE *f = fopen(CFG_FILE ".tmp", "w");
    if (!f) return;
    fprintf(f, "# Link play settings (the game's Esc menu > Network edits this file). Used when the game is started without --link.\n");
    if (F.cab_auto) fprintf(f, "link=%s\ncabinet=auto\n", F.on ? "on" : "off");
    else fprintf(f, "link=%s\ncabinet=%d\n", F.on ? "on" : "off", F.cabinet);
    fprintf(f, "cabinets=%d\nwait=%d\nport=%d\n", F.cabinets, F.wait, F.port);
    fprintf(f, "# find the others: auto = on the network by themselves (every PC its own cabinet number); list = the addresses below\nfind=%s\n",
            F.find_auto ? "auto" : "list");
    if (F.last_auto) fprintf(f, "last_auto=%d\n", F.last_auto);
    for (int n = 0; n < LNK_MAX_NODES; n++) if (F.addr[n][0]) fprintf(f, "address%d=%s\n", n + 1, F.addr[n]);
    if (F.set_id) fprintf(f, "# the game's own ID (test menu) is set to this cabinet's number when the game next starts\nset_game_id=1\n");
    fprintf(f, "lights=%s\nif_lost=%s\n", light_key[F.lights], F.carry_on ? "carry_on" : "restart");
    fprintf(f, "# frame timing: new = the author's current one (Display > Frame rate, 120/144 Hz screens), classic = 0.4.0's (one picture per frame)\ntiming=%s\n",
            F.classic ? "classic" : "new");
    fprintf(f, "# ARCADE MODE (on unless this says off): this program is for real cabinets linked on a wired LAN, so the menu shows only that --\n"
               "# no Online page, no \"Link method\" row, and the method below is not used. arcade_mode=off brings Modern multiplayer back.\n"
               "arcade_mode=%s\n", F.arcade ? "on" : "off");
    fprintf(f, "# link method: arcade = Arcade LAN, for real cabinets (this page: the cabinets link when they are switched on, coins as normal);\n"
               "# modern = Modern multiplayer, the original author's online play (the Online page: host / find a game, a lobby) -- Arcade LAN is then off\n"
               "method=%s\n", F.author ? "modern" : "arcade");
    fprintf(f, "# session recordings (<game>_last.rec beside the program: a played session's inputs, so a problem can be replayed): on | off\n"
               "record=%s\n", F.record ? "on" : "off");
    fclose(f);
#ifdef _WIN32
    remove(CFG_FILE);
#endif
    rename(CFG_FILE ".tmp", CFG_FILE);
}

/* automatic numbering: this PC's cabinet = the position of one of its own addresses in the list (0 = none). At start-up it waits up to
 * 15 s for one to appear: a PC switched on with the cabinet may start the game before its network is up. */
static int own_cabinet(bool wait)
{
    static uint32_t t_last; static int last;                           /* (the menu asks every frame: once a second is enough) */
    if (!wait && t_last && SDL_GetTicks() - t_last < 1000) return last;
    if (!wait) t_last = SDL_GetTicks();
    last = 0;
    for (int tries = 0; tries < (wait ? 30 : 1); tries++) {
        char a[8][16]; const int n = lnk_local_addresses(a, 8);
        for (int c = 0; c < F.cabinets; c++) for (int i = 0; i < n; i++) if (F.addr[c][0] && !strcmp(F.addr[c], a[i])) return last = c + 1;
        snprintf(auto_mine, sizeof auto_mine, "%s", n ? a[0] : "");
        if (wait) SDL_Delay(500);
    }
    return 0;
}

int link_host_prescan(int argc, char **argv)
{
    argv0 = argv;
    cfg_load();
    method_at_start = modern();
    lost_carry_on = F.carry_on;
    for (int i = 1; i < argc; i++) {                                   /* SEVERAL PLAYERS ON ONE PC (engine/eng_party.h): --party, --pad K */
        if (!strcmp(argv[i], "--party")) party = true;
        if (!strcmp(argv[i], "--party-taskbar")) party = party_taskbar = true;
        if (!strcmp(argv[i], "--pad") && i + 1 < argc) {
            char e[32]; snprintf(e, sizeof e, "N22_PAD=%d", atoi(argv[i + 1]));
#ifdef _WIN32
            _putenv(e); _putenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1");
#else
            setenv("N22_PAD", e + 8, 1); setenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
#endif
        }
    }
    { const char *t = F.classic ? "classic" : "new";                  /* FRAME TIMING (engine/eng_vsync.h reads N22_TIMING): link.cfg, or --timing */
      for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--timing")) t = argv[i + 1];
      if (!getenv("N22_TIMING")) {
#ifdef _WIN32
          char e[40]; snprintf(e, sizeof e, "N22_TIMING=%s", t); _putenv(e);
#else
          setenv("N22_TIMING", t, 1);
#endif
      } }
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], "--link", 6)) {           /* the command line decides */
        from_cmdline = true;
        for (int j = 1; j + 1 < argc; j++) if (!strcmp(argv[j], "--link")) return atoi(argv[j + 1]);
        return 0;
    }
    if (!F.on || modern()) return 0;
    int cab = F.cabinet;
    if (F.find_auto) {                                                 /* AUTOMATIC: every other cabinet found on the network */
        cfg.id = cab - 1; cfg.nodes = F.cabinets; cfg.gather_ms = F.wait * 1000; cfg.base_port = F.port;
        int k = 0;
        for (int n = 0; n < F.cabinets; n++) k += snprintf(peers_buf + k, sizeof peers_buf - (size_t)k, "%s%s", n ? "," : "", "*");
        cfg.peers = peers_buf;
        return cab;
    }
    if (F.cab_auto) {                                                  /* where this PC's own address is in the list */
        cab = own_cabinet(true);
        if (cab <= 0) { auto_fail = true; return 0; }
        if (F.last_auto && cab != F.last_auto) F.set_id = true;         /* a new number: the game's own ID follows it */
        if (cab != F.last_auto) { F.last_auto = cab; cfg_save(); }
    }
    cfg.id = cab - 1; cfg.nodes = F.cabinets; cfg.gather_ms = F.wait * 1000; cfg.base_port = F.port;
    int k = 0;
    for (int n = 0; n < F.cabinets; n++) k += snprintf(peers_buf + k, sizeof peers_buf - (size_t)k, "%s%s", n ? "," : "", F.addr[n]);
    cfg.peers = peers_buf;
    return cab;
}

/* the Network page changed "This cabinet": at the next start the game's own ID follows it (once; the test menu can change it after) */
bool link_host_take_id_reset(void)
{
    if (from_cmdline || !on || !F.set_id) return false;
    F.set_id = false;
    cfg_save();
    return true;
}

/* FIND CABINETS (Network page): "which PCs on this network run this game?" -- a broadcast to the discovery port (UDP 22219), which every running copy answers, linked or not, with its PC's name (a small thread keeps a socket there). The
 * answers can then fill the address list, sorted by address, so every PC that does the same gets the same list (This cabinet = Auto). */
#ifdef _WIN32
typedef SOCKET dsock_t;
#define DSOCK_BAD INVALID_SOCKET
#define dsock_close closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
typedef int dsock_t;
#define DSOCK_BAD (-1)
#define dsock_close close
#endif
#define DISC_MAGIC 0x4E323244u                                          /* "N22D" */
static int disc_port;
static struct { bool searching; uint64_t until; dsock_t s; int n; struct { char ip[16], name[33]; bool me; int cab; } f[LNK_MAX_NODES]; bool done; } D = { .s = DSOCK_BAD };
static uint64_t restart_armed_at;
static bool restart_armed(void) { return restart_armed_at && SDL_GetTicks64() - restart_armed_at < 5000; }
static uint32_t disc_game;                                             /* the game (its name's fingerprint): only copies of the same game answer */
static uint32_t disc_tag(void) { return disc_game; }

static int disc_responder(void *u)                                     /* the thread: answer every question, for as long as the game runs */
{
    (void)u;
    dsock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == DSOCK_BAD) return 0;
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);   /* several copies on one PC */
    lv_prepare(s);                                                      /* (the answer goes back out of the connection the question came in on) */
    struct sockaddr_in me; memset(&me, 0, sizeof me);
    me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(INADDR_ANY); me.sin_port = htons((uint16_t)disc_port);
    if (bind(s, (struct sockaddr *)&me, sizeof me) != 0) { fprintf(stderr, "[LINK] Find cabinets: cannot answer on UDP port %d\n", disc_port); dsock_close(s); return 0; }
    char host[64] = "PC"; gethostname(host, sizeof host);
    for (;;) {
        uint8_t b[64]; struct sockaddr_in from; uint32_t ifx = 0;
        const int len = lv_recv(s, b, (int)sizeof b, &from, &ifx);
        if (len < 0) { SDL_Delay(5); continue; }
        if (len < 12 || get_u32(b) != DISC_MAGIC || b[4] != 'Q' || get_u32(b + 8) != disc_tag()) continue;
        uint8_t r[48]; memcpy(r, b, 12); r[4] = 'R';                    /* the question's magic, tag and number back, and this PC's name */
        memset(r + 12, 0, 36); snprintf((char *)r + 12, 33, "%s", host);
        r[46] = (uint8_t)(on ? cfg.id + 1 : 0); r[47] = 1;                /* its cabinet number (0 = link off), and "the number is told" */
        lv_send(s, r, (int)sizeof r, &from, ifx);
    }
    return 0;
}
static void disc_start_responder(int base_port, const char *title)
{
    static bool started;
    if (started) return;
    started = true;
    disc_port = 22219; (void)base_port;                                  /* one fixed port: every copy answers there, whatever its link port */
    disc_game = 2166136261u;                                            /* the game's name: the title up to " - " ("Tokyo Wars") */
    for (const char *c = title; *c && strncmp(c, " - ", 3); c++) disc_game = (disc_game ^ (uint8_t)*c) * 16777619u;
#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    SDL_Thread *t = SDL_CreateThread(disc_responder, "link-find", NULL);
    if (t) SDL_DetachThread(t);
}
static void disc_find(void)
{
    if (D.searching || !disc_port) return;
    if (D.s == DSOCK_BAD) {
        D.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (D.s == DSOCK_BAD) return;
        int one = 1; setsockopt(D.s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
#ifdef _WIN32
        u_long nb = 1; ioctlsocket(D.s, FIONBIO, &nb);
#else
        fcntl(D.s, F_SETFL, fcntl(D.s, F_GETFL, 0) | O_NONBLOCK);
#endif
    }
    D.n = 0; D.done = false; D.searching = true; D.until = SDL_GetTicks64() + 1500;
    uint8_t q[12]; put_u32(q, DISC_MAGIC); q[4] = 'Q'; q[5] = q[6] = q[7] = 0; put_u32(q + 8, disc_tag());
    for (int k = 0; k < 2; k++) lv_send_all(D.s, q, (int)sizeof q, htons((uint16_t)disc_port));   /* every connection, and this PC */
    fprintf(stderr, "[LINK] Find cabinets: asking the network (UDP port %d)\n", disc_port);
}
static int ip_cmp(const void *x, const void *y)
{
    struct in_addr a, b; inet_pton(AF_INET, ((const char *)x), &a); inet_pton(AF_INET, ((const char *)y), &b);
    const uint32_t ha = ntohl(a.s_addr), hb = ntohl(b.s_addr); return ha < hb ? -1 : ha > hb;
}
static void disc_poll(void)
{
    if (!D.searching) return;
    for (;;) {
        uint8_t r[64]; struct sockaddr_in from; socklen_t fl = sizeof from;
        const int len = (int)recvfrom(D.s, (char *)r, sizeof r, 0, (struct sockaddr *)&from, &fl);
        if (len < 0) break;
        if (len < 48 || get_u32(r) != DISC_MAGIC || r[4] != 'R' || get_u32(r + 8) != disc_tag()) continue;
        char ip[16]; if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip)) continue;
        char mine[8][16]; const int nm = lnk_local_addresses(mine, 8); const bool me = lv_is_mine(from.sin_addr.s_addr);
        if (me) { bool had = false; for (int i = 0; i < D.n; i++) had |= D.f[i].me;
                  if (had) continue;                                    /* this PC answered already (by another of its addresses) */
                  if (nm) snprintf(ip, sizeof ip, "%s", mine[0]); }     /* this PC: its network address, not the loopback */
        bool dup = false; for (int i = 0; i < D.n; i++) dup |= !strcmp(D.f[i].ip, ip);
        if (dup || D.n >= LNK_MAX_NODES) continue;
        snprintf(D.f[D.n].ip, sizeof D.f[0].ip, "%s", ip); snprintf(D.f[D.n].name, sizeof D.f[0].name, "%.32s", (const char *)r + 12);
        D.f[D.n].me = me; D.f[D.n].cab = r[47] ? r[46] : -1; D.n++;
    }
    if (SDL_GetTicks64() >= D.until) {
        D.searching = false; D.done = true;
        qsort(D.f, (size_t)D.n, sizeof D.f[0], ip_cmp);                 /* (ip is the first field) */
        fprintf(stderr, "[LINK] Find cabinets: %d found:", D.n);
        for (int i = 0; i < D.n; i++) fprintf(stderr, " %s (%s%s)", D.f[i].ip, D.f[i].name, D.f[i].me ? ", this PC" : "");
        fprintf(stderr, "\n");
    }
}
static bool disc_searching(void) { return D.searching; }
static int  disc_count(void) { return D.n; }
static bool disc_found_usable(void) { return D.done && D.n >= 2; }
static void disc_use(void)                                              /* the found PCs become the address list (Auto numbering) */
{
    if (!disc_found_usable()) return;
    F.cabinets = D.n;
    for (int i = 0; i < LNK_MAX_NODES; i++) snprintf(F.addr[i], sizeof F.addr[0], "%s", i < D.n ? D.f[i].ip : "");
    F.cab_auto = true; F.set_id = true; F.on = true; F.find_auto = false;
    cfg_save();
    fprintf(stderr, "[LINK] Find cabinets: the %d found are now the address list (This cabinet: Auto)\n", D.n);
    D.done = false;
}
static void disc_notes(void (*line)(const char *fmt, ...))
{
    disc_poll();                                                        /* (the menu open: an unlinked game is paused, this is its poll) */
    if (D.searching) { line("Looking for PCs running this game..."); return; }
    if (!D.done) return;
    if (D.n == 0) { line("Found none. Is the game running on the others?"); return; }
    line("Found %d:", D.n);
    for (int i = 0; i < D.n; i++) {
        char c[24] = ""; if (D.f[i].cab > 0) snprintf(c, sizeof c, ", cabinet %d", D.f[i].cab); else if (!D.f[i].cab) snprintf(c, sizeof c, ", link off");
        line("   %s  %s%s%s", D.f[i].ip, D.f[i].name, c, D.f[i].me ? " (this PC)" : "");
    }
    for (int i = 0; i < D.n; i++) for (int j = i + 1; j < D.n; j++)
        if (D.f[i].cab > 0 && D.f[i].cab == D.f[j].cab) { line("WARNING: %s and %s are both cabinet %d.", D.f[i].ip, D.f[j].ip, D.f[i].cab); i = j = D.n; }
    if (D.n >= 2 && !F.find_auto) { line("\"Use the %d found\" on every PC gives them all", D.n); line("the same list."); }
}

/* THE NETWORK PAGE, the same rows in the same order in every game (Gunmen Wars' and Time Crisis II's pages follow it):
 *   Link play, This cabinet, Cabinets in set, Find others, [the addresses: Find others = Address list], Start-up wait, Link lights,
 *   If link is lost, Restart all cabinets, Find cabinets, [Use the N found], [Link method: arcade mode off]
 * A copy started by a one-PC test launcher (--link ...) shows what the launcher gave it, read-only. */
static int link_page_max_cabinets(void) { return G && G->max_cabinets > 0 && G->max_cabinets < LNK_MAX_NODES ? G->max_cabinets : LNK_MAX_NODES; }
enum { K_ON, K_CAB, K_CABS, K_FIND, K_ADDR, K_WAIT, K_LIGHTS, K_LOST, K_RESTART, K_DISC, K_USE, K_METHOD };
static int page_kinds(int *kind, int *arg)
{
    int n = 0;
    kind[n] = K_ON; arg[n++] = 0; kind[n] = K_CAB; arg[n++] = 0; kind[n] = K_CABS; arg[n++] = 0; kind[n] = K_FIND; arg[n++] = 0;
    if (!from_cmdline && !F.find_auto) for (int c = 0; c < F.cabinets; c++) { kind[n] = K_ADDR; arg[n++] = c; }
    kind[n] = K_WAIT; arg[n++] = 0; kind[n] = K_LIGHTS; arg[n++] = 0; kind[n] = K_LOST; arg[n++] = 0; kind[n] = K_RESTART; arg[n++] = 0;
    kind[n] = K_DISC; arg[n++] = 0;
    if (disc_found_usable()) { kind[n] = K_USE; arg[n++] = 0; }
    if (!F.arcade) { kind[n] = K_METHOD; arg[n++] = 0; }
    return n;
}
static int row_kind(int row, int *arg)
{
    int k[32], a[32]; const int n = page_kinds(k, a);
    if (row < 0 || row >= n) { *arg = 0; return -1; }
    *arg = a[row]; return k[row];
}
int  link_page_rows(void) { int k[32], a[32]; return page_kinds(k, a); }
bool link_page_has_value(int row) { int a; const int k = row_kind(row, &a); return k != K_ADDR && k != K_RESTART && k != K_DISC && k != K_USE; }
void link_page_text(int row, char *label, size_t ln, char *value, size_t vn)
{
    int n; const int k = row_kind(row, &n);
    *label = *value = 0;
    switch (k) {
    case K_ON: snprintf(label, ln, "Link play"); snprintf(value, vn, "%s", (from_cmdline || F.on) ? "On" : "Off"); break;
    case K_CAB: {
        snprintf(label, ln, "This cabinet");
        if (from_cmdline) { snprintf(value, vn, "%d", cfg.id + 1); break; }
        const int c = F.cab_auto ? own_cabinet(false) : F.cabinet;
        if (!F.cab_auto) snprintf(value, vn, "%d", F.cabinet);
        else if (c > 0) snprintf(value, vn, "Auto (%d)", c);
        else snprintf(value, vn, "Auto (not in list)");
        break; }
    case K_CABS: snprintf(label, ln, "Cabinets in set"); snprintf(value, vn, "%d", from_cmdline ? cfg.nodes : F.cabinets); break;
    case K_FIND: snprintf(label, ln, "Find others");
        snprintf(value, vn, "%s", from_cmdline ? "This PC (test launcher)" : F.find_auto ? "Automatic" : "Address list"); break;
    case K_ADDR: {
        const int me = F.cab_auto ? own_cabinet(false) : F.cabinet;
        snprintf(label, ln, n + 1 == me ? "Cabinet %d (this PC)" : "Cabinet %d", n + 1);
        if (edit_row == row) snprintf(value, vn, "%s_", edit_buf);
        else snprintf(value, vn, "%s", F.addr[n][0] ? F.addr[n] : "(Enter to type)");
        break; }
    case K_WAIT: snprintf(label, ln, "Start-up wait"); snprintf(value, vn, "%d s", from_cmdline ? cfg.gather_ms / 1000 : F.wait); break;
    case K_LIGHTS: snprintf(label, ln, "Link lights"); snprintf(value, vn, "%s", light_name[F.lights]); break;
    case K_LOST: snprintf(label, ln, "If link is lost"); snprintf(value, vn, "%s", F.carry_on ? "Carry on" : "Restart"); break;
    case K_RESTART: snprintf(label, ln, "Restart all cabinets");
        snprintf(value, vn, "%s", !on ? "(link play is off)" : restart_armed() ? "Press Enter AGAIN" : "Enter"); break;
    case K_DISC: snprintf(label, ln, "Find cabinets"); snprintf(value, vn, "%s", disc_searching() ? "Searching..." : "Enter"); break;
    case K_USE: snprintf(label, ln, "Use the %d found", disc_count()); snprintf(value, vn, "%s", from_cmdline ? "(test launcher)" : "Enter"); break;
    case K_METHOD: snprintf(label, ln, "Link method"); snprintf(value, vn, "%s", F.author ? "Modern multiplayer" : "Arcade LAN"); break;
    }
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
void link_page_change(int row, int dir)
{
    int n; const int k = row_kind(row, &n);
    if (k == K_LIGHTS) { F.lights = (F.lights + (dir < 0 ? 6 : 1)) % 7; cfg_save(); return; }
    if (k == K_LOST) { F.carry_on = !F.carry_on; lost_carry_on = F.carry_on; cfg_save(); return; }
    if (k == K_METHOD) { F.author = !F.author; cfg_save(); return; }   /* link method: used at the next start */
    if (k == K_RESTART) {                                              /* restart all: Enter twice (not by accident) */
        if (!on || dir) return;
        if (!restart_armed()) { restart_armed_at = SDL_GetTicks64(); return; }
        fprintf(stderr, "[LINK] this cabinet asked every cabinet to restart\n");
        lnk_send_restart();
        if (G && G->hint) G->hint("RESTARTING ALL CABINETS", 120);
        restart_in = 60;
        return;
    }
    if (k == K_DISC) { if (!dir) disc_find(); return; }
    if (k == K_USE) { if (!dir && !from_cmdline) disc_use(); return; }
    if (from_cmdline) return;                                          /* the launcher's settings: nothing here to change */
    if (k == K_ON) F.on = !F.on;
    else if (k == K_CAB) {                                             /* 1 .. N, round (with an address list: Auto too) */
        if (F.find_auto) { F.cab_auto = false; F.cabinet = (F.cabinet - 1 + (dir < 0 ? F.cabinets - 1 : 1)) % F.cabinets + 1; }
        else {
            int i = F.cab_auto ? 0 : F.cabinet;
            i = (i + (dir < 0 ? F.cabinets : 1)) % (F.cabinets + 1);
            F.cab_auto = i == 0; if (i) F.cabinet = i;
        }
        F.set_id = true;
    }
    else if (k == K_CABS) { const int c = F.cabinet; F.cabinets = clampi(F.cabinets + (dir ? dir : 1), 2, link_page_max_cabinets()); F.cabinet = clampi(F.cabinet, 1, F.cabinets); F.set_id |= F.cabinet != c; }
    else if (k == K_FIND) {
        F.find_auto = !F.find_auto;
        if (F.find_auto && F.cab_auto) { const int c = own_cabinet(false); F.cab_auto = false; F.cabinet = c > 0 ? c : F.last_auto > 0 ? F.last_auto : F.cabinet; F.set_id = true; }
    }
    else if (k == K_ADDR) {
        if (dir) return;
        edit_row = row; snprintf(edit_buf, sizeof edit_buf, "%s", F.addr[n]);   /* Enter: type the address */
        return;
    } else if (k == K_WAIT) F.wait = clampi(F.wait + (dir ? dir : 1) * 10, 10, 300);
    else return;
    cfg_save();
}
/* FRAME TIMING (link.cfg timing=; shown on the Display page, not here: it is not about the network) */
void link_timing_text(char *value, size_t vn)
{
    snprintf(value, vn, "%s%s", F.classic ? "Classic (0.4.0)" : "New", eng_timing_classic() != F.classic ? " - at next start" : "");
}
void link_timing_change(void) { F.classic = !F.classic; cfg_save(); }
bool link_page_editing(void) { return edit_row >= 0; }
/* one key while an address is typed: digits and dots; Backspace deletes; Enter keeps it; Esc drops it. false = typing is over */
bool link_page_type(SDL_Scancode sc)
{
    if (edit_row < 0) return false;
    size_t n = strlen(edit_buf);
    char c = 0;
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) c = (char)('1' + (sc - SDL_SCANCODE_1));
    else if (sc == SDL_SCANCODE_0) c = '0';
    else if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) c = (char)('1' + (sc - SDL_SCANCODE_KP_1));
    else if (sc == SDL_SCANCODE_KP_0) c = '0';
    else if (sc == SDL_SCANCODE_PERIOD || sc == SDL_SCANCODE_KP_PERIOD) c = '.';
    if (c) { if (n + 1 < sizeof edit_buf) { edit_buf[n] = c; edit_buf[n + 1] = 0; } return true; }
    if (sc == SDL_SCANCODE_BACKSPACE) { if (n) edit_buf[n - 1] = 0; return true; }
    if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) { int c; if (row_kind(edit_row, &c) == K_ADDR) { snprintf(F.addr[c], sizeof F.addr[0], "%s", edit_buf); cfg_save(); } }
    if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_UNKNOWN) { edit_row = -1; return false; }
    return true;                                                       /* any other key: ignored, still typing */
}
void link_page_notes(void (*line)(const char *fmt, ...))
{
    char a[4][48]; const int n = lnk_local_named(a, 4);
    if (!n) line("This PC: no network address found");
    for (int i = 0; i < n; i++) line("%s %s", i ? "        " : "This PC:", a[i]);
    if (from_cmdline) {
        line("Set by the test launcher (Cabinet %d of %d).", cfg.id + 1, cfg.nodes);
        line("For a real cabinet, use 'play as a cabinet'.");
    } else {
        if (edit_row >= 0) line("Type the address, then Enter (Esc cancels).");
        if (F.find_auto) { line("Automatic: the cabinets find each other."); line("Give every PC its own number. Used at next start."); }
        else line("Same list on every cabinet. Used at next start.");
        if (!F.find_auto && F.cab_auto) line("Auto: the number is this PC's place in the list.");
        { char tw[16]; if (on && lnk_twin(tw)) { line("WARNING: the PC at %s is cabinet %d too:", tw, cfg.id + 1); line("give each PC its own number."); } }
        if (method_at_start != modern()) line("Link method: %s at next start.", modern() ? "Modern multiplayer" : "Arcade LAN");
        else if (modern()) line("Modern multiplayer: lobby play on the Online page.");
        else line("Arcade LAN: real cabinets, linked at power-on.");
        if (F.set_id) line("The game's own ID will follow at next start.");
    }
    if (on) {
        char who[40] = ""; int j = 0;
        for (int m = 0; m < cfg.nodes; m++) if (lnk_members() >> m & 1) j += snprintf(who + j, sizeof who - (size_t)j, "%s%d", j ? "+" : "", m + 1);
        if (lnk_state() == LNK_ALONE) line("Status: Cabinet %d, NOT linked (started late)", cfg.id + 1);
        else if (lnk_members() == 1u << cfg.id) line("Status: Cabinet %d, playing solo", cfg.id + 1);
        else line("Status: Cabinet %d, linked with %s", cfg.id + 1, who);
        char hb[160]; hearing(hb, sizeof hb, false);
        line("%s", hb);
        if (*setup_msg_shown) line("%s", setup_msg_shown);
        char qb[160]; quality_text(qb, sizeof qb);
        if (*qb) line("Link quality: %s", qb);
        line("The game keeps running while this menu is open.");
    } else if (method_at_start) line("Status: Modern multiplayer (see the Online page)");
    else if (auto_fail) line("Status: NOT linked (this PC is not in the list)");
    else line("Status: link off");
    if (F.lights) line("Lights: green OK, amber one-way, red silent, grey out");
    disc_notes(line);
}

/* THE LINK LIGHTS (Network page > Link lights): one light per cabinet of the set, at the bottom left of the screen. This cabinet is
 * blue and flickers as it sends; another is green (both ways) or amber (one way: it is heard here but does not hear this one, or its
 * set-up differs) and flickers as its messages arrive, red when nothing comes from it, grey when it is not in this set. */
bool link_lights_on(void) { return on && F.lights && lnk_state() != LNK_GATHERING; }
int  link_lights_pos(void) { return link_lights_on() ? F.lights : 0; }
int link_lights(link_light *out)
{
    if (!link_lights_on()) return 0;
    const uint32_t h = lnk_hear_mask(), by = lnk_heard_by_mask(), mem = lnk_state() == LNK_LINKED ? lnk_members() : 0;
    int k = 0;
    for (int c = 0; c < cfg.nodes && k < LINK_LIGHTS_MAX; c++, k++) {
        link_light *l = &out[k];
        l->cabinet = c + 1; l->lit = true;
        if (c == cfg.id) { l->kind = LINK_LIGHT_SELF; l->lit = (lnk_get_stats()->sent / 8) & 1; continue; }
        char a[16]; int ago; uint32_t pk = 0;
        lnk_peer_info(c, a, &ago, &pk);
        bool same = true; const int nodes = lnk_peer_setup(c, &same);
        uint32_t bc, bt; if (lnk_peer_build(c, &bc, &bt) && bc != cfg.build_crc) same = false;
        if (!(mem >> c & 1)) l->kind = LINK_LIGHT_OUT;
        else if (!(h >> c & 1)) l->kind = LINK_LIGHT_SILENT;
        else { l->kind = (by >> c & 1) && (!nodes || (nodes == cfg.nodes && same)) ? LINK_LIGHT_GOOD : LINK_LIGHT_ONEWAY; l->lit = (pk / 12) & 1; }
    }
    return k;
}
