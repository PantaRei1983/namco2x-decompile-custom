/*
 * ff_link.c -- FINAL FURLONG'S CABINET LINK (our fork). Up to 4 cabinets, each board's C422 on an RS-422 RING: a cabinet's messages go to
 * the next one, which passes each on (the game's own driver does that), round to where it started. Here each PC models its C422 (below,
 * in place of the engine's cable-less one while linked) and the ring's cables are carried over UDP to the next PC.
 *
 * THE BUS LAYER is Gunmen Wars' (gunwars/src/gw_link.c): the session at power-on (cabinets link when they are switched on together), the
 * games kept in step on game time (each message is due at the receiver's game time = the sender's + LAG; a cabinet more than LAG ahead
 * of the slowest waits), a cabinet silent for 5 s is lost and the ring closes round it, the Network page, Find cabinets.
 *
 * THE GAME'S PCB NUMBER (test menu, GAME OPTIONS, 1-4; RAM 0x801EB01C) is this cabinet's number while linked; two boards with one number
 * would not race each other.
 *
 * SETTINGS: the link lines of finfurl.cfg beside the program (added on the first run: link = off, link_cabinet, link_cabinets,
 * link_address, link_port, link_wait, link_lights, link_net).
 *   FF_LINK=<cabinet>:<cabinets>:<first port>   (tests: copies on one PC) e.g. FF_LINK=1:2:23200 and FF_LINK=2:2:23200
 *   FF_LINK_WAIT=<s>  FF_LINK_LAG=<frames>  FF_LINKLOG=1 (each message to the log)
 *
 * DATAGRAMS: "FFL", version, the sender's cabinet (0-based), the type, the network setting, 0, the session, a sequence number (16 bytes), then
 *   H hello   cabinets, state, 0, 0, the sender's number (4), its session (4)                   -- 100 ms while waiting, 500 ms after
 *   P message the sender's game time (8), the message's time (8), words (2), the words (2 each, 9 bits)   -- to the next cabinet only
 *   T time    the sender's game time (8)
 *   B bye     (closing)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#else
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesocket close
#endif
#include <SDL2/SDL.h>
#include "s23_host.h"
#include "s23_game.h"
#include "../../engine/lan_via.h"         /* out of the connection each cabinet was heard on (a VPN on 169.254.x.x) */

uint64_t s23_cycles_now(void);
uint8_t *s23_vram_ptr(uint32_t p);
extern uint32_t rr_frame;

#define MAXCAB 4
#define VERSION 1
#define FRAME_CYCLES 2257920u                 /* 135.4752 MHz / 60 (GORgON) */
#define AHEAD_MARGIN 20000u                   /* the main CPU runs a slice past the check: wait that much earlier */
#define WORD_CYCLES 1355u                     /* one word on the line: 1 Mbit/s, 10 bits -- a guess, unmeasured */

/* ================= THE CABLE ================= */
typedef struct {
    int cabinet, cabinets;                    /* this one (0-based), how many */
    char address[256];                        /* "auto" (found on the network), a list "a,b,c", or "loop" (tests: 127.0.0.1, port + cabinet) */
    unsigned port;
} ff_link_cfg;

typedef struct {
    const char *name;
    int  (*open)(const ff_link_cfg *c, char *why, size_t wn);   /* 1 = ready */
    void (*send)(int cab, const uint8_t *b, int n);             /* to one cabinet (0-based); -1 = to all the others, found or not */
    int  (*recv)(uint8_t *b, int max);                          /* the next datagram's length, 0 = none */
    void (*wait)(int ms);                                       /* until something arrives, at most ms */
    void (*close)(void);
} ff_link_transport;

/* ---- UDP on the local network ---- */
static sock_t us = BAD_SOCK;
static ff_link_cfg U;
static struct sockaddr_in u_peer[MAXCAB]; static int u_known[MAXCAB]; static uint32_t u_via[MAXCAB];   /* u_via: the connection each was heard on */
static struct sockaddr_in u_list[MAXCAB]; static int u_nlist;
static int u_auto, u_loop;

static int udp_open(const ff_link_cfg *c, char *why, size_t wn)
{
    U = *c;
#ifdef _WIN32
    { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
#endif
    us = socket(AF_INET, SOCK_DGRAM, 0);
    if (us == BAD_SOCK) { snprintf(why, wn, "NO NETWORK SOCKET"); return 0; }
    u_loop = !strcmp(c->address, "loop");
    u_auto = !strcmp(c->address, "auto");
    const unsigned my_port = u_loop ? c->port + (unsigned)c->cabinet : c->port;
    struct sockaddr_in me; memset(&me, 0, sizeof me);
    me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(u_loop ? INADDR_LOOPBACK : INADDR_ANY); me.sin_port = htons((uint16_t)my_port);
    if (bind(us, (struct sockaddr *)&me, sizeof me) != 0) {
        snprintf(why, wn, "PORT %u IS IN USE - IS THE GAME ALREADY RUNNING?", my_port);
        closesocket(us); us = BAD_SOCK; return 0;
    }
#ifdef _WIN32
    { u_long nb = 1; ioctlsocket(us, FIONBIO, &nb); }
    { BOOL yes = TRUE; setsockopt(us, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes); }
    { int sz = 4 << 20; setsockopt(us, SOL_SOCKET, SO_RCVBUF, (const char *)&sz, sizeof sz); }   /* bursts from several cabinets (loopback lost a few with 3) */
    lv_prepare(us); memset(u_via, 0, sizeof u_via);
#else
    fcntl(us, F_SETFL, fcntl(us, F_GETFL) | O_NONBLOCK);
    { int yes = 1; setsockopt(us, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes); }
#endif
    if (u_loop) {
        for (int j = 0; j < c->cabinets; j++) {
            memset(&u_peer[j], 0, sizeof u_peer[j]); u_peer[j].sin_family = AF_INET;
            u_peer[j].sin_addr.s_addr = htonl(INADDR_LOOPBACK); u_peer[j].sin_port = htons((uint16_t)(c->port + (unsigned)j)); u_known[j] = 1;
        }
    } else if (!u_auto) {                                      /* a list of the other PCs */
        char list[256]; snprintf(list, sizeof list, "%s", c->address);
        for (char *t = strtok(list, ", "); t && u_nlist < MAXCAB; t = strtok(NULL, ", ")) {
            struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(t, NULL, &hints, &res) != 0 || !res) { fprintf(stderr, "[LINK] cannot find %s\n", t); continue; }
            u_list[u_nlist] = *(struct sockaddr_in *)res->ai_addr; u_list[u_nlist].sin_port = htons((uint16_t)c->port); u_nlist++;
            freeaddrinfo(res);
        }
        if (!u_nlist) { fprintf(stderr, "[LINK] no address in the list: looking on the whole network instead\n"); u_auto = 1; }
    }
    return 1;
}
static void udp_send(int cab, const uint8_t *b, int n)
{
    if (us == BAD_SOCK) return;
    if (cab >= 0) { if (cab < MAXCAB && u_known[cab]) lv_send(us, b, n, &u_peer[cab], u_via[cab]); return; }
    int all = 1;
    for (int j = 0; j < U.cabinets; j++) if (j != U.cabinet) { if (u_known[j]) lv_send(us, b, n, &u_peer[j], u_via[j]); else all = 0; }
    if (all) return;
    if (u_auto) lv_send_all(us, b, n, htons((uint16_t)U.port));  /* not all found yet: to every PC on the network (out of every connection) */
    else for (int i = 0; i < u_nlist; i++) sendto(us, (const char *)b, n, 0, (struct sockaddr *)&u_list[i], sizeof u_list[i]);
}
static int udp_recv(uint8_t *b, int max)
{
    if (us == BAD_SOCK) return 0;
    for (;;) {
        struct sockaddr_in from; uint32_t ifx = 0;
        const int r = lv_recv(us, b, max, &from, &ifx);
        if (r <= 0) return 0;
        if (r < 16 || memcmp(b, "FFL", 3)) continue;
        const int cab = b[4];
        if (cab >= MAXCAB || cab == U.cabinet) continue;       /* our own broadcast */
        if (!u_loop) { u_peer[cab] = from; u_known[cab] = 1; u_via[cab] = ifx; }   /* where that cabinet is (it answers from its port), and
                                                                                 * which connection: the replies go back out of it */
        return r;
    }
}
static void udp_wait(int ms)
{
    if (us == BAD_SOCK) { SDL_Delay((Uint32)ms); return; }
    fd_set fs; FD_ZERO(&fs); FD_SET(us, &fs);
    struct timeval tv = { 0, ms * 1000 };
    select((int)us + 1, &fs, NULL, NULL, &tv);
}
static void udp_close(void) { if (us != BAD_SOCK) closesocket(us); us = BAD_SOCK; }

