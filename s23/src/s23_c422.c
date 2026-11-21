/*
 * s23_c422.c -- THE TWIN-CABINET LINK: Namco's C422 ("RS422 networking (twin cabinet) communication controller"), our own model, and its
 * cable carried over UDP to the other cabinet's PC. Our fork (the LAN link project): neither MAME nor the original port model the data path
 * -- MAME knows only the interrupt -- so this is read off the game's own driver (ROM 0x8000BB6C send, 0x8000BD24 receive, 0x8000BEAC the
 * interrupt handler, 0x80119700 the power-on interrupt check, 0x8008DCE8 the test menu's register display).
 *
 * THE CHIP as the driver uses it (16-bit registers at 0x06400000, 16 KB of RAM at 0x06200000 holding one byte in the low half of each word):
 *   +0  STATUS   bit 1 = a message has arrived, bit 2 = the send is done. The driver clears bit 1 by writing it 0.
 *   +2  MASK     interrupt mask, a set bit masks: the driver unmasks bits 1 / 2 to wait for them (0xFFFB in the power-on check, 0x000F
 *                masks everything). The interrupt (IP5) is up while (STATUS & ~MASK & 6) != 0.
 *   +6  CONTROL  3 while the driver sets a send up, 2 to let it go (1 then 0 in the power-on check): a send starts when bit 0 drops.
 *   +A  COUNT    bytes to send (the driver sends a message in pieces of up to 255); reads what is left, 0 = idle.
 *   +C  RX       the receive ring's write position (12 bits): the next word of RAM+0x2000 the chip fills.
 *   +E  TX       the send position (12 bits, a word of RAM+0x0000), advanced as the bytes go.
 *   +4, +8       stored and read back (the test menu shows +8).
 * A received message ends with the word's bit 8 set on its last byte (the driver searches up to 8 words back from RX for it, then reads
 * the length from the two bytes before it and expects 0x5A at the start). The chip cannot know where a message ends except that the line
 * goes quiet, so here: the sender marks the end of a burst -- a send that finishes with no new one started within two byte times.
 *
 * THE CABLE: every piece of a burst goes to the other PC as a UDP datagram (the session, a sequence number, the end-of-burst flag, the
 * bytes, and the sender's game time -- its CPU cycle count); the receiver drops duplicates and old ones and writes the rest into its ring
 * at ITS OWN game time = the sender's + LAG (about a frame and a half).
 * THE TWO GAMES KEEP IN STEP ON GAME TIME, not on the PCs' clocks (the real pair shares a clock; two PCs do not, and a PC can stall: the
 * graphics driver preparing a new scene took 0.4 - 1.1 s on the Iris Xe, a window being dragged stops its game outright -- the other game
 * ran on, saw nothing from its partner for those frames and gave the partner's player to the computer, "COM"). Each side tells the other
 * how far its game has run ("C42T", every few lines); a side that gets more than LAG ahead of its partner waits for it. A wait of more than
 * 5 s (the partner closed, crashed or unplugged; "C42B" = closed) and the link is broken: the game carries on and handles the lost partner.
 * THE SESSION and the power-on wait: below (s23_c422_gate). The settings: tc2_link.cfg beside the program (made on the first run, link=off).
 *
 *   S23_LINK=<side>:<this PC's port>:<other PC>:<its port>     (tests) side L (left / red) or R (right / blue) -- the I/O board's Link ID
 *   switch; e.g. S23_LINK=L:23001:127.0.0.1:23002 and S23_LINK=R:23002:127.0.0.1:23001 (two copies on one PC); S23_LINK_WAIT=<seconds>
 *   S23_C422LOG=1   every register access and every burst, to the log
 *   S23_LINK_LOSS=<percent>   TEST: that share of this side's datagrams is never sent (a bad cable)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesocket close
#endif
#include <time.h>
#include <SDL2/SDL.h>
#include "s23_host.h"
#include "s23_game.h"
#include "../../engine/lan_via.h"         /* out of the connection the other cabinet was heard on (a VPN on 169.254.x.x) */

uint8_t *s23_vram_ptr(uint32_t p);
uint64_t s23_cycles_now(void);
extern uint32_t rr_frame;

#define RAM_TX  0x06200000u
#define RAM_RX  0x06202000u
#define BYTE_CYCLES 1694u                 /* one byte on the line: 1 Mbit/s, 10 bits a byte, at the 169.344 MHz count -- a guess, unmeasured */
#define POLL_CYCLES 200000u               /* the socket is read about 14 times a frame */

static uint16_t reg[8];
static int logon = -1;
static int side_right;                    /* the Link ID switch (src/s23_h8.c reads it) */
static void open_link(void);
int s23_c422_link_right(void) { if (s23_game.link) return 0; open_link(); return side_right; }   /* read early by the window (src/s23_guns.c): the settings first */

/* the send in progress */
static int tx_busy;                       /* bytes are going out */
static uint64_t tx_next;                  /* the cycle the next byte has gone */
static uint64_t idle_at;                  /* a burst's end: no new send by this cycle */
static int burst_open;                    /* bytes have gone since the last end-of-burst */

