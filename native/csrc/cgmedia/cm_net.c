/*
 * cm_net.c —— UDP socket 工具（纯 C，无 libnice）
 *
 * 设计要点：
 *   - 绑定 0.0.0.0:0 让内核挑端口，STUN / DTLS / SRTP 全程复用这一个 socket
 *   - 非阻塞 + poll() 读，超时返回 0（收包线程靠它做周期性状态推进）
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cm_session.h"

int cm_udp_open(cm_sess *s) {
    int fd;
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int fl;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        CM_LOGE("socket() fail: %s", strerror(errno));
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        CM_LOGE("bind() fail: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&a, &al) < 0) {
        close(fd);
        return -1;
    }
    s->local_port = ntohs(a.sin_port);

    fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    /* 对端地址：先把解析好的固定目标填上（gateway host candidate 是固定 IP:port） */
    memset(&s->peer, 0, sizeof(s->peer));
    s->peer.sin_family = AF_INET;
    s->peer.sin_port = htons((uint16_t)s->cfg.remote_port);
    if (inet_pton(AF_INET, s->cfg.remote_ip, &s->peer.sin_addr) != 1) {
        CM_LOGE("bad remoteIp '%s'", s->cfg.remote_ip);
        close(fd);
        return -1;
    }
    s->peer_set = 1;
    s->sock = fd;
    CM_LOGI("udp bound 0.0.0.0:%d -> %s:%d", s->local_port, s->cfg.remote_ip, s->cfg.remote_port);
    return 0;
}

/*
 * 把 socket "连接"到对端。
 *
 * 为什么必须真 connect()：OpenSSL 的 dgram BIO 在写记录时走 sendmsg/sendmmsg，
 * 若 socket 未连接，msg_name 为 NULL 会直接 EINVAL/EDESTADDRREQ(89)。
 * __BIO_CTRL_DGRAM_SET_PEER 只让 BIO 记住地址，底层仍依赖 connect() 或
 * 显式 msg_name——实测在设备上表现为 ClientHello 写出失败
 * "handshake fail e=5 r=-1 errno=89(Destination address required)"。
 *
 * 时机：必须在 ICE 全部结束、peer 定稿之后调用；ICE 期间用 sendto 显式地址，
 * 且网关可能从与 cfg 不同的地址回包（cm_ice_run 会据此切换 peer），
 * 提前 connect 会把那些回包过滤掉。
 */
int cm_udp_connect_peer(cm_sess *s) {
    if (!s || s->sock < 0 || !s->peer_set) return -1;
    if (connect(s->sock, (struct sockaddr *)&s->peer, sizeof(s->peer)) < 0) {
        CM_LOGE("#%d connect(peer) fail: %s", s->id, strerror(errno));
        return -1;
    }
    CM_LOGI("#%d socket connected to peer", s->id);
    return 0;
}

void cm_udp_close(int fd) {
    if (fd >= 0) close(fd);
}

int cm_udp_send(int fd, const void *data, int len, const char *ip, int port) {
    struct sockaddr_in a;
    (void)ip;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return -1;
    return (int)sendto(fd, data, (size_t)len, 0, (struct sockaddr *)&a, sizeof(a));
}

int cm_udp_recv(int fd, unsigned char *buf, int cap, int timeout_ms,
                char *from_ip, int *from_port) {
    struct pollfd pfd;
    int r;
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);

    if (fd < 0) return -1;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = poll(&pfd, 1, timeout_ms);
    if (r < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    if (r == 0) return 0;
    memset(&from, 0, sizeof(from));
    r = (int)recvfrom(fd, buf, (size_t)cap, 0, (struct sockaddr *)&from, &fl);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
    if (from_ip && fl >= sizeof(struct sockaddr_in)) {
        inet_ntop(AF_INET, &from.sin_addr, from_ip, 64);
        if (from_port) *from_port = ntohs(from.sin_port);
    }
    return r;
}

/* =====================================================================
 * 收包分类（统一泵）
 *
 * 背景：网关 ICE 连通后会持续发 STUN Binding Request keepalive
 * （实测 ~1.8 秒一个）。以前 DTLS 阶段直接让 OpenSSL 独占 socket，
 * 这些 STUN 包被当成 DTLS 记录：`dtls1_get_record` 读到 type=0x00、
 * 版本不是 0xFEFD/0xFEFF 就走 "drop this record, goto again"，
 * **不发回调、不报错、继续等下一条**。表现就是真机日志里
 * `in=14 (in_bytes=182)`（14 × 13 字节记录头）且一条 handshake 都没有，
 * 25 秒后握手超时。
 *
 * 做法：所有 datagram 先在这里分一次类。
 *   - STUN  → 消费掉，Binding Request 当场回 Binding Success（keepalive 必须回）
 *   - 垃圾  → 消费掉丢弃
 *   - DTLS  → **不消费**（MSG_PEEK 已回滚），留在 socket 里给 OpenSSL 读
 * 这样既清掉了干扰，又完全不动已在真机验证过的握手读取路径。
 * ===================================================================== */