static const ff_link_transport udp_transport = { "UDP (local network)", udp_open, udp_send, udp_recv, udp_wait, udp_close };
static const ff_link_transport *T = &udp_transport;    /* an online transport would be chosen here (finfurl.cfg) */

/* ================= THE BUS ================= */
enum { ST_OFF, ST_WAIT, ST_LINKED, ST_ALONE };
static int state = ST_OFF, me, ncab, wait_s = 60, logon;
static uint32_t my_nonce, nonce_of[MAXCAB], session;
static int heard[MAXCAB], peer_state[MAXCAB], lost[MAXCAB];
static uint32_t peer_session[MAXCAB];
static uint64_t peer_time[MAXCAB], heard_ms[MAXCAB];
static uint32_t tx_seq, rx_seq_last[MAXCAB];
static uint64_t lag = FRAME_CYCLES / 10, poll_every, poll_at, hello_ms;   /* poll_every: lag / 4 (the cable read, our packets sent) */
/* THE NETWORK SETTING (Link page "Network", link_net in finfurl.cfg): how far behind the others' packets are played, = how much delay
 * and jitter the link rides out before a cabinet has to wait. 0 LAN 0.1 frame (as before), 1 Wi-Fi 1 frame, 2 slow 3 frames, 3 internet
 * 6 frames. Each hello carries it (header byte 6); while the cabinets find each other every one takes the LARGEST, so all play alike */
static int net_level;
static const char *const net_key[4] = { "lan", "wifi", "slow", "internet" };
static const double net_frames[4] = { 0.1, 1.0, 3.0, 6.0 };
static void set_lag(void)
{
    lag = (uint64_t)(net_frames[net_level] * FRAME_CYCLES);
    if (getenv("FF_LINK_LAG")) lag = (uint64_t)(atof(getenv("FF_LINK_LAG")) * FRAME_CYCLES);
    if (lag < 4 * AHEAD_MARGIN) lag = 4 * AHEAD_MARGIN;
    poll_every = lag / 4;
}
static char why_alone[96], warn_text[112];
static unsigned long n_dgram_out, n_dgram_in, n_pkt_out, n_pkt_in, n_drops, n_late, n_waits;
static uint64_t wait_ms_total, wait_ms_max;

static int linked_hook(void) { return state == ST_LINKED; }
static uint64_t ms_now(void) { return (uint64_t)SDL_GetTicks64(); }
static void put16(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static void put64(uint8_t *b, uint64_t v) { put32(b, (uint32_t)(v >> 32)); put32(b + 4, (uint32_t)v); }
static uint64_t get64(const uint8_t *b) { return (uint64_t)get32(b) << 32 | get32(b + 4); }
static uint32_t make_session(void)
{
    uint32_t h = 0x9E3779B1u;
    for (int i = 0; i < ncab; i++) { h = (h ^ nonce_of[i]) * 0x85EBCA77u; h ^= h >> 13; }
    return h ? h : 1;
}
static int hdr(uint8_t *b, char type)
{
    memcpy(b, "FFL", 3); b[3] = VERSION; b[4] = (uint8_t)me; b[5] = (uint8_t)type; b[6] = (uint8_t)net_level; b[7] = 0;
    put32(b + 8, session); put32(b + 12, type == 'H' ? 0 : tx_seq++);   /* hellos are not counted (read before the sequence check) */
    return 16;
}
/* ---- the settings: the link_ lines of finfurl.cfg (the game's settings file; the engine rewrites only its own lines and keeps these) ---- */
#define CFG_FILE "finfurl.cfg"
static void cfg_read(ff_link_cfg *c, int *on)
{
    FILE *f = fopen(CFG_FILE, "r");
    int found = 0;
    if (f) {
        char line[320];
        while (fgets(line, sizeof line, f)) {
            char k[64], v[256];
            if (line[0] == '#' || sscanf(line, " %63[^= ] = %255s", k, v) != 2 || strncmp(k, "link", 4)) continue;
            found = 1;
            if (!strcmp(k, "link")) *on = !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "yes");
            else if (!strcmp(k, "link_cabinet")) c->cabinet = !strcmp(v, "auto") ? -2 : atoi(v) - 1;   /* -2: Auto (its place in the address list) */
            else if (!strcmp(k, "link_cabinets")) c->cabinets = atoi(v);
            else if (!strcmp(k, "link_address")) snprintf(c->address, sizeof c->address, "%s", v);
            else if (!strcmp(k, "link_port")) c->port = (unsigned)atoi(v);
            else if (!strcmp(k, "link_wait")) wait_s = atoi(v);
            else if (!strcmp(k, "link_net")) { for (int i = 0; i < 4; i++) if (!strcmp(v, net_key[i])) net_level = i; }
        }
        fclose(f);
    }
    if (found) return;
    f = fopen(CFG_FILE, "a");                                  /* the first run: the link's lines, off, to fill in */
    if (f) {
        fprintf(f, "# CABINET LINK. link = on to use it. link_cabinet: this cabinet's number (1, 2, ...), a different one on every PC\n"
                   "# (the game's PCB NUMBER follows it). link_cabinets: how many are linked. link_address: auto finds the others on the\n"
                   "# network; or list the other PCs, e.g. 192.168.1.20,192.168.1.21; loop = copies on this one PC (each in its own folder).\n"
                   "# link_port: the same on every PC. link_wait: seconds to wait at power-on for the others. link_lights: off, bottom-left,\n"
                   "# bottom-centre, bottom-right, top-left, top-centre, top-right.\n"
                   "# link_net: lan, wifi, slow or internet (more delay, rides out a worse network; the linked PCs use the slowest)\n"
                   "link = off\nlink_cabinet = 1\nlink_cabinets = 2\nlink_address = auto\nlink_port = 23200\nlink_wait = 60\nlink_lights = bottom-left\nlink_net = lan\n");
        fclose(f);
    }
}
static int lights_pos = 1;
static void lights_read(void)
{
    static const char *const key[7] = { "off", "bottom-left", "bottom-centre", "bottom-right", "top-left", "top-centre", "top-right" };
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char k[64], v[64];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %63s", k, v) != 2 || strcmp(k, "link_lights")) continue;
        lights_pos = 0;
        for (int i = 1; i < 7; i++) if (!strcmp(v, key[i])) lights_pos = i;
    }
    fclose(f);
}

