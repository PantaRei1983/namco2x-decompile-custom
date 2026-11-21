/* link_net.c -- see link_net.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif
#include "link_net.h"
#include "lan_via.h"                      /* out of the connection each cabinet was heard on (a VPN on 169.254.x.x) */

#define MAGIC      0x4E32324Cu              /* "N22L" */
#define PROTO      2
#define T_FRAME    1
#define T_HELLO    2                        /* + state 1, members 1, game ID + 1 (0 = unknown) 1, hears 1, age/waited ms 4, set size 1, pad 3,
                                             *   list hash 4, [then] time sent 4, the last time heard from the receiver 4, held since 4,
                                             *   build fingerprint 4, build date 4 (older builds send the first 16 or 8 bytes only) */
#define HELLO_LEN  36
#define QWIN_MS    2000                     /* the quality figures: over this window */
#define T_BYE      3                        /* this cabinet is closing: step over it at once */
#define T_RESTART  4                        /* + address-list hash 4: every cabinet of this set restarts (Network page "Restart all") */
#define HELLO_MS   25                       /* a hello to every cabinet this often ... */
#define ALIVE_MS   120                      /* ... and one not heard for this long is off: the game gives up after 8 frames (133 ms) */
#define JOIN_MS    3000                     /* a session this young is one this cabinet was gathered for (it only missed the start) */
#define HEAR_MS    1000                     /* the diagnostics' "hears": a packet within the last second */
#define HDR        20                       /* magic 4, proto 1, type 1, src 1, dst 1, seq 4, tag 4, session 4 */

typedef struct {
    uint64_t heard;                         /* SDL ticks of its last packet (0 = never / gone) */
    int      state;                         /* what its last hello said */
    uint8_t  members;
    uint32_t session, ms;                   /* its session and that session's age, or (gathering) how long it has waited */
    int      game_id;                       /* its game's own cabinet ID, -1 = unknown */
    struct sockaddr_in from;                /* where its last packet came from (the diagnostics: a PC set up as the wrong cabinet, two
                                             * PCs as one cabinet, a list with a wrong address) */
    uint64_t ever;                          /* its first packet (0 = never heard since this start) */
    uint32_t pkts;
    uint8_t  hears;                         /* who IT hears, from its hellos (bit n = cabinet n+1): a one-way path shows here */
    uint8_t  nodes;                         /* its "Cabinets in the set" (0 = not told: an older build) */
    uint32_t list_hash;                     /* a fingerprint of its address list */
    uint32_t build_crc, build_time;         /* its program (0 = not told) */
    uint32_t echo_t; uint64_t echo_at;      /* its last "time sent", and when it came: sent back in the next hello (the round trip) */
    uint32_t q_n, q_sum, q_max, q_recv, q_lost;           /* this window: round-trip samples, messages received / missing */
    int      rtt_avg, rtt_max, loss_pct;    /* the last complete window's (-1 = none) */
    bool     warned_from;
    uint32_t vf; uint64_t vf_at;            /* our fork: the video frames it had run (+1; 0 = not told) when its last hello came, and when */
} peer_t;

static struct {
    bool on;
    lnk_cfg cfg;
    sock_t s;
    struct sockaddr_in addr[LNK_MAX_NODES];
    uint32_t tag;
    uint32_t list_hash;                     /* this PC's set-up, told to the others: the set size and the address list must match */
    int      state;
    uint32_t session;                       /* 0 while gathering */
    uint8_t  members;
    int      game_id;                       /* this game's own ID, -1 = unknown */
    uint64_t t_open, t_session;
    peer_t   p[LNK_MAX_NODES];
    uint32_t seq_to[LNK_MAX_NODES], last_seq[LNK_MAX_NODES];   /* numbered per destination: a gap = packets lost on that path */
    uint64_t next_hello, q_t0;
    int      restart_from;                  /* a cabinet asked the whole set to restart (its number, 0 = none) */
    lnk_stats st;
    uint32_t rng;
    struct { uint64_t due; int dst, len; uint8_t b[HDR + 2 * LNK_MAX_WORDS]; } held[256];   /* test delay */
    int nheld;
    bool warned_tag;
    uint32_t my_vf;                         /* this cabinet's video frames run (+1), told in the hellos (lnk_set_frame) */
    bool     find[LNK_MAX_NODES];           /* AUTOMATIC FINDING: the address list said "*" for this cabinet -- its address is learnt
                                             * from its packets; until the first one, packets for it go to every PC on the network */
    bool     found[LNK_MAX_NODES];
    uint64_t found_heard[LNK_MAX_NODES];    /* the last packet from the learnt address: another address only takes over after 1 s of
                                             * silence (a broadcast arrives by several ways: the network, and 127.0.0.1 on the same PC) */
    uint32_t via[LNK_MAX_NODES];            /* the connection (Windows' interface index) each cabinet was last heard on: packets to it go
                                             * back out of that one (0 = not known: wherever Windows chooses) -- engine/lan_via.h */
    struct in_addr mine[8]; int nmine;      /* this PC's addresses: a packet "from this cabinet" sent by another PC = two PCs, one number */
    uint32_t twin_from;                     /* that other PC's address (IPv4, host order; 0 = none seen) */
} L;