#define CM_CLS_NONE 0
#define CM_CLS_DTLS 1
#define CM_CLS_RTP  2
#define CM_CLS_RTCP 3
#define CM_CLS_STUN 4
#define CM_CLS_JUNK 5

static int classify(const unsigned char *b, int n) {
    if (n < 13) {
        /* DTLS 记录头就要 13 字节；短包只可能是 STUN? 不，STUN 至少 20。
         * 统一当垃圾，但要先看是不是短 STUN 片段 —— 不可能，STUN 头固定 20。 */
        return CM_CLS_JUNK;
    }
    /* STUN: 高两位 00 + magic cookie */
    if ((b[0] & 0xC0) == 0x00 && n >= 20 && cm_rd32(b + 4) == 0x2112A442u) return CM_CLS_STUN;
    /* DTLS 记录：content type 20/21/22/23 + version 0xFEFD/0xFEFF */
    if (b[0] >= 20 && b[0] <= 23 && b[1] == 0xFE && (b[2] == 0xFD || b[2] == 0xFF))
        return CM_CLS_DTLS;
    /* RTP/RTCP 都是 version=2 */
    if ((b[0] & 0xC0) == 0x80) {
        /* RTCP 的 PT 字段（未掩 marker）192..223 */
        if (b[1] >= 192 && b[1] <= 223) return CM_CLS_RTCP;
        return CM_CLS_RTP;
    }
    return CM_CLS_JUNK;
}

/* 消费 socket 上的一个 datagram（不带 MSG_PEEK） */
static int consume(int fd, unsigned char *buf, int cap, char *from_ip, int *from_port) {
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n;
    memset(&from, 0, sizeof(from));
    n = (int)recvfrom(fd, buf, (size_t)cap, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    if (from_ip && fl >= sizeof(struct sockaddr_in)) {
        inet_ntop(AF_INET, &from.sin_addr, from_ip, 64);
        if (from_port) *from_port = ntohs(from.sin_port);
    }
    return n;
}

/*
 * 等到 socket 上出现一条 DTLS 记录为止，途中把 STUN/垃圾清掉。
 *
 * 注意一律用 MSG_DONTWAIT：本模块不假设调用方把 socket 设成了非阻塞。
 * （踩过：dselftest part1 的 loopback socket 是阻塞的，MSG_PEEK 无
 * MSG_DONTWAIT 时直接永久睡在 __skb_wait_for_more_packets。）
 * 返回 1=有 DTLS 记录可供 OpenSSL 读；0=超时；<0=socket 错误。
 */
int cm_udp_prepare_dtls(cm_sess *s, int timeout_ms) {
    uint64_t deadline = cm_now_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    unsigned char peek[2048];
    char ip[64];
    int port;
    int rounds = 0;

    for (;;) {
        int n;
        if (s->cancelled) return 0;
        /* 注意用 recvfrom 而不是 recv：drain 阶段 socket 还没 connect()，
         * recv 会直接 ENOTCONN。recvfrom + MSG_PEEK 两种情况都能用。 */
        n = (int)recvfrom(s->sock, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT, NULL, NULL);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
            /* 空：等到 deadline 或下次 poll */
            {
                struct pollfd pfd;
                int left = (int)(deadline - cm_now_ms());
                if (left <= 0) return 0;
                if (left > 200) left = 200;
                pfd.fd = s->sock;
                pfd.events = POLLIN;
                pfd.revents = 0;
                poll(&pfd, 1, left);
                continue;
            }
        }
        if (n == 0) {
            /* 0 长度的 datagram：消费掉 */
            consume(s->sock, peek, sizeof(peek), ip, &port);
            s->stats.rx_other++;
            continue;
        }
        switch (classify(peek, n)) {
        case CM_CLS_DTLS:
            return 1; /* 留在 socket 里，OpenSSL 自己读 */
        case CM_CLS_STUN: {
            unsigned char tmp[2048];
            int m = consume(s->sock, tmp, sizeof(tmp), ip, &port);
            s->stats.rx_pkts++;
            s->stats.rx_stun++;
            if (m > 0) cm_stun_handle(s, tmp, m, ip, port);
            break;
        }
        default: {
            unsigned char tmp[2048];
            int m = consume(s->sock, tmp, sizeof(tmp), ip, &port);
            s->stats.rx_pkts++;
            s->stats.rx_other++;
            /* 前几条垃圾打 hex，方便对账 */
            if (m > 0 && s->stats.rx_other <= 4) {
                char hex[3 * 16 + 1];
                int i, o = 0;
                int lim = m < 16 ? m : 16;
                for (i = 0; i < lim && o < (int)sizeof(hex) - 3; i++)
                    o += snprintf(hex + o, sizeof(hex) - (size_t)o, "%02x ", tmp[i]);
                CM_LOGD("#%d dropped %d junk bytes (%s)", s->id, m, hex);
            }
            break;
        }
        }
        rounds++;
        if (rounds > 256) return 0; /* 防饿死 */
    }
}