static void open_link(void)
{
    static int done; if (done) return; done = 1;
    logon = getenv("FF_LINKLOG") != NULL;
    ff_link_cfg c; memset(&c, 0, sizeof c); c.cabinets = 2; c.port = 23200; snprintf(c.address, sizeof c.address, "auto");
    int on = 0;
    const char *e = getenv("FF_LINK");
    if (e && *e) {
        int cab = 0, n = 0; unsigned port = 0;
        if (sscanf(e, "%d:%d:%u", &cab, &n, &port) != 3) { fprintf(stderr, "[LINK] FF_LINK=%s: expected cabinet:cabinets:first port\n", e); return; }
        c.cabinet = cab - 1; c.cabinets = n; c.port = port; snprintf(c.address, sizeof c.address, "loop"); on = 1;
    } else cfg_read(&c, &on);
    lights_read();
    { void disc_start_answering_(void); disc_start_answering_(); }   /* other PCs' "Find cabinets" can see this one, the link on or off */
    if (on && c.cabinet == -2) {                                  /* This cabinet: Auto -- its place in the address list */
        int n = 0, pos; int auto_place_(const char *, int *); pos = auto_place_(c.address, &n);
        if (!pos) { fprintf(stderr, "[LINK] This cabinet is Auto, but this PC's address is not in the address list (%s): this cabinet plays alone\n", c.address);
                    snprintf(why_alone, sizeof why_alone, "THIS PC IS NOT IN THE LINK'S ADDRESS LIST - USE FIND CABINETS"); state = ST_ALONE; return; }
        c.cabinet = pos - 1; c.cabinets = n;
        fprintf(stderr, "[LINK] This cabinet is Auto: number %d of %d (its place in the address list)\n", pos, n);
    }
    if (getenv("FF_LINK_WAIT")) wait_s = atoi(getenv("FF_LINK_WAIT"));
    if (getenv("FF_LINK_NET")) net_level = atoi(getenv("FF_LINK_NET")) & 3;   /* tests: 0 lan .. 3 internet */
    set_lag();
    if (!on) { fprintf(stderr, "[LINK] link off (finfurl.cfg): this cabinet plays alone\n"); return; }
    if (c.cabinets < 2 || c.cabinets > MAXCAB || c.cabinet < 0 || c.cabinet >= c.cabinets) {
        fprintf(stderr, "[LINK] link_cabinet %d of link_cabinets %d does not fit (2-%d cabinets, link_cabinet 1 to link_cabinets): this cabinet plays alone\n", c.cabinet + 1, c.cabinets, MAXCAB);
        snprintf(why_alone, sizeof why_alone, "CHECK LINK_CABINET AND LINK_CABINETS IN GUNWARS.CFG"); state = ST_ALONE; return;
    }
    me = c.cabinet; ncab = c.cabinets;
    if (!T->open(&c, why_alone, sizeof why_alone)) { fprintf(stderr, "[LINK] %s: this cabinet plays alone\n", why_alone); state = ST_ALONE; return; }
    my_nonce = (uint32_t)SDL_GetPerformanceCounter() ^ (uint32_t)time(NULL) * 2654435761u ^ (uint32_t)(uintptr_t)&c;
    if (!my_nonce) my_nonce = 1;
    nonce_of[me] = my_nonce; heard[me] = 1;
    for (int i = 0; i < MAXCAB; i++) rx_seq_last[i] = 0xFFFFFFFFu;
    state = ST_WAIT;
    fprintf(stderr, "[LINK] link on (%s): this is cabinet %d of %d, port %u, the others %s\n", T->name, me + 1, ncab, c.port,
            !strcmp(c.address, "auto") ? "found on the network" : c.address);
}
static void send_hello(void)
{
    uint8_t b[32]; int p = hdr(b, 'H');
    b[p++] = (uint8_t)ncab; b[p++] = (uint8_t)state; b[p++] = 0; b[p++] = 0;
    put32(b + p, my_nonce); p += 4; put32(b + p, session); p += 4;
    T->send(-1, b, p);
}
static void go_linked(void)
{
    session = make_session(); state = ST_LINKED;
    const uint64_t t = ms_now();
    for (int i = 0; i < ncab; i++) heard_ms[i] = t;
    fprintf(stderr, "[LINK] LINKED: %d cabinets, this is cabinet %d (bus node %d%s), session %08X\n", ncab, me + 1, me, me == ncab - 1 ? ", the root" : "", session);
    for (int i = 0; i < 3; i++) send_hello();
}
static void go_alone(const char *why)
{
    if (state == ST_ALONE) return;
    state = ST_ALONE; snprintf(why_alone, sizeof why_alone, "%s", why);
    fprintf(stderr, "[LINK] NOT linked: %s -- this cabinet plays alone (restart all the cabinets to link them)\n", why);
}
static void on_hello(int cab, const uint8_t *b, int r)
{
    if (r < 28) return;
    if (b[16] != ncab) {
        static int said;
        snprintf(warn_text, sizeof warn_text, "CABINET %d IS SET FOR %d CABINETS, THIS ONE FOR %d - SET LINK_CABINETS THE SAME ON ALL", cab + 1, b[16], ncab);
        if (!said) { said = 1; fprintf(stderr, "[LINK] WARNING: %s\n", warn_text); }
        return;
    }
    if (cab >= ncab) return;
    const uint32_t nonce = get32(b + 20);
    if (state == ST_WAIT && heard[cab] && nonce != nonce_of[cab] && b[17] == ST_WAIT && peer_state[cab] == ST_WAIT && ms_now() - heard_ms[cab] < 400) {
        static int said;                                       /* two PCs, one number: both say "cabinet c" at once */
        snprintf(warn_text, sizeof warn_text, "TWO PCS ARE SET TO CABINET %d - GIVE EACH PC ITS OWN NUMBER", cab + 1);
        if (!said) { said = 1; fprintf(stderr, "[LINK] WARNING: %s\n", warn_text); }
    }
    if (state == ST_LINKED) {
        if (nonce != nonce_of[cab]) { static uint32_t told; if (told != nonce) { told = nonce; fprintf(stderr, "[LINK] cabinet %d restarted: it plays alone until all are restarted\n", cab + 1); } }
        return;
    }
    if (state != ST_WAIT) return;
    if (b[6] > net_level && b[6] < 4) { net_level = b[6]; set_lag();
        fprintf(stderr, "[LINK] cabinet %d is set to network \"%s\": this one plays the same (%.1f frames behind)\n", cab + 1, net_key[net_level], net_frames[net_level]); }
    nonce_of[cab] = nonce; heard[cab] = 1; peer_state[cab] = b[17]; peer_session[cab] = get32(b + 24); heard_ms[cab] = ms_now();
    for (int i = 0; i < ncab; i++) if (!heard[i]) return;     /* not all here yet */
    const uint32_t s = make_session();
    for (int i = 0; i < ncab; i++)
        if (i != me && peer_state[i] == ST_LINKED && peer_session[i] != s) { go_alone("ANOTHER CABINET WAS ALREADY PLAYING"); return; }
    go_linked();
}

