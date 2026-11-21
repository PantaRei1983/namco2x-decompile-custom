/* lan_via.h -- SENDING OUT OF THE RIGHT CONNECTION (every LAN game: engine/link_net.c, engine/link_host.c, gunwars/src/gw_link.c,
 * s23/src/s23_c422.c include it).
 *
 * Why: cabinets on a plain switch with no router get Windows' fallback addresses (169.254.x.x). A VPN such as Tailscale claims the
 * same 169.254.0.0/16 range with a better metric, so Windows sends the link's packets into the VPN and the other cabinets never get
 * them (this PC hears them, they don't hear it). So every packet received says which connection it came in on (IP_PKTINFO), and the
 * replies to that cabinet go back out of that same connection; the "anyone there?" broadcasts go out of every connection.
 * Windows only (elsewhere: plain recvfrom/sendto). iphlpapi.dll is loaded at run time: no build change for any game. */
#ifndef LAN_VIA_H
#define LAN_VIA_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
typedef SOCKET lv_sock;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int lv_sock;
#endif

typedef struct { uint32_t addr, mask, ifx; char name[24]; } lv_if;   /* addr/mask in network order; ifx = Windows' interface index */

/* this PC's connections that are up, each with its IPv4 address (not 127.x; the ones Windows is still checking left out) */
static inline int lv_ifs_now(lv_if *o, int max)
{
    int n = 0;
#ifdef _WIN32
    typedef ULONG (WINAPI *gaa_t)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);
    static gaa_t gaa; static bool tried;
    if (!tried) { tried = true; HMODULE h = LoadLibraryA("iphlpapi.dll"); if (h) gaa = (gaa_t)(void *)GetProcAddress(h, "GetAdaptersAddresses"); }
    if (!gaa) return 0;
    ULONG sz = 16384; IP_ADAPTER_ADDRESSES *aa = NULL;
    for (int tries = 0; tries < 3; tries++) {
        aa = (IP_ADAPTER_ADDRESSES *)malloc(sz); if (!aa) return 0;
        const ULONG r = gaa(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz);
        if (r == NO_ERROR) break;
        free(aa); aa = NULL; if (r != ERROR_BUFFER_OVERFLOW) return 0;
    }
    if (!aa) return 0;
    for (IP_ADAPTER_ADDRESSES *a = aa; a && n < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET || u->DadState != IpDadStatePreferred) continue;
            const uint32_t ip = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr;
            if ((ntohl(ip) >> 24) == 127) continue;
            const unsigned pl = u->OnLinkPrefixLength <= 32 ? u->OnLinkPrefixLength : 24;
            o[n].addr = ip; o[n].mask = htonl(pl ? 0xFFFFFFFFu << (32 - pl) : 0); o[n].ifx = a->IfIndex;
            o[n].name[0] = 0;
            if (a->FriendlyName) WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, o[n].name, (int)sizeof o[n].name, NULL, NULL);
            o[n].name[sizeof o[n].name - 1] = 0;
            n++;
        }
    }
    free(aa);
#else
    (void)o; (void)max;
#endif
    return n;
}

/* the same, looked up at most once a second (the menu asks every frame) */
static inline int lv_ifs(lv_if *o, int max)
{
    static lv_if c[8]; static int nc; static uint32_t at;
#ifdef _WIN32
    const uint32_t now = GetTickCount();
#else
    const uint32_t now = 1;
#endif
    if (!at || now - at > 1000) { nc = lv_ifs_now(c, 8); at = now ? now : 1; }
    const int n = nc < max ? nc : max; memcpy(o, c, sizeof c[0] * (size_t)n); return n;
}

/* a socket that will use lv_recv: ask for the arriving connection with every packet */
static inline void lv_prepare(lv_sock s)
{
#ifdef _WIN32
    DWORD one = 1; setsockopt(s, IPPROTO_IP, IP_PKTINFO, (const char *)&one, sizeof one);
#else
    (void)s;
#endif
}