/*
 * 媒体阶段：返回 1=RTP/SRTP 2=RTCP/SRTCP（内容在 buf 里）0=无 <0=错误。
 * STUN keepalive 当场应答，DTLS 记录（握手期外的 alert/appdata）消费掉。
 */
int cm_udp_pump_media(cm_sess *s, unsigned char *buf, int cap, int timeout_ms,
                      char *from_ip, int *from_port, int *out_len) {
    uint64_t deadline = cm_now_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    int rounds = 0;

    *out_len = 0;
    for (;;) {
        int n;
        if (s->cancelled || s->rx_quit) return 0;
        n = (int)recvfrom(s->sock, buf, (size_t)cap, MSG_PEEK | MSG_DONTWAIT, NULL, NULL);
        if (n < 0) {
            int left;
            struct pollfd pfd;
            /* UDP socket 已经 connect() 过，网关（或中间设备）回 ICMP port
             * unreachable 时内核会把错误挂到 socket 上，下一次 recvfrom 直接
             * 返回 ECONNREFUSED / ECONNRESET。这不是致命错误 —— 只要继续收包
             * 就行，绝不能因此退出媒体循环（真机上就是「收到 SPS/PPS 后立刻
             * 退出，画面永远黑」的根因）。ENETUNREACH/EHOSTUNREACH 同理按瞬态
             * 处理，交给外层 poll 重新等待。 */
            if (errno == ECONNREFUSED || errno == ECONNRESET ||
                errno == ENETUNREACH || errno == EHOSTUNREACH ||
                errno == ENETDOWN || errno == EPROTO) {
                s->stats.rx_icmp++;
                poll(NULL, 0, 5);
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
            left = (int)(deadline - cm_now_ms());
            if (left <= 0) return 0;
            if (left > 20) left = 20;
            pfd.fd = s->sock;
            pfd.events = POLLIN;
            pfd.revents = 0;
            poll(&pfd, 1, left);
            /* 超时窗口用完就返回，让上层跑 kf_tick/stat */
            if (cm_now_ms() >= deadline) return 0;
            continue;
        }
        switch (classify(buf, n)) {
        case CM_CLS_STUN: {
            unsigned char tmp[2048];
            int m = consume(s->sock, tmp, sizeof(tmp), from_ip, from_port);
            s->stats.rx_pkts++;
            s->stats.rx_stun++;
            if (m > 0) cm_stun_handle(s, tmp, m, from_ip, *from_port);
            break;
        }
        case CM_CLS_DTLS: {
            unsigned char tmp[2048];
            int m = consume(s->sock, tmp, sizeof(tmp), from_ip, from_port);
            s->stats.rx_pkts++;
            s->stats.rx_other++;
            CM_LOGD("#%d late DTLS record during media (%d bytes, type=%d)", s->id, m, tmp[0]);
            break;
        }
        case CM_CLS_RTP:
        case CM_CLS_RTCP: {
            int cls = classify(buf, n);
            int m = consume(s->sock, buf, (size_t)cap, from_ip, from_port);
            if (m <= 0) return 0;
            s->stats.rx_pkts++;
            if (cls == CM_CLS_RTCP) { s->stats.rx_rtcp++; *out_len = m; return 2; }
            s->stats.rx_rtp++;
            *out_len = m;
            return 1;
        }
        default: {
            unsigned char tmp[2048];
            int m = consume(s->sock, tmp, sizeof(tmp), from_ip, from_port);
            if (m > 0) s->stats.rx_pkts++;
            s->stats.rx_other++;
            break;
        }
        }
        rounds++;
        if (rounds > 256) return 0;
    }
}

/*
 * 发到会话对端。
 *
 * DTLS 阶段 socket 已经 connect() 过，send() 即可；ICE 阶段用 sendto
 * 显式地址（网关可能从与 cfg 不同的地址回包）。两条路都覆盖。
 */
int cm_udp_send_peer(cm_sess *s, const void *data, int len) {
    ssize_t r;
    if (!s || s->sock < 0) return -1;
    r = send(s->sock, data, (size_t)len, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        /* 未 connect 时 send() 报 EDESTADDRREQ，回落到 sendto */
        if (errno == EDESTADDRREQ || errno == ENOTCONN || errno == EINVAL) {
            if (!s->peer_set) return -1;
            return (int)sendto(s->sock, data, (size_t)len, 0,
                               (struct sockaddr *)&s->peer, sizeof(s->peer));
        }
        return -1;
    }
    return (int)r;
}