/* ================= THE CHIP (the C422 as Final Furlong's driver uses it; read off the game: 0x80027700 reset, 0x80027F68 send,
 * 0x80029674 the interrupt handler, which also passes each received message on: 0x80029D74) =================
 * Registers (16-bit, 0x0F300000 on GORgON): +0 STATUS (bit 1 a message arrived -- the driver clears it by writing it 0; bit 2 the send is
 * done -- the driver never clears it: here READING the status clears it, as an edge interrupt would), +2 MASK (a set bit masks), +6 CONTROL
 * (1 while a send is set up; it goes when bit 0 drops), +A COUNT (words to send, reads what is left), +C RX (the receive ring's position,
 * 12 bits: here the LAST word written -- the driver looks there for the end mark, bit 8, and back from it), +E TX (13 bits: bit 12 sends
 * out of the receive half, which is how a message is passed on). RAM at 0x0F200000: send half words 0-0xFFF, receive ring 0x1000-0x1FFF,
 * 9 bits a word (bit 8 = the message's last word, set by the game itself). */
static uint16_t reg[8];
static int tx_busy; static uint64_t tx_next;
static uint16_t txw[0x1000]; static int txn;                  /* the message going out */
static uint16_t ram_rd(uint32_t w) { const uint8_t *m = s23_vram_ptr(0x06200000u + 2 * (w & 0x1FFF)); return m ? (uint16_t)(m[0] << 8 | m[1]) : 0; }
static void ram_wr(uint32_t w, uint16_t v) { uint8_t *m = s23_vram_ptr(0x06200000u + 2 * (w & 0x1FFF)); if (m) { m[0] = (uint8_t)(v >> 8); m[1] = (uint8_t)v; } }
static void irq_update(void) { s23_irq_set(S23_SRC_C422, (reg[0] & ~reg[1] & 6) != 0); }
static void ring_send(const uint16_t *w, int n, uint64_t t);
static void tx_start(uint64_t now) { if (reg[5] & 0xFFF) { tx_busy = 1; tx_next = now + WORD_CYCLES; txn = 0; } }
static void chip_run(uint64_t now)
{
    while (tx_busy && now >= tx_next) {
        if (txn < 0x1000) txw[txn++] = ram_rd(reg[7]) & 0x1FF;
        reg[7] = (uint16_t)((reg[7] & 0x1000) | ((reg[7] + 1) & 0xFFF));
        reg[5] = (uint16_t)((reg[5] - 1) & 0xFFF);
        if (!reg[5]) { tx_busy = 0; reg[0] |= 4; ring_send(txw, txn, tx_next); irq_update(); }
        else tx_next += WORD_CYCLES;
    }
}
static void chip_receive(const uint16_t *w, int n)               /* a message from the cabinet before this one in the ring */
{
    uint16_t at = reg[6];
    for (int i = 0; i < n; i++) { at = (uint16_t)((at + 1) & 0xFFF); ram_wr(0x1000u | at, w[i]); }
    reg[6] = at;
    reg[0] |= 2; irq_update();
}
static int chip_read(unsigned k, uint32_t *v)
{
    if (state != ST_LINKED) return 0;                            /* alone: the engine's (MAME's) chip, as before */
    *v = reg[k];
    if (k == 0 && (reg[0] & 4)) { reg[0] &= (uint16_t)~4u; irq_update(); }
    return 1;
}
static int chip_write(unsigned k, uint32_t v)
{
    if (state != ST_LINKED) return 0;
    const uint16_t old = reg[k];
    switch (k) {
    case 0: reg[0] = (uint16_t)((reg[0] & v & 6u) | (v & ~6u)); break;
    case 3: reg[3] = (uint16_t)v; if ((old & 1) && !(v & 1) && !tx_busy) tx_start(s23_cycles_now()); break;
    case 5: reg[5] = (uint16_t)(v & 0xFFF); if (!(reg[3] & 1) && !tx_busy) tx_start(s23_cycles_now()); break;
    case 7: reg[7] = (uint16_t)(v & 0x1FFF); break;
    default: reg[k] = (uint16_t)v; break;
    }
    irq_update();
    return 1;
}