/* the cable */
static sock_t sk = BAD_SOCK;
static struct sockaddr_in peer;           /* the other cabinet (learned from its hello when the address is "auto") */
static int peer_known, auto_addr, loopback;
static uint32_t tx_seq, rx_seq_last = 0xFFFFFFFFu;
static uint32_t peer_via, last_ifx;                         /* the connection the other cabinet is heard on (engine/lan_via.h) */
static uint8_t pend[1500]; static int npend;   /* this burst's bytes not yet sent */
static uint64_t poll_at;
static unsigned long n_bursts_out, n_bursts_in, n_bytes_out, n_bytes_in, n_drops;

/* in step on game time (above) */
#define FRAME_CYCLES 2822400u             /* 169.344 MHz / 60 */
static uint64_t lag = FRAME_CYCLES * 3 / 2;
static uint64_t peer_time;                /* how far the partner's game has run (its last word) */
static int peer_lost;                     /* the link broke (a wait ran out, or the partner closed): no more waiting */
static unsigned long n_waits; static uint64_t wait_ms_total, wait_ms_max;
typedef struct { uint64_t due; int n, end; uint8_t d[1500]; } rx_piece;
#define RXQ 64
static rx_piece rxq[RXQ]; static int rxq_head, rxq_n;   /* received, not yet due */

/* THE SESSION (as the real cabinets: they link when they are switched on together; one switched on later, or restarted, plays alone
 * until both are restarted -- the game itself only links two boards that start together: one started 25 s late got a half link).
 * Each start draws a random number; two cabinets waiting at power-on make their session from both numbers.
 *   hello "C42H" (every 100 ms while waiting, every 500 ms after): version, side, state, our number, our session
 *   data  "C422": the session, a sequence number, the end-of-burst flag, the bytes -- only between the two of a session */
enum { ST_OFF, ST_WAIT, ST_LINKED, ST_ALONE };
static int state = ST_OFF;
static uint32_t my_nonce, their_nonce, session;
static uint64_t hello_ms;
static char why_alone[96], warn_text[96];
int s23_c422_linked(void) { if (s23_game.link) return s23_game.link->linked ? s23_game.link->linked() : 0; return state == ST_LINKED; }
int s23_c422_state(void) { return state; }

static uint64_t ms_now(void) { return (uint64_t)SDL_GetTicks64(); }
static uint32_t mix(uint32_t left, uint32_t right)
{
    uint32_t h = left * 0x9E3779B1u ^ right;
    h ^= h >> 15; h *= 0x85EBCA77u; h ^= h >> 13;
    return h ? h : 1;
}
static uint32_t session_with(uint32_t other) { return side_right ? mix(other, my_nonce) : mix(my_nonce, other); }

static uint16_t ram_rd(uint32_t a) { const uint8_t *m = s23_vram_ptr(a); return m ? (uint16_t)(m[0] << 8 | m[1]) : 0; }
static void ram_wr(uint32_t a, uint16_t v) { uint8_t *m = s23_vram_ptr(a); if (m) { m[0] = (uint8_t)(v >> 8); m[1] = (uint8_t)v; } }

static void irq_update(void) { s23_irq_set(S23_SRC_C422, (reg[0] & ~reg[1] & 6) != 0); }

static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static void put64(uint8_t *b, uint64_t v) { put32(b, (uint32_t)(v >> 32)); put32(b + 4, (uint32_t)v); }
static uint64_t get64(const uint8_t *b) { return (uint64_t)get32(b) << 32 | get32(b + 4); }

/* the settings: tc2_link.cfg beside the program (link=on|off, side=left|right, address=auto|<the other PC>, port=23000, wait=60), or
 * S23_LINK=side:port:host:port (tests: two copies on one PC) */
static int wait_s = 60;
static void cfg_read(char *side, char *host, size_t hn, unsigned *lport, unsigned *pport, int *on)
{
    FILE *f = fopen("tc2_link.cfg", "r");
    if (!f) {                                                /* the first run: the file, with the link off, to fill in */
        f = fopen("tc2_link.cfg", "w");
        if (f) {
            fprintf(f, "# Time Crisis 2 twin-cabinet link. Set link=on to use it.\n"
                       "# side: which cabinet this PC is in -- left (the red player) or right (the blue player). The two PCs must differ.\n"
                       "# address: auto finds the other cabinet on the network by itself; or the other PC's address, e.g. 192.168.1.20\n"
                       "# port: the same number on both PCs. wait: seconds to wait at power-on for the other cabinet (then it plays alone).\n"
                       "# lights: the link lights (off, bottom-left, bottom-centre, bottom-right, top-left, top-centre, top-right)\n"
                       "# record: on keeps the last played session's inputs as tc2_last.rec (to replay a problem), off keeps none\n"
                       "link=off\nside=left\naddress=auto\nport=23000\nwait=60\nlights=bottom-left\nrecord=on\n");
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char k[64], v[192];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %191s", k, v) != 2) continue;
        if (!strcmp(k, "link")) *on = !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "yes");
        else if (!strcmp(k, "side")) *side = (v[0] == 'r' || v[0] == 'R') ? 'R' : 'L';
        else if (!strcmp(k, "address")) snprintf(host, hn, "%s", v);
        else if (!strcmp(k, "port")) *lport = *pport = (unsigned)atoi(v);
        else if (!strcmp(k, "other_port")) *pport = (unsigned)atoi(v);
        else if (!strcmp(k, "wait")) wait_s = atoi(v);
    }
    fclose(f);
}