static uint64_t now_ms(void) { return SDL_GetTicks64(); }
static uint32_t rnd(void) { L.rng ^= L.rng << 13; L.rng ^= L.rng >> 17; L.rng ^= L.rng << 5; return L.rng; }

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool set_addr(int n, const char *host)
{
    memset(&L.addr[n], 0, sizeof L.addr[n]);
    L.addr[n].sin_family = AF_INET;
    L.addr[n].sin_port = htons((uint16_t)(L.cfg.base_port + n));
    L.find[n] = !strcmp(host, "*"); L.found[n] = false;
    if (L.find[n]) { L.addr[n].sin_addr.s_addr = htonl(INADDR_BROADCAST); return true; }   /* (the list's fingerprint: the same on every PC) */
    if (inet_pton(AF_INET, host, &L.addr[n].sin_addr) != 1) { fprintf(stderr, "[LINK] cabinet %d: '%s' is not an IPv4 address\n", n + 1, host); return false; }
    return true;
}

bool lnk_open(const lnk_cfg *cfg)
{
    L.cfg = *cfg;
    if (L.cfg.nodes < 2 || L.cfg.nodes > LNK_MAX_NODES || L.cfg.id < 0 || L.cfg.id >= L.cfg.nodes) {
        fprintf(stderr, "[LINK] cabinet %d of %d: out of range\n", L.cfg.id + 1, L.cfg.nodes); return false;
    }
    L.tag = 0;
    for (int i = 0; i < 4 && cfg->tag[i]; i++) L.tag |= (uint32_t)(uint8_t)cfg->tag[i] << (24 - 8 * i);
#ifdef _WIN32
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) { fprintf(stderr, "[LINK] WSAStartup failed\n"); return false; }
#endif
    /* the addresses: --link-peers "a,b,c,d" (a missing or empty entry = 127.0.0.1) */
    char list[512]; snprintf(list, sizeof list, "%s", cfg->peers ? cfg->peers : "");
    char *p = list;
    for (int n = 0; n < L.cfg.nodes; n++) {
        char *comma = p ? strchr(p, ',') : NULL;
        if (comma) *comma = 0;
        if (!set_addr(n, (p && *p) ? p : "127.0.0.1")) return false;
        p = comma ? comma + 1 : NULL;
    }
    L.list_hash = 2166136261u;                                          /* FNV-1a over the set size, the port and the addresses */
    { const uint32_t v[2] = { (uint32_t)L.cfg.nodes, (uint32_t)L.cfg.base_port };
      for (int i = 0; i < 2; i++) for (int k = 0; k < 4; k++) L.list_hash = (L.list_hash ^ (uint8_t)(v[i] >> (8 * k))) * 16777619u;
      for (int n = 0; n < L.cfg.nodes; n++) { const uint8_t *a = (const uint8_t *)&L.addr[n].sin_addr; for (int k = 0; k < 4; k++) L.list_hash = (L.list_hash ^ a[k]) * 16777619u; } }
    L.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (L.s == SOCK_BAD) { fprintf(stderr, "[LINK] no socket\n"); return false; }
    { int one = 1; setsockopt(L.s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one); }   /* (automatic finding) */
    lv_prepare(L.s);                                                    /* (each packet says which connection it came in on) */
    memset(L.via, 0, sizeof L.via);
    { lv_if f[8]; const int nf = lv_ifs(f, 8);
      L.nmine = 0; L.twin_from = 0;
      for (int i = 0; i < nf; i++) L.mine[L.nmine++].s_addr = f[i].addr; }
    struct sockaddr_in me; memset(&me, 0, sizeof me);
    me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(INADDR_ANY); me.sin_port = htons((uint16_t)(L.cfg.base_port + L.cfg.id));
    if (bind(L.s, (struct sockaddr *)&me, sizeof me) != 0) {
        fprintf(stderr, "[LINK] cannot listen on UDP port %d (is another copy already cabinet %d?)\n", L.cfg.base_port + L.cfg.id, L.cfg.id + 1);
        sock_close(L.s); return false;
    }
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(L.s, FIONBIO, &nb);
    BOOL off = FALSE; DWORD ret = 0;                   /* a send to a closed port must not make the next receive fail */
    WSAIoctl(L.s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
#else
    fcntl(L.s, F_SETFL, fcntl(L.s, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (L.cfg.gather_ms <= 0) L.cfg.gather_ms = 60000;
    L.rng = 0x9E3779B9u ^ (uint32_t)L.cfg.id * 7919u ^ (uint32_t)now_ms() ^ (uint32_t)SDL_GetPerformanceCounter();
    L.state = LNK_GATHERING;
    L.game_id = -1;
    for (int n = 0; n < LNK_MAX_NODES; n++) { L.p[n].game_id = -1; L.p[n].rtt_avg = L.p[n].rtt_max = L.p[n].loss_pct = -1; }
    L.t_open = now_ms();
    L.on = true;
    fprintf(stderr, "[LINK] cabinet %d of up to %d, UDP port %d%s; waiting up to %d s for the others\n", L.cfg.id + 1, L.cfg.nodes,
            L.cfg.base_port + L.cfg.id, cfg->peers ? "" : ", all cabinets on this PC (127.0.0.1)", L.cfg.gather_ms / 1000);
    if (cfg->delay_ms || cfg->jitter_ms || cfg->loss_pct || cfg->burst_every_ms)
        fprintf(stderr, "[LINK] TEST network: +%d ms, jitter %d ms, %d%% lost, bursts: every %d ms %d ms held\n", cfg->delay_ms, cfg->jitter_ms,
                cfg->loss_pct, cfg->burst_every_ms, cfg->burst_hold_ms);
    return true;
}

bool lnk_on(void) { return L.on; }
int  lnk_state(void) { return L.state; }
uint32_t lnk_members(void) { return L.members; }
const lnk_stats *lnk_get_stats(void) { return &L.st; }

static bool recent(int n, uint64_t t) { return L.p[n].heard && t - L.p[n].heard < ALIVE_MS; }

/* THE DIAGNOSTICS (the log, the Network page): who this one hears, from where, and who hears it. "Hears" is looser than alive: a
 * cabinet heard within the last second (a slow path still shows as heard; the link itself needs ALIVE_MS) */
bool lnk_peer_info(int n, char addr[16], int *ago_ms, uint32_t *pkts)
{
    if (n < 0 || n >= L.cfg.nodes || n == L.cfg.id || !L.p[n].ever) return false;
    const peer_t *p = &L.p[n];
    if (!inet_ntop(AF_INET, &p->from.sin_addr, addr, 16)) snprintf(addr, 16, "?");
    *ago_ms = p->heard ? (int)(now_ms() - p->heard) : -1;
    *pkts = p->pkts;
    return true;
}
int lnk_peer_setup(int n, bool *same_list)
{
    if (n < 0 || n >= L.cfg.nodes || n == L.cfg.id || !L.p[n].nodes || !L.p[n].heard || now_ms() - L.p[n].heard >= HEAR_MS) return 0;
    *same_list = L.p[n].list_hash == L.list_hash;
    return L.p[n].nodes;
}
bool lnk_peer_build(int n, uint32_t *crc, uint32_t *time)
{
    if (n < 0 || n >= L.cfg.nodes || n == L.cfg.id || !L.p[n].build_crc || !L.p[n].heard || now_ms() - L.p[n].heard >= HEAR_MS) return false;
    *crc = L.p[n].build_crc; *time = L.p[n].build_time;
    return true;
}
bool lnk_peer_quality(int n, int *rtt_avg_ms, int *rtt_max_ms, int *loss_pct)
{
    if (n < 0 || n >= L.cfg.nodes || n == L.cfg.id || !L.p[n].heard || now_ms() - L.p[n].heard >= HEAR_MS || L.p[n].rtt_avg < 0) return false;
    *rtt_avg_ms = L.p[n].rtt_avg; *rtt_max_ms = L.p[n].rtt_max; *loss_pct = L.p[n].loss_pct < 0 ? 0 : L.p[n].loss_pct;
    return true;
}
uint32_t lnk_hear_mask(void)
{
    uint64_t t = now_ms(); uint32_t m = 0;
    for (int n = 0; n < L.cfg.nodes; n++) if (n != L.cfg.id && L.p[n].heard && t - L.p[n].heard < HEAR_MS) m |= 1u << n;
    return m;
}
uint32_t lnk_heard_by_mask(void)
{
    uint64_t t = now_ms(); uint32_t m = 0;
    for (int n = 0; n < L.cfg.nodes; n++) if (n != L.cfg.id && L.p[n].heard && t - L.p[n].heard < HEAR_MS &&
                                                   ((L.p[n].hears >> L.cfg.id & 1) || t - L.p[n].ever < 1500)) m |= 1u << n;   /* (just appeared: its hellos saying so are on the way) */
    return m;
}

/* a member of this session that is on now */
static bool alive(int n, uint64_t t)
{
    if (n == L.cfg.id) return L.state == LNK_LINKED;
    return L.state == LNK_LINKED && (L.members >> n & 1) && recent(n, t) && L.p[n].state == LNK_LINKED && L.p[n].session == L.session;
}

uint32_t lnk_alive_mask(void)
{
    uint64_t t = now_ms(); uint32_t m = 0;
    for (int n = 0; n < L.cfg.nodes; n++) if (alive(n, t)) m |= 1u << n;
    return m;
}

uint32_t lnk_present_mask(void)
{
    uint64_t t = now_ms(); uint32_t m = 1u << L.cfg.id;
    for (int n = 0; n < L.cfg.nodes; n++) if (n != L.cfg.id && recent(n, t) && L.p[n].state == LNK_GATHERING) m |= 1u << n;
    return m;
}

int lnk_next_hop(void)
{
    if (L.state != LNK_LINKED) return -1;
    uint64_t t = now_ms();
    for (int k = 1; k < L.cfg.nodes; k++) { int n = (L.cfg.id + k) % L.cfg.nodes; if (alive(n, t)) return n; }
    return -1;
}

void lnk_set_game_id(int id) { L.game_id = id; }
int  lnk_peer_game_id(int n) { return n == L.cfg.id ? L.game_id : (n >= 0 && n < L.cfg.nodes && recent(n, now_ms())) ? L.p[n].game_id : -1; }

static void raw_send(int dst, const uint8_t *b, int len)
{
    if (L.find[dst] && !L.found[dst]) {                                 /* not heard yet: to every PC (its port says which cabinet) */
        lv_send_all(L.s, b, len, L.addr[dst].sin_port);                /* (out of every connection, and this PC) */
        return;
    }
    lv_send(L.s, b, len, &L.addr[dst], L.via[dst]);
}
bool lnk_peer_found(int n) { return n >= 0 && n < L.cfg.nodes && (!L.find[n] || L.found[n]); }
bool lnk_twin(char addr[16])
{
    if (!L.twin_from) return false;
    struct in_addr a; a.s_addr = htonl(L.twin_from); inet_ntop(AF_INET, &a, addr, 16); return true;
}

/* the test impairments apply to frames only (hellos go straight out) */
static void send_impaired(int dst, const uint8_t *b, int len, bool bursts);
static void send_frame_packet(int dst, const uint8_t *b, int len) { send_impaired(dst, b, len, true); }
/* TEST (--link-test): a worse network. Frames get all of it; hellos the delay, jitter and loss (a real network treats them alike) but
 * not the bursts (held 120 ms, a cabinet would count as gone) */
static void send_impaired(int dst, const uint8_t *b, int len, bool bursts)
{
    if (L.cfg.loss_pct && (int)(rnd() % 100) < L.cfg.loss_pct) { L.st.dropped_test++; return; }
    int d = L.cfg.delay_ms + (L.cfg.jitter_ms ? (int)(rnd() % (unsigned)(L.cfg.jitter_ms + 1)) : 0);
    if (bursts && L.cfg.burst_every_ms > 0 && L.cfg.burst_hold_ms > 0) {            /* a radio asleep: held to the end of its nap, then all at once */
        const int phase = (int)(now_ms() % (uint64_t)L.cfg.burst_every_ms);
        if (phase < L.cfg.burst_hold_ms) d += L.cfg.burst_hold_ms - phase;
    }
    if (d <= 0 || L.nheld >= 256) { raw_send(dst, b, len); return; }
    L.held[L.nheld].due = now_ms() + (uint64_t)d; L.held[L.nheld].dst = dst; L.held[L.nheld].len = len;
    memcpy(L.held[L.nheld].b, b, (size_t)len); L.nheld++; L.st.delayed++;
}

static void release_held(void)
{
    uint64_t t = now_ms();
    for (int i = 0; i < L.nheld; ) {
        if (L.held[i].due <= t) {                      /* in the order they were sent (jitter still reorders: its due times differ) */
            raw_send(L.held[i].dst, L.held[i].b, L.held[i].len);
            memmove(&L.held[i], &L.held[i + 1], (size_t)(L.nheld - i - 1) * sizeof L.held[0]); L.nheld--;
        } else i++;
    }
}

static int header(uint8_t *b, int type, int dst)
{
    put32(b, MAGIC); b[4] = PROTO; b[5] = (uint8_t)type; b[6] = (uint8_t)L.cfg.id; b[7] = (uint8_t)dst;
    put32(b + 8, ++L.seq_to[dst]); put32(b + 12, L.tag); put32(b + 16, L.session);
    return HDR;
}

void lnk_send(const uint16_t *w, int n)
{
    if (!L.on) return;
    int dst = lnk_next_hop();
    if (dst < 0) { L.st.no_peer++; return; }
    if (n > LNK_MAX_WORDS) n = LNK_MAX_WORDS;
    uint8_t b[HDR + 2 * LNK_MAX_WORDS];
    int len = header(b, T_FRAME, dst);
    for (int i = 0; i < n; i++) { b[len++] = (uint8_t)(w[i] >> 8); b[len++] = (uint8_t)w[i]; }
    send_frame_packet(dst, b, len);
    L.st.sent++;
}

static void hellos(void)
{
    uint64_t t = now_ms();
    if (t < L.next_hello) return;
    L.next_hello = t + HELLO_MS;
    uint8_t b[HDR + HELLO_LEN];
    uint32_t ms = (uint32_t)(t - (L.state == LNK_LINKED ? L.t_session : L.t_open));
    if (!L.q_t0) L.q_t0 = t;
    if (t - L.q_t0 >= QWIN_MS) {                                        /* close the quality window */
        static int windows;                                             /* (the first two are start-up: hellos that waited in the socket
                                                                         * while the other copy was still loading) */
        const bool counts = ++windows > 2;
        for (int n = 0; n < L.cfg.nodes; n++) {
            peer_t *p = &L.p[n];
            if (!counts) p->q_n = 0;
            p->rtt_avg = p->q_n ? (int)(p->q_sum / p->q_n) : -1; p->rtt_max = p->q_n ? (int)p->q_max : -1;
            p->loss_pct = p->q_recv + p->q_lost ? (int)(p->q_lost * 100u / (p->q_recv + p->q_lost)) : -1;
            p->q_n = p->q_sum = p->q_max = p->q_recv = p->q_lost = 0;
        }
        L.q_t0 = t;
    }
    uint8_t hear = (uint8_t)(1u << L.cfg.id);                           /* who this one hears (the others show it: a one-way path) */
    for (int n = 0; n < L.cfg.nodes; n++) if (n != L.cfg.id && L.p[n].heard && t - L.p[n].heard < HEAR_MS) hear |= (uint8_t)(1u << n);
    for (int n = 0; n < L.cfg.nodes; n++) if (n != L.cfg.id) {
        header(b, T_HELLO, n);
        b[HDR] = (uint8_t)L.state; b[HDR + 1] = L.members; b[HDR + 2] = (uint8_t)(L.game_id + 1); b[HDR + 3] = hear; put32(b + HDR + 4, ms);
        b[HDR + 8] = (uint8_t)L.cfg.nodes; b[HDR + 9] = (uint8_t)(L.my_vf >> 16); b[HDR + 10] = (uint8_t)(L.my_vf >> 8); b[HDR + 11] = (uint8_t)L.my_vf; put32(b + HDR + 12, L.list_hash);
        const peer_t *p = &L.p[n];
        put32(b + HDR + 16, (uint32_t)t); put32(b + HDR + 20, p->echo_at ? p->echo_t : 0);
        put32(b + HDR + 24, p->echo_at ? (uint32_t)(t - p->echo_at) : 0);
        put32(b + HDR + 28, L.cfg.build_crc); put32(b + HDR + 32, L.cfg.build_time);
        if (L.cfg.delay_ms || L.cfg.jitter_ms || L.cfg.loss_pct) send_impaired(n, b, HDR + HELLO_LEN, false);
        else raw_send(n, b, HDR + HELLO_LEN);
    }
}

/* one packet off the socket: hellos and goodbyes are taken in here; returns the frame's word count (0 = not a frame, -1 = none left) */
static int take(uint16_t *w, int max)
{
    uint8_t b[HDR + 2 * LNK_MAX_WORDS + 16];
    struct sockaddr_in from; uint32_t ifx = 0;
    int len = lv_recv(L.s, b, (int)sizeof b, &from, &ifx);
    if (len < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAECONNRESET) return 0;
#endif
        return -1;
    }
    if (len >= HDR && get32(b) == MAGIC && b[4] == PROTO && b[6] == L.cfg.id && get32(b + 12) == L.tag && !L.twin_from) {
        bool me = (ntohl(from.sin_addr.s_addr) >> 24) == 127;           /* our own broadcast coming back is fine; another PC is not */
        for (int i = 0; i < L.nmine; i++) me |= from.sin_addr.s_addr == L.mine[i].s_addr;
        if (!me) { L.twin_from = ntohl(from.sin_addr.s_addr); char a[16]; inet_ntop(AF_INET, &from.sin_addr, a, sizeof a);
                   fprintf(stderr, "[LINK] WARNING: the PC at %s is set up as cabinet %d too -- give every PC its own cabinet number\n", a, L.cfg.id + 1); }
    }
    if (len < HDR || get32(b) != MAGIC || b[4] != PROTO || b[6] >= L.cfg.nodes || b[6] == L.cfg.id) { L.st.bad++; return 0; }
    if (get32(b + 12) != L.tag) {
        if (!L.warned_tag) { fprintf(stderr, "[LINK] cabinet %d is running another game: ignored\n", b[6] + 1); L.warned_tag = true; }
        return 0;
    }
    int src = b[6];
    peer_t *p = &L.p[src];
    p->heard = now_ms();
    if (!p->ever) p->ever = p->heard;
    p->pkts++;
    if (from.sin_addr.s_addr != p->from.sin_addr.s_addr && p->from.sin_family) p->warned_from = false;   /* a new sender: check it again */
    p->from = from;
    if (L.find[src]) {                                                  /* automatic finding: here it is */
        const bool same = L.found[src] && L.addr[src].sin_addr.s_addr == from.sin_addr.s_addr;
        if (same) L.found_heard[src] = p->heard;
        else if (!L.found[src] || p->heard - L.found_heard[src] > 1000) {
            char a[16]; inet_ntop(AF_INET, &from.sin_addr, a, sizeof a);
            fprintf(stderr, "[LINK] found cabinet %d on the network at %s\n", src + 1, a);
            L.addr[src].sin_addr = from.sin_addr; L.found[src] = true; L.found_heard[src] = p->heard;
        }
    }
    if (from.sin_addr.s_addr == L.addr[src].sin_addr.s_addr) L.via[src] = ifx;   /* replies to it: out of the connection it came in on */
    if (!p->warned_from && !L.find[src] && (from.sin_addr.s_addr != L.addr[src].sin_addr.s_addr || from.sin_port != L.addr[src].sin_port)) {
        char got[16] = "?", want[16] = "?";
        inet_ntop(AF_INET, &from.sin_addr, got, sizeof got); inet_ntop(AF_INET, &L.addr[src].sin_addr, want, sizeof want);
        if (strncmp(got, "127.", 4) || strncmp(want, "127.", 4))       /* (all on one PC: the loopback, any address) */
            fprintf(stderr, "[LINK] NOTE: messages from cabinet %d come from %s port %u, but this PC's list has cabinet %d at %s port %u "
                    "(two PCs set up as the same cabinet, or a wrong address in a list?)\n", src + 1, got, ntohs(from.sin_port), src + 1,
                    want, ntohs(L.addr[src].sin_port));
        p->warned_from = true;
    }
    uint32_t seq = get32(b + 8);
    /* lost = gaps in the numbering; a late (reordered) packet fills its gap back in */
    if (L.last_seq[src] && seq > L.last_seq[src] + 1 && seq - L.last_seq[src] < 1000) { L.st.lost_seq += seq - L.last_seq[src] - 1; p->q_lost += seq - L.last_seq[src] - 1; }
    else if (seq < L.last_seq[src] && L.last_seq[src] - seq < 1000 && L.st.lost_seq) { L.st.lost_seq--; if (p->q_lost) p->q_lost--; }
    p->q_recv++;
    if (seq > L.last_seq[src] || L.last_seq[src] - seq > 100000) L.last_seq[src] = seq;   /* (a restarted cabinet counts from 1 again) */
    switch (b[5]) {
    case T_HELLO:
        if (len >= HDR + 8) { p->state = b[HDR]; p->members = b[HDR + 1]; p->game_id = b[HDR + 2] - 1; p->hears = b[HDR + 3]; p->session = get32(b + 16); p->ms = get32(b + HDR + 4); }
        if (len >= HDR + 16) { p->nodes = b[HDR + 8]; p->list_hash = get32(b + HDR + 12); }
        if (len >= HDR + 12) { const uint32_t v = (uint32_t)b[HDR + 9] << 16 | (uint32_t)b[HDR + 10] << 8 | b[HDR + 11]; if (v) { p->vf = v; p->vf_at = now_ms(); } }
        if (len >= HDR + HELLO_LEN) {
            p->echo_t = get32(b + HDR + 16); p->echo_at = now_ms();
            const uint32_t back = get32(b + HDR + 20), held = get32(b + HDR + 24);
            if (back) {                                                 /* our own time, back: the round trip less its wait before sending */
                const uint32_t rtt = (uint32_t)p->echo_at - back - held;
                if (rtt < 5000) { p->q_n++; p->q_sum += rtt; if (rtt > p->q_max) p->q_max = rtt; }
            }
            p->build_crc = get32(b + HDR + 28); p->build_time = get32(b + HDR + 32);
        }
        return 0;
    case T_BYE:
        p->heard = 0;
        return 0;
    case T_RESTART:                                                    /* only from a cabinet of this very set-up */
        if (len >= HDR + 4 && get32(b + HDR) == L.list_hash) L.restart_from = src + 1;
        return 0;
    case T_FRAME: {
        if (L.state != LNK_LINKED || !(L.members >> src & 1) || get32(b + 16) != L.session) { L.st.foreign++; return 0; }
        int n = (len - HDR) / 2;
        if (n > max) n = max;
        for (int i = 0; i < n; i++) w[i] = (uint16_t)(b[HDR + 2 * i] << 8 | b[HDR + 2 * i + 1]);
        L.st.recv++;
        return n;
    }
    default:
        return 0;
    }
}

/* this cabinet stands still for a moment (party sync, engine/link_host.c): its hellos go on, so the others do not count it as gone */
void lnk_idle(void) { if (L.on) { hellos(); release_held(); } }

int lnk_recv(uint16_t *w, int max)
{
    if (!L.on) return 0;
    hellos();
    release_held();
    for (;;) {
        int n = take(w, max);
        if (n < 0) return 0;
        if (n > 0) return n;
    }
}

static void start_session(uint32_t id, uint8_t members, uint32_t age_ms, const char *why)
{
    L.state = LNK_LINKED; L.session = id; L.members = members; L.t_session = now_ms() - age_ms;
    char who[40] = ""; int k = 0;
    for (int n = 0; n < L.cfg.nodes; n++) if (members >> n & 1) k += snprintf(who + k, sizeof who - (size_t)k, "%s%d", k ? ", " : "", n + 1);
    fprintf(stderr, "[LINK] linked (%s): cabinets %s, session %08X\n", why, who, id);
}

int lnk_gather_left_ms(void)
{
    uint64_t t = now_ms(); uint32_t waited = (uint32_t)(t - L.t_open);
    for (int n = 0; n < L.cfg.nodes; n++)
        if (n != L.cfg.id && recent(n, t) && L.p[n].state == LNK_GATHERING && L.p[n].ms > waited) waited = L.p[n].ms;
    return waited >= (uint32_t)L.cfg.gather_ms ? 0 : L.cfg.gather_ms - (int)waited;
}

int lnk_gather(void)
{
    if (!L.on || L.state != LNK_GATHERING) return L.state;
    hellos();
    release_held();                                                    /* (TEST delays: due while gathering too) */
    uint16_t junk[LNK_MAX_WORDS];
    while (take(junk, LNK_MAX_WORDS) >= 0) {}
    uint64_t t = now_ms();
    /* a session already running: ours (we only missed its start) or somebody else's (we are late) */
    for (int n = 0; n < L.cfg.nodes; n++) {
        const peer_t *p = &L.p[n];
        if (n == L.cfg.id || !recent(n, t) || p->state != LNK_LINKED) continue;
        if ((p->members >> L.cfg.id & 1) && p->ms < JOIN_MS) { start_session(p->session, p->members, p->ms, "joined at the start"); return L.state; }
        L.state = LNK_ALONE;
        fprintf(stderr, "[LINK] cabinet %d started after the others (they are already playing): NOT linked -- restart every cabinet to link it\n", L.cfg.id + 1);
        return L.state;
    }
    uint32_t present = lnk_present_mask(), all = (1u << L.cfg.nodes) - 1;
    int lowest = 0; while (!(present >> lowest & 1)) lowest++;
    if (lowest == L.cfg.id && (present == all || lnk_gather_left_ms() == 0)) {       /* this cabinet decides */
        uint32_t id = (uint32_t)(L.cfg.id + 1) << 24 | (rnd() & 0xFFFFFFu);
        start_session(id, (uint8_t)present, 0, present == all ? "every cabinet is here" : "the wait ran out");
        L.next_hello = 0; hellos();                                                  /* tell the others at once */
    } else if (lnk_gather_left_ms() == 0 && t - L.t_open > (uint64_t)L.cfg.gather_ms + 15000) {
        L.state = LNK_ALONE;                                                         /* nobody opened a session: play alone */
        fprintf(stderr, "[LINK] no session was opened: NOT linked\n");
    }
    return L.state;
}

void lnk_send_restart(void)
{
    if (!L.on) return;
    uint8_t b[HDR + 4];
    for (int k = 0; k < 3; k++)                                        /* three times: it is UDP */
        for (int n = 0; n < L.cfg.nodes; n++) if (n != L.cfg.id) { header(b, T_RESTART, n); put32(b + HDR, L.list_hash); raw_send(n, b, HDR + 4); }
}
int lnk_restart_request(void) { const int r = L.restart_from; L.restart_from = 0; return r; }

void lnk_close(void)
{
    if (!L.on) return;
    uint8_t b[HDR];
    for (int k = 0; k < 3; k++)                               /* a goodbye (three times: it is UDP) */
        for (int n = 0; n < L.cfg.nodes; n++) if (n != L.cfg.id) { header(b, T_BYE, n); raw_send(n, b, HDR); }
    sock_close(L.s); L.on = false;
}

int lnk_local_addresses(char out[][16], int max)
{
#ifdef _WIN32
    { lv_if f[8]; const int nf = lv_ifs(f, 8);
      if (nf > 0) { int n = 0;
                    for (int pass = 0; pass < 2; pass++)                /* a router's addresses first, then the no-router ones (169.254.x.x) */
                        for (int i = 0; i < nf && n < max; i++) {
                            const bool ll = (ntohl(f[i].addr) >> 16) == 0xA9FE;
                            if (ll != (pass == 1)) continue;
                            struct in_addr a; a.s_addr = f[i].addr; inet_ntop(AF_INET, &a, out[n++], 16);
                        }
                    return n; } }
    static bool wsa;
    if (!wsa) { WSADATA wd; wsa = WSAStartup(MAKEWORD(2, 2), &wd) == 0; }
#endif
    char host[256];
    if (gethostname(host, sizeof host) != 0) return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return 0;
    int n = 0;
    for (struct addrinfo *r = res; r && n < max; r = r->ai_next) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)r->ai_addr;
        char s[16];
        if (!inet_ntop(AF_INET, &a->sin_addr, s, sizeof s) || !strncmp(s, "127.", 4) || !strncmp(s, "169.254.", 8)) continue;   /* loopback; Windows' no-router fallback */
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= !strcmp(out[i], s);
        if (!dup) snprintf(out[n++], 16, "%s", s);
    }
    freeaddrinfo(res);
    return n;
}