/* ---- messages out: each to the next cabinet in the ring (round any that are lost) ---- */
static int next_cab(void) { for (int k = 1; k < ncab; k++) { const int c = (me + k) % ncab; if (!lost[c]) return c; } return -1; }
static void ring_send(const uint16_t *w, int n, uint64_t t)
{
    const int to = next_cab();
    if (state != ST_LINKED || to < 0 || n <= 0) return;
    static uint8_t b[16 + 18 + 2 * 0x1000]; int p = hdr(b, 'P');
    put64(b + p, s23_cycles_now()); p += 8; put64(b + p, t); p += 8; put16(b + p, (uint32_t)n); p += 2;
    for (int i = 0; i < n; i++) { put16(b + p, w[i]); p += 2; }
    T->send(to, b, p);
    if (logon) { fprintf(stderr, "[LINK] f%u out %d words to cabinet %d:", rr_frame, n, to + 1); for (int i = 0; i < n && i < 12; i++) fprintf(stderr, " %03X", w[i]); fprintf(stderr, "\n"); }
    n_dgram_out++; n_pkt_out++;
}

/* ---- messages in: a queue in order of their due time (one sender, the cabinet before this one: they come in order) ---- */
#define RXQ 64
typedef struct { uint64_t due; int n; uint16_t w[0x1000]; } rx_msg;
static rx_msg rxq[RXQ]; static int rx_head, rx_n;
static uint64_t delivered_to;
static void queue_in(uint64_t due, const uint8_t *b, int n)
{
    if (rx_n == RXQ) { static int said; if (!said++) fprintf(stderr, "[LINK] the receive queue is full: messages dropped\n"); return; }
    rx_msg *m = &rxq[(rx_head + rx_n) % RXQ]; m->due = due; m->n = n;
    for (int i = 0; i < n; i++) m->w[i] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
    rx_n++;
}
static void deliver(uint64_t t)
{
    while (rx_n && rxq[rx_head].due <= t) {
        chip_receive(rxq[rx_head].w, rxq[rx_head].n);
        if (logon) fprintf(stderr, "[LINK] f%u in %d words (rx %03X)\n", rr_frame, rxq[rx_head].n, reg[6]);
        rx_head = (rx_head + 1) % RXQ; rx_n--; n_pkt_in++;
    }
    if (t > delivered_to) delivered_to = t;
}

static void poll_cable(void)
{
    if (state != ST_WAIT && state != ST_LINKED) return;
    const uint64_t t = ms_now();
    if (t - hello_ms >= (state == ST_WAIT ? 100u : 500u)) { hello_ms = t; send_hello(); }
    for (int k = 0; k < 256; k++) {
        static uint8_t b[16 + 18 + 2 * 0x1000 + 64];
        const int r = T->recv(b, sizeof b);
        if (r <= 0) return;
        if (b[3] != VERSION) {
            static int said;
            snprintf(warn_text, sizeof warn_text, "ANOTHER PC HAS A DIFFERENT VERSION OF THE GAME - COPY THE SAME KIT TO ALL");
            if (!said) { said = 1; fprintf(stderr, "[LINK] WARNING: a PC runs another version of the link (%d, this is %d)\n", b[3], VERSION); }
            continue;
        }
        const int cab = b[4];
        if (cab >= ncab) continue;
        if (b[5] == 'H') { on_hello(cab, b, r); continue; }
        if (state != ST_LINKED || get32(b + 8) != session) continue;   /* not from our session */
        heard_ms[cab] = ms_now();
        const uint32_t seq = get32(b + 12);
        if (rx_seq_last[cab] != 0xFFFFFFFFu && (int32_t)(seq - rx_seq_last[cab]) <= 0) continue;   /* a duplicate or a late one */
        if (rx_seq_last[cab] != 0xFFFFFFFFu && seq != rx_seq_last[cab] + 1) n_drops += seq - rx_seq_last[cab] - 1;
        rx_seq_last[cab] = seq;
        n_dgram_in++;
        if (b[5] == 'B') { if (!lost[cab]) fprintf(stderr, "[LINK] cabinet %d was closed (frame %u)\n", cab + 1, rr_frame); lost[cab] = 1; continue; }
        if (r < 24) continue;
        const uint64_t ts = get64(b + 16);
        if (ts > peer_time[cab]) peer_time[cab] = ts;
        if (b[5] != 'P' || r < 34) continue;
        const int n = b[32] << 8 | b[33];
        if (34 + 2 * n > r || n > 0x1000) continue;
        const uint64_t due = get64(b + 24) + lag;
        if (due < delivered_to) n_late++;
        queue_in(due, b + 34, n);
    }
}
static void send_time(uint64_t now)
{
    if (state != ST_LINKED) return;
    uint8_t b[24]; int p = hdr(b, 'T'); put64(b + p, now); p += 8;   /* (every message up to now has gone: chip_run runs first) */
    T->send(-1, b, p);
}
/* AHEAD OF THE SLOWEST CABINET by more than LAG: wait for it (its game stalled, or its PC runs a little slow); the window is kept alive */
static int behind(uint64_t now)
{
    for (int i = 0; i < ncab; i++) if (i != me && !lost[i] && now + AHEAD_MARGIN > peer_time[i] + lag) return i;
    return -1;
}
static void wait_for_others(uint64_t now)
{
    if (state != ST_LINKED || behind(now) < 0) return;
    const uint64_t t0 = ms_now(); uint64_t told = t0;
    send_time(now);
    int b;
    while ((b = behind(now)) >= 0) {
        T->wait(2);
        poll_cable();
        const uint64_t t = ms_now();
        if (t - told >= 20) { told = t; send_time(now); if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_PumpEvents(); }
        if (t - heard_ms[b] >= 5000) {
            lost[b] = 1;
            fprintf(stderr, "[LINK] cabinet %d has not answered for 5 s (frame %u): this cabinet plays on without it\n", b + 1, rr_frame);
        }
    }
    const uint64_t w = ms_now() - t0;
    n_waits++; wait_ms_total += w; if (w > wait_ms_max) wait_ms_max = w;
    if (w >= 100) fprintf(stderr, "[LINK] waited %llu ms for the others (frame %u)\n", (unsigned long long)w, rr_frame);
}