/* THE LINK LIGHTS (as Tokyo Wars' and Rave Racer's, engine/link_host.c): lights= in tc2_link.cfg beside the program -- off, bottom-left
 * (the default), bottom-centre, bottom-right, top-left, top-centre, top-right. Read whatever else says how to link (the launchers' S23_LINK). */
static int lights_pos = 1;
static uint64_t heard_ms;                 /* the last datagram from the partner */
static void lights_read(void)
{
    static const char *const key[7] = { "off", "bottom-left", "bottom-centre", "bottom-right", "top-left", "top-centre", "top-right" };
    FILE *f = fopen(getenv("S23_CABINET_DIR") ? "../tc2_link.cfg" : "tc2_link.cfg", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char k[64], v[64];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %63s", k, v) != 2 || strcmp(k, "lights")) continue;
        lights_pos = !strcmp(v, "on") ? 1 : 0;
        for (int i = 1; i < 7; i++) if (!strcmp(v, key[i])) lights_pos = i;
    }
    fclose(f);
}
/* SESSION RECORDINGS (as the other games' link.cfg record=): record= in tc2_link.cfg -- on (the default) keeps tc2_last.rec, off none */
int s23_c422_record(void)
{
    FILE *f = fopen(getenv("S23_CABINET_DIR") ? "../tc2_link.cfg" : "tc2_link.cfg", "r");
    if (!f) return 1;
    char line[256]; int on = 1;
    while (fgets(line, sizeof line, f)) {
        char k[64], v[64];
        if (line[0] != '#' && sscanf(line, " %63[^= ] = %63s", k, v) == 2 && !strcmp(k, "record")) on = strcmp(v, "off") != 0;
    }
    fclose(f);
    return on;
}
/* where (0 = not shown) and the two lights: [0] this cabinet (blue, flickers as it sends), [1] the other one -- 1 green (messages arriving,
 * flickers with them), 3 red (nothing for a second, or the link broke), 4 grey (not linked: playing alone). lit = bright this frame. */
int s23_c422_lights(int kind[2], int lit[2])
{
    if (s23_game.link) return s23_game.link->lights ? s23_game.link->lights(kind, lit) : 0;
    if (!lights_pos || state == ST_OFF || state == ST_WAIT) return 0;
    kind[0] = 0; lit[0] = (int)(n_bursts_out / 8) & 1;
    if (state != ST_LINKED) { kind[1] = 4; lit[1] = 1; }
    else if (peer_lost || ms_now() - heard_ms > 1000) { kind[1] = 3; lit[1] = 1; }
    else { kind[1] = 1; lit[1] = (int)(n_bursts_in / 8) & 1; }
    return lights_pos;
}

static void open_link(void)
{
    static int done; if (done) return; done = 1;
    if (s23_game.link || !s23_game.twin) { logon = 0; return; }   /* its own link (s23_game.link), or a one-cabinet game: no C422 cable, no tc2_link.cfg */
    lights_read();
    logon = getenv("S23_C422LOG") != NULL;
    char side = 'L', host[256] = "auto"; unsigned lport = 23000, pport = 23000; int on = 0;
    const char *e = getenv("S23_LINK");
    if (e && *e) {
        if (sscanf(e, "%c:%u:%255[^:]:%u", &side, &lport, host, &pport) != 4) { fprintf(stderr, "[C422] S23_LINK=%s: expected side:port:host:port\n", e); return; }
        on = 1;
    } else cfg_read(&side, host, sizeof host, &lport, &pport, &on);
    { const char *sd = getenv("S23_SIDE"); if (sd && *sd) side = (sd[0] == 'r' || sd[0] == 'R') ? 'R' : 'L'; }   /* the launchers: LEFT / RIGHT cabinet */
    side_right = side == 'R' || side == 'r';
    if (getenv("S23_LINK_WAIT")) wait_s = atoi(getenv("S23_LINK_WAIT"));
    if (getenv("S23_LINK_LAG")) lag = (uint64_t)(atof(getenv("S23_LINK_LAG")) * FRAME_CYCLES);   /* TEST: in frames */
    if (!on) { fprintf(stderr, "[C422] link off (tc2_link.cfg): this cabinet plays alone\n"); return; }
#ifdef _WIN32
    { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
#endif
    sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk == BAD_SOCK) { fprintf(stderr, "[C422] no socket\n"); return; }
    auto_addr = !strcmp(host, "auto");
    loopback = !strcmp(host, "127.0.0.1") || !strcmp(host, "localhost");   /* both copies on this PC: this PC only (no firewall question) */
    struct sockaddr_in me; memset(&me, 0, sizeof me);
    me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY); me.sin_port = htons((uint16_t)lport);
    if (bind(sk, (struct sockaddr *)&me, sizeof me) != 0) {
        fprintf(stderr, "[C422] cannot use port %u (is the game already running?): this cabinet plays alone\n", lport);
        snprintf(why_alone, sizeof why_alone, "PORT %u IS IN USE - IS THE GAME ALREADY RUNNING?", lport);
        closesocket(sk); sk = BAD_SOCK; state = ST_ALONE; return;
    }
#ifdef _WIN32
    { u_long nb = 1; ioctlsocket(sk, FIONBIO, &nb); }
    { BOOL yes = TRUE; setsockopt(sk, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes); }
    lv_prepare(sk); peer_via = 0;
#else
    fcntl(sk, F_SETFL, fcntl(sk, F_GETFL) | O_NONBLOCK);
    { int yes = 1; setsockopt(sk, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes); }
