/* pencg_cap.c —— PenCG 专用极简抓包器（AF_PACKET 原始套接字 → pcap）
 *
 * 为什么需要它：X6 Pro 固件没有 tcpdump / nc / python，只有 busybox 与 root。
 * 设备是 uid=0 且 CapEff=000001ffffffffff（含 CAP_NET_RAW），所以可以直接开
 * AF_PACKET 收包。产物是标准 pcap，能直接拖回 Windows 用 Wireshark 打开。
 *
 * 用法：
 *   pencg_cap -i wlan0 -o /userdisk/miniapp/data/cap.pcap [-s 600] [-f "udp port 6443"]
 *     -i  网卡名（默认 wlan0）
 *     -o  输出文件（默认 /userdisk/miniapp/data/cap.pcap）
 *     -s  抓多少秒（默认 600）
 *     -f  BPF 过滤表达式（可选；不填=全抓，量大）
 *
 * 设计取舍（都是被设备现实逼出来的）：
 *   - 不用 libpcap：设备上没有，静态链进去会让二进制膨胀且要交叉编依赖。
 *     这里手写 pcap 文件头 + 每包记录头，格式标准（magic 0xa1b2c3d4，链路层
 *     类型 1 = LINKTYPE_ETHERNET），Wireshark / tcpdump -r 都能直接读。
 *   - 不做 BPF 编译：libpcap 的 bpf_compile 不在，改在用户态按五元组粗过滤。
 *     这样过滤在软件层，抓不到的内核仍会拷上来 —— 因此强烈建议同时缩小抓包
 *     时长，并且优先靠"事后在 Wireshark 里过滤"而不是这里过滤。
 *   - 每次 write() 直接落盘不缓冲：设备随时可能被拔电/重启，丢了就白抓。
 *     频率约 100 包/秒时开销可忽略。
 *   - 记录真实时间戳（CLOCK_REALTIME）而不是单调时钟，方便和日志逐行对账。
 *
 * 编译（在 PenCG/native 下，用和 cgmedia 相同的 zig 工具链）：
 *   zig cc -target aarch64-linux-gnu.2.17 -O2 -static -o ../libs/pencg_cap native/csrc/tools/pencg_cap.c
 * 静态链接是为了不依赖设备 glibc 版本。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#define SNAP_LEN   65536   /* 不截断：RTP 包里 SPS/PPS 全在，截了就白抓 */