/* ================= THE ENGINE'S HOOKS (s23_game.link) ================= */
static void gate(int (*pump)(const char *, const char *))
{
    open_link();
    if (state != ST_WAIT) return;
    const uint64_t t0 = ms_now();
    while (state == ST_WAIT) {
        poll_cable();
        const int left = wait_s - (int)((ms_now() - t0) / 1000);
        if (left <= 0) { go_alone("THE OTHER CABINETS DID NOT ANSWER"); break; }
        int here = 0; for (int i = 0; i < ncab; i++) here += heard[i];
        char l1[112], l2[128];
        snprintf(l1, sizeof l1, "CABINET %d: WAITING FOR THE OTHERS...  %d OF %d HERE   %d", me + 1, here, ncab, left);
        snprintf(l2, sizeof l2, "%s", warn_text[0] ? warn_text : "SWITCH ALL THE CABINETS ON TOGETHER.   PRESS S TO PLAY ALONE");
        int k = 0;
        if (pump) k = pump(l1, l2); else SDL_Delay(10);
        if (k == 'Q') exit(0);
        if (k == 'S') { go_alone("S WAS PRESSED"); break; }
    }
    void eng_ui_set_hint(const char *, int);
    if (pump && state == ST_LINKED) { static char h[96]; const char *s23_party_hint(void); const char *ph = s23_party_hint();
        if (ph[0]) eng_ui_set_hint(ph, 60 * 15);                       /* a party: whose picture this is (src/s23_window.c) */
        else { snprintf(h, sizeof h, "LINKED: CABINET %d OF %d", me + 1, ncab); eng_ui_set_hint(h, 60 * 6); } }
    if (pump && state == ST_ALONE) { static char h[128]; snprintf(h, sizeof h, "NOT LINKED: %s", why_alone); eng_ui_set_hint(h, 60 * 8); }
}
static void tick(void)
{
    if (state != ST_LINKED) return;
    { extern uint8_t s23_ram[]; uint8_t *p = s23_ram + 0x1EB01C;   /* the PCB NUMBER (GAME OPTIONS) = this cabinet's number while linked */
      if (p[0] | p[1] | p[2] || p[3] != (uint8_t)(me + 1)) { if (rr_frame > 60) fprintf(stderr, "[LINK] f%u PCB NUMBER %u -> %d\n", rr_frame, p[3], me + 1);
          p[0] = p[1] = p[2] = 0; p[3] = (uint8_t)(me + 1); } }
    const uint64_t now = s23_cycles_now();
    const int polling = now >= poll_at;                        /* the others' news and the wait for them, then the chip to now (our */
    if (polling) { poll_at = now + poll_every; poll_cable(); } /* messages go as they finish), what is due in, then our time told */
    wait_for_others(now);
    chip_run(now);
    deliver(now);
    if (polling) send_time(now);
}
static void report(void)
{
    if (state == ST_OFF) return;
    if (state == ST_LINKED) for (int i = 0; i < 3; i++) { uint8_t b[16]; T->send(-1, b, hdr(b, 'B')); }   /* the others would wait 5 s for us */
    fprintf(stderr, "[LINK] link: %s; %lu packets out (%lu datagrams), %lu in (%lu datagrams), %lu datagrams lost, %lu packets late; waited %lu times, %llu ms in all, longest %llu ms\n",
            state == ST_LINKED ? "LINKED" : "alone", n_pkt_out, n_dgram_out, n_pkt_in, n_dgram_in, n_drops, n_late,
            n_waits, (unsigned long long)wait_ms_total, (unsigned long long)wait_ms_max);
    T->close();
}
/* [0] this cabinet (blue, flickers as it sends), [1] the others: 1 green (all heard), 3 red (one silent for a second, or lost), 4 grey (alone) */
static int lights(int kind[2], int lit[2])
{
    if (!lights_pos || state == ST_OFF || state == ST_WAIT) return 0;
    kind[0] = 0; lit[0] = (int)(n_dgram_out / 8) & 1;
    if (state != ST_LINKED) { kind[1] = 4; lit[1] = 1; return lights_pos; }
    const uint64_t t = ms_now();
    kind[1] = 1; lit[1] = (int)(n_dgram_in / 8) & 1;
    for (int i = 0; i < ncab; i++) if (i != me && (lost[i] || t - heard_ms[i] > 1000)) { kind[1] = 3; lit[1] = 1; }
    return lights_pos;
}
/* ONE LIGHT PER LINKED CABINET (the user: "only the number of linked cabinets"): this one outlined and blue, the others green (lit as
 * their data comes in) or red (silent for a second / lost); not linked: no lights; alone: this one only, grey */
static int lights_n(int kind[8], int lit[8], int *self, int *n)
{
    if (!lights_pos || state == ST_OFF || state == ST_WAIT) return 0;
    if (state != ST_LINKED) { *n = 1; *self = 0; kind[0] = 4; lit[0] = 1; return lights_pos; }
    const uint64_t t = ms_now();
    *n = ncab; *self = me;
    for (int i = 0; i < ncab && i < 8; i++) {
        if (i == me) { kind[i] = 0; lit[i] = (int)(n_dgram_out / 8) & 1; }
        else if (lost[i] || t - heard_ms[i] > 1000) { kind[i] = 3; lit[i] = 1; }
        else { kind[i] = 1; lit[i] = (int)(n_dgram_in / 8) & 1; }
    }
    return lights_pos;
}
__attribute__((constructor)) static void lights_hook(void) { extern int (*eng_link_lights_n)(int *, int *, int *, int *); eng_link_lights_n = lights_n; }
const s23_link_ops ff_link_ops = { "FINAL FURLONG  -  CABINET LINK", gate, tick, report, linked_hook, lights, chip_read, chip_write };

#include "eng/eng_cfg.h"
/* ---- FIND CABINETS (as the other games' Network page): every running copy answers "FFLD?" on UDP DISC_PORT with "FFLD!<PC name>" -- a small
 * thread, the link on or off; the Link page's "Find cabinets" asks the whole network and lists who answered; "Use the cabinets found" writes
 * them, sorted by address, as the address list with "This cabinet: Auto" -- every PC that does the same gets the same list, and each takes the
 * number of its own place in it (open_link) ---- */