#endif
    memset(&peer, 0, sizeof peer); peer.sin_family = AF_INET; peer.sin_port = htons((uint16_t)pport);
    if (!auto_addr) {
        struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) { fprintf(stderr, "[C422] cannot find %s: looking on the whole network instead\n", host); auto_addr = 1; }
        else { peer.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr; peer_known = 1; freeaddrinfo(res); }
    }
    my_nonce = (uint32_t)SDL_GetPerformanceCounter() ^ (uint32_t)time(NULL) * 2654435761u ^ (uint32_t)(uintptr_t)&me;
    if (!my_nonce) my_nonce = 1;
    state = ST_WAIT;
    fprintf(stderr, "[C422] link on: this cabinet is %s (Link ID), port %u, the other cabinet %s:%u\n", side_right ? "RIGHT / blue" : "LEFT / red",
            lport, auto_addr ? "found on the network" : host, pport);
}

/* ONE GUN A SCREEN (src/s23_guns.c): the device this game took, told to the other game on this PC ("C42G", with every hello) */
static char my_claim[256];
static void send_claim(void)
{
    if (sk == BAD_SOCK || state != ST_LINKED || !my_claim[0]) return;
    uint8_t b[8 + sizeof my_claim]; memcpy(b, "C42G", 4); put32(b + 4, session);
    const size_t n = strlen(my_claim) + 1; memcpy(b + 8, my_claim, n);
    lv_send(sk, b, (int)((int)(8 + n)), &peer, peer_via);
}
void s23_c422_send_claim(const char *name) { snprintf(my_claim, sizeof my_claim, "%s", name); send_claim(); }

static void send_hello(void)
{
    if (sk == BAD_SOCK) return;
    send_claim();
    uint8_t b[16]; memcpy(b, "C42H", 4);
    b[4] = 2; b[5] = (uint8_t)side_right;                    /* 2: data stamped with game time (1 = the first kit; the two never link) */ b[6] = (uint8_t)state; b[7] = 0;
    put32(b + 8, my_nonce); put32(b + 12, session);
    if (peer_known) lv_send(sk, b, (int)(sizeof b), &peer, peer_via);
    if (auto_addr && state != ST_LINKED) {                   /* not found yet: to every PC on the network */
        lv_send_all(sk, b, (int)sizeof b, peer.sin_port);    /* (out of every connection, and this PC) */
    }
}

static void go_linked(uint32_t other, const struct sockaddr_in *from)
{
    their_nonce = other; session = session_with(other); state = ST_LINKED;
    if (auto_addr || !peer_known) { peer.sin_addr = from->sin_addr; peer_known = 1; }
    peer_via = last_ifx;                                     /* replies: out of the connection it came in on */
    fprintf(stderr, "[C422] LINKED with the %s cabinet at %s (session %08X)\n", side_right ? "left" : "right", inet_ntoa(from->sin_addr), session);
    for (int i = 0; i < 3; i++) send_hello();
}
static void go_alone(const char *why)
{
    if (state == ST_ALONE) return;
    state = ST_ALONE; snprintf(why_alone, sizeof why_alone, "%s", why);
    fprintf(stderr, "[C422] NOT linked: %s -- this cabinet plays alone (restart both cabinets to link them)\n", why);
}

static void on_hello(const uint8_t *b, int r, const struct sockaddr_in *from)
{
    if (r < 16) return;
    if (b[4] != 2) {
        static int said;
        snprintf(warn_text, sizeof warn_text, "THE OTHER PC HAS A DIFFERENT VERSION OF THE GAME - COPY THE SAME KIT TO BOTH");
        if (!said) { said = 1; fprintf(stderr, "[C422] WARNING: the PC at %s runs another version of the link (%d, this is 2)\n", inet_ntoa(from->sin_addr), b[4]); }
        return;
    }
    const int their_side = b[5] & 1, their_state = b[6];
    const uint32_t nonce = get32(b + 8), their_session = get32(b + 12);
    if (nonce == my_nonce) return;                           /* our own broadcast */
    if (their_side == side_right) {                          /* both cabinets set to the same side */
        static int said;
        snprintf(warn_text, sizeof warn_text, "THE OTHER PC IS ALSO SET TO %s - SET ONE OF THEM TO %s", side_right ? "RIGHT" : "LEFT", side_right ? "LEFT" : "RIGHT");
        if (!said) { said = 1; fprintf(stderr, "[C422] WARNING: a PC at %s is also set to side=%s in tc2_link.cfg\n", inet_ntoa(from->sin_addr), side_right ? "right" : "left"); }
        return;
    }
    if (!auto_addr && peer_known && !loopback && from->sin_addr.s_addr != peer.sin_addr.s_addr) return;   /* not the address we were given */
    if (state == ST_WAIT) {
        if (their_state == ST_WAIT) go_linked(nonce, from);
        else if (their_state == ST_LINKED && their_session == session_with(nonce)) go_linked(nonce, from);   /* it has just decided: with us */
        else go_alone(their_state == ST_LINKED ? "THE OTHER CABINET WAS ALREADY PLAYING" : "THE OTHER CABINET STARTED WITHOUT THIS ONE");
    } else if (state == ST_LINKED && nonce != their_nonce) {
        static uint32_t told;
        if (told != nonce) { told = nonce; fprintf(stderr, "[C422] the other cabinet restarted: it plays alone until both are restarted\n"); }
    }
}