/* our fork: KEEPING THE SET IN STEP between the games' own syncs (attract mode runs on each cabinet alone). Each cabinet tells the
 * others in its hellos how many video frames it has run since the set started; a cabinet whose program stopped for a moment (a
 * texture load, a busy PC) or whose screen refreshes a little slower is behind from then on, and lnk_frames_behind says by how much
 * (engine/link_host.c then has it run frames without drawing them until it is level). */
void lnk_set_frame(uint32_t frames) { L.my_vf = (frames + 1) & 0xFFFFFF; }
double lnk_frames_behind(void)
{
    if (!L.on || L.state != LNK_LINKED || !L.my_vf) return 0;
    const uint64_t t = now_ms();
    double worst = 0;
    for (int n = 0; n < L.cfg.nodes; n++) {
        const peer_t *p = &L.p[n];
        if (n == L.cfg.id || !(L.members >> n & 1) || !p->vf || t - p->vf_at > 300 || p->state != LNK_LINKED || p->session != L.session) continue;
        const double travel = p->rtt_avg > 0 ? p->rtt_avg / 2.0 : 0.0;
        const double est = (double)(p->vf - 1) + (double)(t - p->vf_at + travel) * 59.906 / 1000.0;   /* its frame now */
        const double d = est - (double)(L.my_vf - 1);
        if (d > worst) worst = d;
    }
    return worst;
}

/* the menu's "This PC" lines: each address with its connection's name ("192.168.1.66 (WiFi)") */
int lnk_local_named(char out[][48], int max)
{
#ifdef _WIN32
    lv_if f[8]; const int nf = lv_ifs(f, 8);
    if (nf > 0) { int n = 0;
                  for (int i = 0; i < nf && n < max; i++) { struct in_addr a; a.s_addr = f[i].addr; char ip[16]; inet_ntop(AF_INET, &a, ip, sizeof ip);
                                                            snprintf(out[n++], 48, "%s (%.24s)", ip, f[i].name[0] ? f[i].name : "?"); }
                  return n; }
#endif
    char a[8][16]; const int n = lnk_local_addresses(a, max < 8 ? max : 8);
    for (int i = 0; i < n; i++) snprintf(out[i], 48, "%.15s", a[i]);
    return n;
}