#define DISC_PORT 23279
static int wsa_up(void) {
#ifdef _WIN32
    static int done; if (!done) { WSADATA w; done = WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 1 : -1; } return done > 0;
#else
    return 1;
#endif
}
#ifdef _WIN32
static DWORD WINAPI disc_answer(LPVOID arg)
#else
static void *disc_answer(void *arg)
#endif
{
    const sock_t s = (sock_t)(uintptr_t)arg;
    for (;;) {
        char b[64]; struct sockaddr_in from; uint32_t ifx = 0;
        const int r = lv_recv(s, b, (int)sizeof b, &from, &ifx);
        if (r < 0) { SDL_Delay(50); continue; }
        if (r >= 5 && !memcmp(b, "FFLD?", 5)) {
            char host[40] = "", o[64]; gethostname(host, sizeof host - 1);
            const int n = snprintf(o, sizeof o, "FFLD!%s", host);
            lv_send(s, o, n, &from, ifx);                       /* (back out of the connection the question came in on) */
        }
    }
    return 0;
}
void disc_start_answering_(void);
static void disc_start_answering(void)
{
    static int started; if (started) return; started = 1;
    if (!wsa_up()) return;
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0); if (s == BAD_SOCK) return;
    struct sockaddr_in me; memset(&me, 0, sizeof me); me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(INADDR_ANY); me.sin_port = htons(DISC_PORT);
    if (bind(s, (struct sockaddr *)&me, sizeof me) != 0) { closesocket(s); return; }   /* (another copy on this PC answers already) */
    lv_prepare(s);
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, disc_answer, (LPVOID)(uintptr_t)s, 0, NULL); if (h) CloseHandle(h);
#else
    pthread_t t; pthread_create(&t, NULL, disc_answer, (void *)(uintptr_t)s);
#endif
}
/* this PC's own IPv4 addresses (the loopback too) */
static int own_ips(uint32_t *ip, int max)
{
    int n = 0; if (!wsa_up()) return 0;
    ip[n++] = htonl(INADDR_LOOPBACK);
    char host[256]; if (gethostname(host, sizeof host) != 0) return n;
    struct addrinfo hints, *res = NULL, *r; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return n;
    for (r = res; r && n < max; r = r->ai_next) ip[n++] = ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return n;
}
static struct { int searching, done; uint64_t until; sock_t s; int n; struct { uint32_t ip; char name[33]; int me; } f[MAXCAB]; } D = { 0, 0, 0, BAD_SOCK, 0 };
static void disc_find(void)
{
    if (D.searching || !wsa_up()) return;
    D.s = socket(AF_INET, SOCK_DGRAM, 0); if (D.s == BAD_SOCK) return;
#ifdef _WIN32
    { u_long nb = 1; ioctlsocket(D.s, FIONBIO, &nb); BOOL yes = TRUE; setsockopt(D.s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes); }
#else
    fcntl(D.s, F_SETFL, fcntl(D.s, F_GETFL) | O_NONBLOCK); { int yes = 1; setsockopt(D.s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes); }
#endif
    lv_send_all(D.s, "FFLD?", 5, htons(DISC_PORT));           /* every connection, and this PC (whatever the network) */
    D.n = 0; D.done = 0; D.searching = 1; D.until = ms_now() + 1500;
}
static void disc_poll(void)
{
    if (!D.searching) return;
    uint32_t mine[16]; const int nm = own_ips(mine, 16);
    for (;;) {
        char b[64]; struct sockaddr_in from; socklen_t fl = sizeof from;
        const int r = (int)recvfrom(D.s, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) break;
        if (r < 5 || memcmp(b, "FFLD!", 5)) continue;
        b[r] = 0;
        uint32_t ip = from.sin_addr.s_addr; const int me = lv_is_mine(ip);
        if (me) { int had = 0; for (int k = 0; k < D.n; k++) had |= D.f[k].me;
                  if (had) continue;                           /* this PC answered already (by another of its addresses) */
                  if (nm > 1) ip = mine[1]; }                  /* this PC: its network address, not the loopback */
        int dup = 0; for (int k = 0; k < D.n; k++) if (D.f[k].ip == ip) { dup = 1; D.f[k].me |= me; }
        if (!dup && D.n < MAXCAB) { D.f[D.n].ip = ip; D.f[D.n].me = me; snprintf(D.f[D.n].name, sizeof D.f[D.n].name, "%s", b + 5); D.n++; }
    }
    if (ms_now() >= D.until) {                                 /* done: by address, so every PC sees the same order */
        closesocket(D.s); D.s = BAD_SOCK; D.searching = 0; D.done = 1;
        for (int i = 1; i < D.n; i++) for (int j = i; j > 0 && ntohl(D.f[j].ip) < ntohl(D.f[j - 1].ip); j--) { __typeof__(D.f[0]) t = D.f[j]; D.f[j] = D.f[j - 1]; D.f[j - 1] = t; }
    }
}
static void disc_use(void)
{
    if (!D.done || D.n < 2) return;
    char list[256] = ""; size_t k = 0;
    for (int i = 0; i < D.n; i++) { struct in_addr a; a.s_addr = D.f[i].ip; k += (size_t)snprintf(list + k, sizeof list - k, "%s%s", i ? "," : "", inet_ntoa(a)); }
    eng_cfg_set("link_address", list); eng_cfg_set("link_cabinet", "auto"); eng_cfg_set_int("link_cabinets", D.n); eng_cfg_set("link", "on");
}
/* "This cabinet: Auto": its place in the address list (open_link); 0 = this PC is not in it */
static int auto_place(const char *list, int *count)
{
    uint32_t mine[16]; const int nm = own_ips(mine, 16);
    char l[256]; snprintf(l, sizeof l, "%s", list); int pos = 0, i = 0;
    for (char *t = strtok(l, ", "); t; t = strtok(NULL, ", ")) {
        i++; struct in_addr a; a.s_addr = inet_addr(t);
        for (int k = 0; k < nm; k++) if (mine[k] == a.s_addr && !pos) pos = i;
    }
    *count = i;
    return pos;
}

/* ================= THE MENU'S LINK PAGE (Esc menu): the link lines of finfurl.cfg; they take effect at the next start ================= */
#include "eng/eng_ui.h"
#include "eng/eng_cfg.h"
static int pg_get(const char *k, int def) { return eng_cfg_int(k, def); }
static int cab_auto(void) { const char *c = eng_cfg_get("link_cabinet"); return c && !strcmp(c, "auto"); }
static int net_cfg(void) { const char *v = eng_cfg_get("link_net"); for (int i = 0; i < 4; i++) if (v && !strcmp(v, net_key[i])) return i; return 0; }
/* THE STANDARD NETWORK PAGE (the same rows, names and order as every other LAN game's -- engine/link_host.c): Link play, This cabinet,
 * Cabinets in set, Find others, Start-up wait, Link lights, If link is lost, Find cabinets, [Use the N found]; then this game's own:
 * Network delay. (No "Restart all cabinets": this link has none. If link is lost: it always plays on without the lost one.) */