/* a piece of a burst onto the cable (end = the line goes quiet after it) */
static void flush(int end)
{
    if (sk == BAD_SOCK || state != ST_LINKED) { npend = 0; return; }
    if (!npend && !end) return;
    { static int loss = -1; if (loss < 0) { const char *e = getenv("S23_LINK_LOSS"); loss = e ? atoi(e) : 0; }
      if (loss && rand() % 100 < loss) { tx_seq++; npend = 0; return; } }   /* TEST: lost on the cable */
    uint8_t b[24 + sizeof pend];
    memcpy(b, "C422", 4);
    put32(b + 4, session); put32(b + 8, tx_seq++);
    b[12] = (uint8_t)end; b[13] = 0; b[14] = (uint8_t)(npend >> 8); b[15] = (uint8_t)npend;
    put64(b + 16, s23_cycles_now());                         /* this game's time: due at the other end LAG later */
    memcpy(b + 24, pend, (size_t)npend);
    lv_send(sk, b, (int)(24 + npend), &peer, peer_via);
    if (end) n_bursts_out++;
    n_bytes_out += (unsigned long)npend;
    npend = 0;
}

/* bytes from the cable into the receive ring */
static void receive(const uint8_t *d, int n, int end)
{
    unsigned rx = reg[6] & 0xFFF;
    for (int i = 0; i < n; i++) {
        const int last = end && i == n - 1;
        ram_wr(RAM_RX + 2 * rx, (uint16_t)(d[i] | (last ? 0x100 : 0)));
        rx = (rx + 1) & 0xFFF;
    }
    if (end && n == 0) {                                     /* an end with no bytes: mark the last byte written */
        const unsigned l = (rx - 1) & 0xFFF; ram_wr(RAM_RX + 2 * l, (uint16_t)(ram_rd(RAM_RX + 2 * l) | 0x100));
    }
    reg[6] = (uint16_t)((reg[6] & ~0xFFFu) | rx);
    n_bytes_in += (unsigned long)n;
    if (end) { n_bursts_in++; reg[0] |= 2; irq_update(); }
}

static void poll_cable(void)
{
    if (sk == BAD_SOCK) return;
    const uint64_t t = ms_now();
    if (t - hello_ms >= (state == ST_WAIT ? 100u : 500u)) { hello_ms = t; send_hello(); }
    for (int k = 0; k < 64; k++) {
        uint8_t b[2048]; struct sockaddr_in from; uint32_t ifx = 0;
        const int r = lv_recv(sk, b, (int)sizeof b, &from, &ifx);
        if (r < 0) return;
        last_ifx = ifx; if (peer_known && from.sin_addr.s_addr == peer.sin_addr.s_addr) peer_via = ifx;
        if (r < 16) continue;
        if (!memcmp(b, "C42H", 4)) { on_hello(b, r, &from); continue; }
        if (state != ST_LINKED || get32(b + 4) != session) continue;                      /* not from our session */
        heard_ms = ms_now();
        if (!memcmp(b, "C42G", 4)) { void s23_guns_other_claimed(const char *); b[r < (int)sizeof b ? r : (int)sizeof b - 1] = 0; s23_guns_other_claimed((const char *)b + 8); continue; }
        if (!memcmp(b, "C42T", 4)) { const uint64_t t = get64(b + 8); if (t > peer_time) peer_time = t; continue; }
        if (!memcmp(b, "C42B", 4)) {
            if (!peer_lost) fprintf(stderr, "[C422] the other cabinet was closed (frame %u): the link is broken\n", rr_frame);
            peer_lost = 1; continue;
        }
        if (memcmp(b, "C422", 4) || r < 24) continue;
        const uint32_t seq = get32(b + 8);
        const int end = b[12] & 1, n = b[14] << 8 | b[15];
        if (24 + n > r || n > (int)sizeof rxq[0].d) continue;
        const uint64_t ts = get64(b + 16);
        if (ts > peer_time) peer_time = ts;
        if (rx_seq_last != 0xFFFFFFFFu && (int32_t)(seq - rx_seq_last) <= 0) continue;     /* a duplicate or a late one */
        if (rx_seq_last != 0xFFFFFFFFu && seq != rx_seq_last + 1) n_drops += seq - rx_seq_last - 1;
        rx_seq_last = seq;
        if (rxq_n == RXQ) {                                  /* full (cannot happen in step): the oldest goes in now */
            rx_piece *o = &rxq[rxq_head]; receive(o->d, o->n, o->end); rxq_head = (rxq_head + 1) % RXQ; rxq_n--;
        }
        rx_piece *q = &rxq[(rxq_head + rxq_n++) % RXQ];
        q->due = ts + lag; q->n = n; q->end = end; memcpy(q->d, b + 24, (size_t)n);
        if (logon) fprintf(stderr, "[C422] f%u in  %3d bytes%s (sent at %llu, due %llu)\n", rr_frame, n, end ? " END" : "", (unsigned long long)ts, (unsigned long long)q->due);
    }
}

/* our game's time to the other side (it may run up to LAG past it) */
static void send_time(uint64_t now)
{
    if (sk == BAD_SOCK || state != ST_LINKED || peer_lost) return;
    uint8_t b[16]; memcpy(b, "C42T", 4); put32(b + 4, session); put64(b + 8, now);
    lv_send(sk, b, (int)(sizeof b), &peer, peer_via);
}