/* recvfrom, plus the connection it came in on (*ifx; 0 = not known) */
static inline int lv_recv_one(lv_sock s, void *buf, int max, struct sockaddr_in *from, uint32_t *ifx)
{
    *ifx = 0;
#ifdef _WIN32
    static LPFN_WSARECVMSG rm; static bool tried;
    if (!tried) { GUID g = WSAID_WSARECVMSG; DWORD got = 0;
                  if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &g, sizeof g, &rm, sizeof rm, &got, NULL, NULL) != 0) rm = NULL;
                  tried = true; }
    if (rm) {
        char ctl[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))];
        WSABUF wb; wb.buf = (char *)buf; wb.len = (ULONG)max;
        WSAMSG m; memset(&m, 0, sizeof m);
        m.name = (struct sockaddr *)from; m.namelen = sizeof *from; m.lpBuffers = &wb; m.dwBufferCount = 1;
        m.Control.buf = ctl; m.Control.len = sizeof ctl;
        DWORD got = 0;
        if (rm(s, &m, &got, NULL, NULL) != 0) {
            if (WSAGetLastError() == WSAEMSGSIZE) return max;          /* (too big for us: not one of ours anyway) */
            return -1;
        }
        for (WSACMSGHDR *c = WSA_CMSG_FIRSTHDR(&m); c; c = WSA_CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) *ifx = ((IN_PKTINFO *)WSA_CMSG_DATA(c))->ipi_ifindex;
        return (int)got;
    }
#endif
    socklen_t fl = sizeof *from;
    return (int)recvfrom(s, (char *)buf, max, 0, (struct sockaddr *)from, &fl);
}

/* is a one of this PC's own addresses (127.x included)? */
static inline bool lv_is_mine(uint32_t a)
{
    if ((ntohl(a) >> 24) == 127) return true;
    lv_if f[8]; const int n = lv_ifs(f, 8);
    for (int i = 0; i < n; i++) if (f[i].addr == a) return true;
    return false;
}

/* recvfrom + the connection; a packet with no sender address (0.0.0.0: a broadcast another PC sent before Windows gave it an address
 * on that connection) is skipped -- an answer to 0.0.0.0 goes nowhere */
static inline int lv_recv(lv_sock s, void *buf, int max, struct sockaddr_in *from, uint32_t *ifx)
{
    for (;;) {
        memset(from, 0, sizeof *from);
        const int r = lv_recv_one(s, buf, max, from, ifx);
        if (r < 0 || from->sin_addr.s_addr != 0) return r;
    }
}

/* sendto, out of connection ifx (0 = wherever Windows chooses; to this PC's own addresses always the usual way) */
static inline int lv_send(lv_sock s, const void *buf, int len, const struct sockaddr_in *to, uint32_t ifx)
{
#ifdef _WIN32
    if (ifx && !lv_is_mine(to->sin_addr.s_addr)) {                     /* (to this PC itself: the usual way, it never leaves) */
        char ctl[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))]; memset(ctl, 0, sizeof ctl);
        WSABUF wb; wb.buf = (char *)buf; wb.len = (ULONG)len;
        WSAMSG m; memset(&m, 0, sizeof m);
        m.name = (struct sockaddr *)to; m.namelen = sizeof *to; m.lpBuffers = &wb; m.dwBufferCount = 1;
        m.Control.buf = ctl; m.Control.len = sizeof ctl;
        WSACMSGHDR *c = WSA_CMSG_FIRSTHDR(&m);
        c->cmsg_level = IPPROTO_IP; c->cmsg_type = IP_PKTINFO; c->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
        IN_PKTINFO *pi = (IN_PKTINFO *)WSA_CMSG_DATA(c); pi->ipi_addr.s_addr = 0; pi->ipi_ifindex = ifx;
        { lv_if f[8]; const int n = lv_ifs(f, 8);                     /* the sender's address: that connection's own (left empty, Windows
                                                                         * sends a broadcast from 0.0.0.0 and the others can't answer it) */
          for (int i = 0; i < n; i++) if (f[i].ifx == ifx) { pi->ipi_addr.s_addr = f[i].addr; break; } }
        DWORD sent = 0;
        if (WSASendMsg(s, &m, 0, &sent, NULL, NULL) == 0) return (int)sent;
    }
#else
    (void)ifx;
#endif
    return (int)sendto(s, (const char *)buf, len, 0, (const struct sockaddr *)to, sizeof *to);
}

/* to every PC: 255.255.255.255 the usual way, then out of each connection (255.255.255.255 and its own network's broadcast address),
 * then this PC (copies on one PC). port in network order. */
static inline void lv_send_all(lv_sock s, const void *buf, int len, uint16_t port)
{
    struct sockaddr_in to; memset(&to, 0, sizeof to); to.sin_family = AF_INET; to.sin_port = port;
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST); lv_send(s, buf, len, &to, 0);
    lv_if ifs[8]; const int nif = lv_ifs(ifs, 8);
    for (int i = 0; i < nif; i++) {
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST); lv_send(s, buf, len, &to, ifs[i].ifx);
        to.sin_addr.s_addr = ifs[i].addr | ~ifs[i].mask;
        if (to.sin_addr.s_addr != htonl(INADDR_BROADCAST)) lv_send(s, buf, len, &to, ifs[i].ifx);
    }
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); lv_send(s, buf, len, &to, 0);
}

#endif