enum { K_ON, K_CAB, K_CABS, K_FIND, K_WAIT, K_LIGHTS, K_LOST, K_DISC, K_USE, K_NET };
static int lk_kind(int r) { const int use = D.done && D.n >= 2; if (r < K_USE) return r; return use ? r : r + 1; }
static int lk_rows(void) { return (D.done && D.n >= 2) ? 10 : 9; }
static const char *const lk_light_key[7] = { "off", "bottom-left", "bottom-centre", "bottom-right", "top-left", "top-centre", "top-right" };
static const char *const lk_light_name[7] = { "Off", "Bottom left", "Bottom centre", "Bottom right", "Top left", "Top centre", "Top right" };
static int lk_light(void) { const char *v = eng_cfg_get("link_lights"); for (int i = 0; i < 7; i++) if (v && !strcmp(v, lk_light_key[i])) return i; return 1; }
static int lk_on(void) { const char *o = eng_cfg_get("link"); return o && (!strcmp(o, "on") || !strcmp(o, "1") || !strcmp(o, "yes")); }
static int lk_has_value(int r) { const int k = lk_kind(r); return k != K_LOST && k != K_DISC && k != K_USE; }
static bool lk_has_value_b(int r) { return lk_has_value(r) != 0; }
static void lk_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    const char *a = eng_cfg_get("link_address");
    *v = 0;
    switch (lk_kind(r)) {
    case K_ON: snprintf(l, ln, "Link play"); snprintf(v, vn, "%s", lk_on() ? "On" : "Off"); break;
    case K_CAB: snprintf(l, ln, "This cabinet");
        if (cab_auto()) snprintf(v, vn, "Auto (its place in the list)"); else snprintf(v, vn, "%d", pg_get("link_cabinet", 1)); break;
    case K_CABS: snprintf(l, ln, "Cabinets in set"); snprintf(v, vn, "%d", pg_get("link_cabinets", 2)); break;
    case K_FIND: snprintf(l, ln, "Find others");
        snprintf(v, vn, "%s", !a || !strcmp(a, "auto") ? "Automatic" : !strcmp(a, "loop") ? "This PC (test copies)" : "Address list"); break;
    case K_WAIT: snprintf(l, ln, "Start-up wait"); snprintf(v, vn, "%d s", pg_get("link_wait", 60)); break;
    case K_LIGHTS: snprintf(l, ln, "Link lights"); snprintf(v, vn, "%s", lk_light_name[lk_light()]); break;
    case K_LOST: snprintf(l, ln, "If link is lost"); snprintf(v, vn, "Carry on (this game always does)"); break;
    case K_DISC: snprintf(l, ln, "Find cabinets"); snprintf(v, vn, "%s", D.searching ? "Searching..." : "Enter"); break;
    case K_USE: snprintf(l, ln, "Use the %d found", D.n); snprintf(v, vn, "Enter"); break;
    default: { static const char *const nm[4] = { "LAN (cable, no added delay)", "Wi-Fi (+17 ms)", "Slow network (+50 ms)", "Internet (+100 ms)" };
               snprintf(l, ln, "Network delay"); snprintf(v, vn, "%s", nm[net_cfg()]); break; }
    }
}
static void lk_change(int r, int dir)
{
    const int d = dir < 0 ? -1 : 1;
    switch (lk_kind(r)) {
    case K_ON: eng_cfg_set("link", lk_on() ? "off" : "on"); return;
    case K_CAB: { const int n = pg_get("link_cabinets", 2); const char *a = eng_cfg_get("link_address");
                  const int list = a && strcmp(a, "auto") && strcmp(a, "loop");            /* Auto numbering only with an address list */
                  int c = cab_auto() ? 0 : pg_get("link_cabinet", 1); c += d;
                  if (c < (list ? 0 : 1)) c = n; if (c > n) c = list ? 0 : 1;
                  if (c == 0) eng_cfg_set("link_cabinet", "auto"); else eng_cfg_set_int("link_cabinet", c); return; }
    case K_CABS: { int n = pg_get("link_cabinets", 2) + d; if (n < 2) n = MAXCAB; if (n > MAXCAB) n = 2; eng_cfg_set_int("link_cabinets", n);
                   if (!cab_auto() && pg_get("link_cabinet", 1) > n) eng_cfg_set_int("link_cabinet", n); return; }
    case K_FIND: { const char *a = eng_cfg_get("link_address");                         /* Automatic <-> test copies (a found list: back to Automatic) */
                   eng_cfg_set("link_address", a && !strcmp(a, "auto") ? "loop" : "auto");
                   if (cab_auto()) eng_cfg_set_int("link_cabinet", 1); return; }
    case K_WAIT: { int w = pg_get("link_wait", 60) + d * 10; if (w < 10) w = 10; if (w > 300) w = 300; eng_cfg_set_int("link_wait", w); return; }
    case K_LIGHTS: eng_cfg_set("link_lights", lk_light_key[(lk_light() + (d < 0 ? 6 : 1)) % 7]); return;
    case K_LOST: return;
    case K_DISC: if (dir == 0) disc_find(); return;
    case K_USE: if (dir == 0) disc_use(); return;
    default: eng_cfg_set("link_net", net_key[(net_cfg() + (d < 0 ? 3 : 1)) & 3]); return;
    }
}
static void lk_notes(void (*line)(const char *, ...))
{
    disc_poll();
    if (state == ST_LINKED) {
        int up = 0; const uint64_t t = ms_now(); for (int i = 0; i < ncab; i++) if (i == me || (!lost[i] && t - heard_ms[i] < 1000)) up++;
        line("Now: LINKED - cabinet %d of %d, %d answering.", me + 1, ncab, up);
    } else if (state == ST_ALONE) line("Now: not linked - %s", why_alone);
    else line("Now: link off - this cabinet plays alone.");
    if (D.searching) line("Searching the network...");
    else if (D.done) {
        line("Found %d PC%s running Final Furlong:", D.n, D.n == 1 ? "" : "s");
        for (int i = 0; i < D.n; i++) { struct in_addr a; a.s_addr = D.f[i].ip; line("  %d. %s  %s%s", i + 1, inet_ntoa(a), D.f[i].name, D.f[i].me ? "  (this PC)" : ""); }
        if (D.n >= 2) line("Automatic needs no list: give every PC its own cabinet number.");
        else line("Start the game on the other PCs, then search again.");
    } else line("Find cabinets: start the game on every PC, then search.");
    if (state == ST_LINKED) line("Network: %s (the slowest setting of the linked PCs is used).", net_key[net_level]);
    line("Automatic: the cabinets find each other. Give every PC its own number.");
    line("Changes take effect at the next start.");
}
const eng_ui_page ff_link_page = { "Network", 560, 140, 26, lk_rows, lk_has_value_b, NULL, lk_text, lk_change, lk_notes };
void disc_start_answering_(void) { disc_start_answering(); }
int auto_place_(const char *l, int *n) { return auto_place(l, n); }