/* the received pieces whose time has come, into the ring */
static void deliver(uint64_t now)
{
    while (rxq_n && rxq[rxq_head].due <= now) {
        rx_piece *p = &rxq[rxq_head];
        receive(p->d, p->n, p->end);
        if (logon) { fprintf(stderr, "[C422] f%u ring %3d bytes%s (rx at %03X):", rr_frame, p->n, p->end ? " END" : "", reg[6] & 0xFFF);
                     for (int i = 0; i < p->n; i++) fprintf(stderr, " %02X", p->d[i]); fprintf(stderr, "\n"); }
        rxq_head = (rxq_head + 1) % RXQ; rxq_n--;
    }
}

/* AHEAD OF THE PARTNER by more than LAG: wait for it (its game stalled, or its PC runs a little slow). The window is kept alive meanwhile. */
static void wait_for_peer(uint64_t now)
{
    if (state != ST_LINKED || peer_lost || sk == BAD_SOCK || now <= peer_time + lag) return;
    const uint64_t t0 = ms_now(); uint64_t told = t0;
    send_time(now);
    while (now > peer_time + lag && !peer_lost) {
        fd_set fs; FD_ZERO(&fs); FD_SET(sk, &fs);
        struct timeval tv = { 0, 2000 };
        select((int)sk + 1, &fs, NULL, NULL, &tv);
        poll_cable();
        const uint64_t t = ms_now();
        if (t - told >= 20) { told = t; send_time(now); if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_PumpEvents(); }
        if (t - t0 >= 5000) {
            peer_lost = 1;
            fprintf(stderr, "[C422] the other cabinet has not answered for 5 s (frame %u): the link is broken, this cabinet plays on\n", rr_frame);
        }
    }
    const uint64_t w = ms_now() - t0;
    n_waits++; wait_ms_total += w; if (w > wait_ms_max) wait_ms_max = w;
    if (w >= 100) fprintf(stderr, "[C422] waited %llu ms for the other cabinet (frame %u)\n", (unsigned long long)w, rr_frame);
}

/* POWER-ON: wait for the other cabinet (up to wait= seconds), as the real pair links only when both are switched on. pump (the window, or
 * NULL headless) shows the two lines and returns 'S' to stop waiting (play alone), 'Q' to quit, 0 otherwise. */
void s23_c422_gate(int (*pump)(const char *l1, const char *l2))
{
    if (s23_game.link && s23_game.link->gate) s23_game.link->gate(pump);   /* the game's own link (the C422 chip still runs, cable-less) */
    open_link();
    if (state != ST_WAIT) return;
    const uint64_t t0 = ms_now();
    while (state == ST_WAIT) {
        poll_cable();
        const int left = wait_s - (int)((ms_now() - t0) / 1000);
        if (left <= 0) { go_alone("THE OTHER CABINET DID NOT ANSWER"); break; }
        char l1[96], l2[128];
        snprintf(l1, sizeof l1, "WAITING FOR THE %s CABINET...  %d", side_right ? "LEFT (RED)" : "RIGHT (BLUE)", left);
        snprintf(l2, sizeof l2, "%s", warn_text[0] ? warn_text : "SWITCH BOTH CABINETS ON TOGETHER.   PRESS S OR START TO PLAY ALONE");
        int k = 0;
        if (pump) k = pump(l1, l2); else SDL_Delay(10);
        if (k == 'Q') exit(0);
        if (k == 'S') { go_alone("S WAS PRESSED"); break; }
    }
    void eng_ui_set_hint(const char *, int);
    if (pump && state == ST_LINKED) { const char *s23_party_hint(void); const char *ph = s23_party_hint();   /* (a party: whose picture this is) */
        if (ph[0]) eng_ui_set_hint(ph, 60 * 15);
        else eng_ui_set_hint(side_right ? "LINKED WITH THE LEFT (RED) CABINET" : "LINKED WITH THE RIGHT (BLUE) CABINET", 60 * 6); }
    if (pump && state == ST_ALONE) { static char h[128]; snprintf(h, sizeof h, "NOT LINKED: %s", why_alone); eng_ui_set_hint(h, 60 * 8); }
}

static void tx_start(uint64_t now)
{
    if ((reg[5] & 0xFF) == 0) return;
    tx_busy = 1; tx_next = now + BYTE_CYCLES; idle_at = 0;
    if (logon) fprintf(stderr, "[C422] f%u send %u bytes from %03X\n", rr_frame, reg[5] & 0xFF, reg[7] & 0xFFF);
}