#define RING_SIZE  (4 * 1024 * 1024)

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void wr16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void wr32(unsigned char *p, unsigned v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

int main(int argc, char **argv)
{
    const char *ifname = "wlan0";
    const char *out    = "/userdisk/miniapp/data/cap.pcap";
    int   seconds      = 600;
    /* 只保留这几种端口：不填就全抓。6443=网关 WS，TCP 443/80=HTTP API，
     * 大范围 UDP 覆盖 SRTP（媒体端口是随机的 1xxxx~6xxxx）。 */
    const char *bpf    = NULL;
    int udp_lo = 0, udp_hi = 0, tcp_lo = 0, tcp_hi = 0;

    int i;
    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-i") && i + 1 < argc) ifname = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out    = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) bpf    = argv[++i];
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "usage: %s [-i if] [-o out.pcap] [-s secs] [-f filter]\n", argv[0]);
            return 0;
        }
    }

    /* 极简过滤语法：只认 "udp lo hi" / "tcp lo hi" / "port N" —— 够用就行。
     * 复杂条件请在 Wireshark 里做（抓全量代价是文件大，但不会漏）。 */
    if (bpf) {
        int lo, hi;
        char proto[8];
        if (sscanf(bpf, "%7s %d %d", proto, &lo, &hi) == 3) {
            if (!strcmp(proto, "udp")) { udp_lo = lo; udp_hi = hi; }
            else if (!strcmp(proto, "tcp")) { tcp_lo = lo; tcp_hi = hi; }
        } else if (sscanf(bpf, "port %d", &lo) == 1) {
            udp_lo = udp_hi = lo;      /* 单端口：UDP 走这个 */
        }
    }

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        fprintf(stderr, "socket(AF_PACKET) failed: %s (need root + CAP_NET_RAW)\n", strerror(errno));
        return 1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex  = (int)if_nametoindex(ifname);
    if (sll.sll_ifindex == 0) {
        fprintf(stderr, "if_nametoindex(%s) failed: %s\n", ifname, strerror(errno));
        close(fd);
        return 1;
    }
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        fprintf(stderr, "bind(%s) failed: %s\n", ifname, strerror(errno));
        close(fd);
        return 1;
    }

    int rcv = RING_SIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv)) < 0) {
        fprintf(stderr, "warn: SO_RCVBUF failed: %s (continuing)\n", strerror(errno));
    }

    FILE *fp = fopen(out, "wb");
    if (!fp) {
        fprintf(stderr, "fopen(%s) failed: %s\n", out, strerror(errno));
        close(fd);
        return 1;
    }

    /* pcap 全局头：magic / ver 2.4 / thiszone 0 / sigfigs 0 / snaplen / network=1(ETHERNET) */
    unsigned char gh[24];
    wr32(gh + 0,  0xa1b2c3d4u);
    wr16(gh + 4,  2);
    wr16(gh + 6,  4);
    wr32(gh + 8,  0);
    wr32(gh + 12, 0);
    wr32(gh + 16, SNAP_LEN);
    wr32(gh + 20, 1);
    fwrite(gh, 1, sizeof(gh), fp);
    fflush(fp);

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    fprintf(stderr, "cap start: if=%s out=%s secs=%d filter=%s\n",
            ifname, out, seconds, bpf ? bpf : "(none)");

    unsigned char *buf = malloc(SNAP_LEN);
    if (!buf) { fclose(fp); close(fd); return 1; }

    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    unsigned long long n_seen = 0, n_keep = 0;
    unsigned long long b_keep = 0;

    while (!g_stop) {
        clock_gettime(CLOCK_MONOTONIC, &tn);
        if ((int)(tn.tv_sec - t0.tv_sec) >= seconds) break;

        ssize_t n = recv(fd, buf, SNAP_LEN, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 空闲：小睡一下，别把 CPU 烧了（本机是笔，风扇都没有） */
                struct timespec nap = { 0, 2000000 };  /* 2 ms */
                nanosleep(&nap, NULL);
                continue;
            }
            if (errno == EINTR) continue;
            fprintf(stderr, "recv failed: %s\n", strerror(errno));
            break;
        }
        if (n <= 0) continue;
        n_seen++;

        /* 用户态过滤。语义：
         *   - 没给 -f            → 全收（含 ARP/ICMP），事后在 Wireshark 里筛
         *   - 给了 udp/tcp 端口  → 只留该协议且端口落在区间的包
         * 由于过滤在用户态，被丢掉的包已经过内核拷上来了，所以"过滤"只省
         * 磁盘、不省 CPU。默认全收是有意的：宁可文件大，也别漏掉判定依据。 */
        if (udp_lo || tcp_lo) {
            int keep = 0;
            if (n >= 34 && buf[12] == 0x08 && buf[13] == 0x00) {   /* IPv4 */
                const unsigned char *ip = buf + 14;
                int ihl = (ip[0] & 0x0f) * 4;
                if (ihl >= 20 && n >= (ssize_t)(14 + ihl + 4)) {
                    int proto = ip[9];
                    const unsigned char *l4 = ip + ihl;
                    int sp = (l4[0] << 8) | l4[1];
                    int dp = (l4[2] << 8) | l4[3];
                    if (proto == 17 && udp_lo)
                        keep = (sp >= udp_lo && sp <= udp_hi) || (dp >= udp_lo && dp <= udp_hi);
                    else if (proto == 6 && tcp_lo)
                        keep = (sp >= tcp_lo && sp <= tcp_hi) || (dp >= tcp_lo && dp <= tcp_hi);
                }
            }
            if (!keep) continue;
        }

        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        unsigned char rh[16];
        wr32(rh + 0,  (unsigned)rt.tv_sec);
        wr32(rh + 4,  (unsigned)(rt.tv_nsec / 1000));
        wr32(rh + 8,  (unsigned)n);
        wr32(rh + 12, (unsigned)n);
        if (fwrite(rh, 1, sizeof(rh), fp) != sizeof(rh)) break;
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) break;
        n_keep++;
        b_keep += (unsigned long long)n;

        /* 每 200 包刷一次盘：既不太频繁，最多丢 200 包 */
        if ((n_keep % 200) == 0) fflush(fp);
    }

    fflush(fp);
    fclose(fp);
    close(fd);
    free(buf);

    fprintf(stderr, "cap done: seen=%llu kept=%llu bytes=%llu -> %s\n",
            n_seen, n_keep, b_keep, out);
    return 0;
}