/* time passed (src/s23_main.c s23_host_step): bytes go out, a quiet line ends the burst, the cable is read */
void s23_c422_tick(void)
{
    if (s23_game.link && s23_game.link->tick) s23_game.link->tick();
    open_link();
    { static int lf = -2, lt; if (lf == -2) { const char *e = getenv("S23_C422LOG_AT"); lf = -1; if (e) { lf = 0; sscanf(e, "%d-%d", &lf, &lt); } }   /* TEST: from-to frames */
      if (lf >= 0) logon = (int)rr_frame >= lf && (int)rr_frame <= lt; }
    { static int at = -2; static double lg; if (at == -2) { const char *e = getenv("S23_LINK_LAG2"); at = -1; if (e) sscanf(e, "%d:%lf", &at, &lg); }   /* TEST: frame:lag */
      if (at >= 0 && (int)rr_frame >= at) { lag = (uint64_t)(lg * FRAME_CYCLES); at = -1; fprintf(stderr, "[C422] TEST: lag now %.2f frames\n", lg); } }
    const uint64_t now = s23_cycles_now();
    while (tx_busy && now >= tx_next) {
        const unsigned tx = reg[7] & 0xFFF;
        if (npend < (int)sizeof pend) pend[npend++] = (uint8_t)ram_rd(RAM_TX + 2 * tx);
        reg[7] = (uint16_t)((reg[7] & ~0xFFFu) | ((tx + 1) & 0xFFF));
        burst_open = 1;
        reg[5] = (uint16_t)((reg[5] & 0xFF00u) | ((reg[5] - 1) & 0xFF));
        if ((reg[5] & 0xFF) == 0) {
            tx_busy = 0; reg[0] |= 4; idle_at = tx_next + 2 * BYTE_CYCLES;
            flush(0);                                        /* this piece now; the end mark follows when the line stays quiet */
            irq_update();
        } else tx_next += BYTE_CYCLES;
    }
    if (!tx_busy && burst_open && idle_at && now >= idle_at) {
        burst_open = 0; idle_at = 0; flush(1);
        if (logon) fprintf(stderr, "[C422] f%u burst end\n", rr_frame);
    }
    if (now >= poll_at) { poll_at = now + POLL_CYCLES; poll_cable(); send_time(now); }
    { static int at = -1, ms; if (at < 0) { at = 0; const char *e = getenv("S23_LINK_STALL"); if (e) sscanf(e, "%d:%d", &at, &ms); }   /* TEST: frame:ms, */
      if (at > 0 && (int)rr_frame >= at) { at = 0; fprintf(stderr, "[C422] TEST: this PC stalls %d ms\n", ms); SDL_Delay((Uint32)ms); } }   /* a PC that stalls once */
    wait_for_peer(now);
    deliver(now);
}

int s23_c422_read(uint32_t p, uint32_t *v)
{
    open_link();
    const unsigned k = (p - 0x06400000u) >> 1 & 7;
    *v = reg[k];
    if (logon && k != 0) fprintf(stderr, "[C422] f%u read  +%X = %04X\n", rr_frame, k * 2, reg[k]);
    return 1;
}

int s23_c422_write(uint32_t p, uint32_t v)
{
    open_link();
    const unsigned k = (p - 0x06400000u) >> 1 & 7;
    const uint16_t old = reg[k];
    if (logon) fprintf(stderr, "[C422] f%u write +%X = %04X\n", rr_frame, k * 2, (unsigned)(uint16_t)v);
    switch (k) {
    case 0: reg[0] = (uint16_t)((reg[0] & v & 6u) | (v & ~6u)); break;     /* bits 1 / 2: written 0 clears them, never sets */
    case 3: reg[3] = (uint16_t)v; if ((old & 1) && !(v & 1)) tx_start(s23_cycles_now()); break;
    case 5: reg[5] = (uint16_t)v; reg[0] &= (uint16_t)~4u;                   /* a new count: the last send's "done" is old news */
            if (!(reg[3] & 1)) tx_start(s23_cycles_now()); break;
    default: reg[k] = (uint16_t)v; break;
    }
    irq_update();
    return 1;
}

void s23_c422_load_reg(unsigned k, uint16_t v) { if (k < 8) reg[k] = v; }

void s23_c422_report(void)
{
    if (s23_game.link && s23_game.link->report) s23_game.link->report();
    if (state == ST_OFF) return;
    if (sk != BAD_SOCK && state == ST_LINKED && !peer_lost)  /* closing: tell the other cabinet now (it would wait 5 s for us) */
        for (int i = 0; i < 3; i++) { uint8_t b[16]; memcpy(b, "C42B", 4); put32(b + 4, session); memset(b + 8, 0, 8); lv_send(sk, b, (int)(sizeof b), &peer, peer_via); }
    fprintf(stderr, "[C422] link: %s%s; %lu bursts / %lu bytes out, %lu bursts / %lu bytes in, %lu datagrams lost; waited for the other %lu times, %llu ms in all, longest %llu ms\n",
            state == ST_LINKED ? "LINKED" : "alone", peer_lost ? " (then broken)" : "", n_bursts_out, n_bytes_out, n_bursts_in, n_bytes_in, n_drops,
            n_waits, (unsigned long long)wait_ms_total, (unsigned long long)wait_ms_max);
}

/* ================= THE NETWORK PAGE (Esc menu; Time Crisis II): tc2_link.cfg, with the same rows, names and order as every other LAN
 * game's Network page (engine/link_host.c): Link play, This cabinet, Cabinets in set, Find others, Start-up wait, Link lights, If link is
 * lost. The changes are used at the next start. (Two cabinets always; the other side is always found by itself unless an address is typed
 * in the file; a lost link always plays on, the partner's player taken by the computer.) ================= */
#include "eng/eng_ui.h"
#define PG_FILE "tc2_link.cfg"
static char pg_v[8][64];                                          /* link, side, address, wait, lights: read when the page is drawn */
static const char *const pg_key[5] = { "link", "side", "address", "wait", "lights" };
static uint32_t pg_read_at;
static void pg_read(void)
{
    if (pg_read_at && SDL_GetTicks() - pg_read_at < 500) return;
    pg_read_at = SDL_GetTicks();
    snprintf(pg_v[0], 64, "off"); snprintf(pg_v[1], 64, "left"); snprintf(pg_v[2], 64, "auto"); snprintf(pg_v[3], 64, "60"); snprintf(pg_v[4], 64, "bottom-left");
    FILE *f = fopen(PG_FILE, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) { char k[64], v[192];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %191s", k, v) != 2) continue;
        for (int i = 0; i < 5; i++) if (!strcmp(k, pg_key[i])) snprintf(pg_v[i], 64, "%.63s", v); }
    fclose(f);
}
static void pg_set(int i, const char *v)                          /* that one line changed, the rest of the file kept as it is */
{
    char *buf = NULL; size_t n = 0, cap = 0; int done = 0;
    FILE *f = fopen(PG_FILE, "r");
    if (f) { char line[256];
        while (fgets(line, sizeof line, f)) { char k[64]; char out[300];
            if (line[0] != '#' && sscanf(line, " %63[^= ]", k) == 1 && !strcmp(k, pg_key[i])) { snprintf(out, sizeof out, "%s=%s\n", pg_key[i], v); done = 1; }
            else snprintf(out, sizeof out, "%s", line);
            const size_t l = strlen(out); if (n + l + 1 > cap) { cap = (n + l + 1) * 2; buf = realloc(buf, cap); if (!buf) { fclose(f); return; } }
            memcpy(buf + n, out, l); n += l; }
        fclose(f); }
    f = fopen(PG_FILE, "w"); if (!f) { free(buf); return; }
    if (n) fwrite(buf, 1, n, f);
    if (!done) fprintf(f, "%s=%s\n", pg_key[i], v);
    fclose(f); free(buf);
    pg_read_at = 0;
}
static const char *const pg_light_key[7] = { "off", "bottom-left", "bottom-centre", "bottom-right", "top-left", "top-centre", "top-right" };
static const char *const pg_light_name[7] = { "Off", "Bottom left", "Bottom centre", "Bottom right", "Top left", "Top centre", "Top right" };
static int pg_light(void) { for (int i = 0; i < 7; i++) if (!strcmp(pg_v[4], pg_light_key[i])) return i; return !strcmp(pg_v[4], "on") ? 1 : 0; }
static int pg_on(void) { return !strcmp(pg_v[0], "on") || !strcmp(pg_v[0], "1") || !strcmp(pg_v[0], "yes"); }
static int pg_right(void) { return pg_v[1][0] == 'r' || pg_v[1][0] == 'R'; }
static int pg_rows(void) { return 7; }
static bool pg_has_value(int r) { return r != 2 && r != 6; }
static void pg_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    pg_read(); *v = 0;
    switch (r) {
    case 0: snprintf(l, ln, "Link play"); snprintf(v, vn, "%s", pg_on() ? "On" : "Off"); break;
    case 1: snprintf(l, ln, "This cabinet"); snprintf(v, vn, "%s", pg_right() ? "2 (right, blue player)" : "1 (left, red player)"); break;
    case 2: snprintf(l, ln, "Cabinets in set"); snprintf(v, vn, "2 (this game)"); break;
    case 3: snprintf(l, ln, "Find others"); if (!strcmp(pg_v[2], "auto")) snprintf(v, vn, "Automatic"); else snprintf(v, vn, "Address: %.40s", pg_v[2]); break;
    case 4: snprintf(l, ln, "Start-up wait"); snprintf(v, vn, "%d s", atoi(pg_v[3])); break;
    case 5: snprintf(l, ln, "Link lights"); snprintf(v, vn, "%s", pg_light_name[pg_light()]); break;
    default: snprintf(l, ln, "If link is lost"); snprintf(v, vn, "Carry on (this game always does)"); break;
    }
}
static void pg_change(int r, int dir)
{
    pg_read(); const int d = dir < 0 ? -1 : 1; char b[16];
    switch (r) {
    case 0: pg_set(0, pg_on() ? "off" : "on"); break;
    case 1: pg_set(1, pg_right() ? "left" : "right"); break;
    case 3: pg_set(2, "auto"); break;                              /* (a typed address: back to Automatic) */
    case 4: { int w = atoi(pg_v[3]) + d * 10; if (w < 10) w = 10; if (w > 300) w = 300; snprintf(b, sizeof b, "%d", w); pg_set(3, b); break; }
    case 5: pg_set(4, pg_light_key[(pg_light() + (d < 0 ? 6 : 1)) % 7]); break;
    }
}
static void pg_notes(void (*line)(const char *, ...))
{
    if (state == ST_LINKED) line("Now: LINKED with the %s cabinet%s.", side_right ? "left (red)" : "right (blue)", peer_lost ? " (then lost: playing on)" : "");
    else if (state == ST_ALONE) line("Now: not linked - %s", why_alone[0] ? why_alone : "the other cabinet did not answer");
    else line("Now: link off - this cabinet plays alone.");
    line("Automatic: the two cabinets find each other on the network.");
    line("One PC is cabinet 1 (left), the other cabinet 2 (right).");
    line("Changes take effect at the next start.");
}
const eng_ui_page s23_c422_page = { "Network", 560, 140, 26, pg_rows, pg_has_value, NULL, pg_text, pg_change, pg_notes };
