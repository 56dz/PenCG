/*
 * cm_dtls.c —— 在同一个 UDP socket 上做 DTLS 握手并导出 SRTP 密钥
 *
 * 真机验证过的要点（/tmp/probe/dtls2.c）：
 *   - BIO_new_dgram 之后 **必须** BIO_CTRL_DGRAM_SET_PEER 设对端地址，
 *     否则 SSL_do_handshake 直接 -1 / errno=22（EINVAL）
 *   - 我们是 DTLS client ⇒ 收流用 server_write_*：key+salt = km[16..31]+km[46..59]
 *
 * 在真实 NAT 环境上额外要做的（首轮到设备全 WANT_READ 的教训）：
 *   - 关掉 PMTU 探测：BIO_CTRL_DGRAM_SET_MTU=1200 + SSL_OP_NO_QUERY_MTU。
 *     真实网卡上 PTB 回不来，开着 MTU discover 握手会一直等。
 *   - BIO_CTRL_DGRAM_SET_CONNECTED 也告诉 BIO 一次，防它 sendto 到空地址。
 *   - DTLS **不会自己重传**：必须用 DTLSv1_get_timeout / DTLSv1_handle_timeout
 *     驱动（设备上这两个是 ssl.h 里的宏，走 SSL_ctrl，符号本身不存在）。
 *   - 必须装证书：answer SDP 里的 a=fingerprint 要和实际证书一致，
 *     否则网关可能直接拒绝。证书内嵌在 cm_cert.h。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "cm_cert.h"
#include "cm_session.h"

#define KM_LEN 60
#define DTLS_MTU 1200
#define HANDSHAKE_BUDGET_MS 13000

/* 设备上的 libssl 已裁掉 DTLSv1_* / SSL_set_mtu 这些符号，
 * 它们其实只是 ssl.h 里的宏（走 SSL_ctrl）。这里给出兜底定义，
 * 防止某些头文件版本把它们藏在 #ifndef OPENSSL_NO_DTLS 里。 */
#ifndef DTLS_CTRL_GET_TIMEOUT
#define DTLS_CTRL_GET_TIMEOUT 73
#endif
#ifndef DTLS_CTRL_HANDLE_TIMEOUT
#define DTLS_CTRL_HANDLE_TIMEOUT 74
#endif
#ifndef DTLSv1_get_timeout
#define DTLSv1_get_timeout(ssl, arg) SSL_ctrl((ssl), DTLS_CTRL_GET_TIMEOUT, 0, (void *)(arg))
#endif
#ifndef DTLSv1_handle_timeout
#define DTLSv1_handle_timeout(ssl) SSL_ctrl((ssl), DTLS_CTRL_HANDLE_TIMEOUT, 0, NULL)
#endif
#ifndef SSL_OP_NO_QUERY_MTU
#define SSL_OP_NO_QUERY_MTU 0x00001000U
#endif
#ifndef BIO_CTRL_DGRAM_SET_CONNECTED
#define BIO_CTRL_DGRAM_SET_CONNECTED 32
#endif
#ifndef BIO_CTRL_DGRAM_SET_MTU
#define BIO_CTRL_DGRAM_SET_MTU 42
#endif

typedef struct {
    unsigned char km[KM_LEN];
    int have_km;

    /* 诊断计数（lead 要的「到底有没有收到字节」） */
    int in_msgs;
    int out_msgs;
    int in_bytes;
    unsigned char first_in[32];
    int first_in_len;
    int retx;

    /* 记录层诊断：读到多少条 13 字节记录头，其中多少条 content type 非法
     * （= 不是 DTLS，典型的 STUN keepalive 被灌进 OpenSSL） */
    int hdr_msgs;
    int bad_hdr;
    int filtered_stun; /* 握手期间被 cm_udp_prepare_dtls 清掉的 STUN 包数 */

    /* 指纹取证：把我们发出的 ClientHello 完整 hex dump 出来（只 dump 一次）。
     * 网关若按 DTLS 指纹风控，比对对象就是这串字节 vs Chrome 的 ClientHello。 */
    int ch_dumped;          /* 0=还没 dump 过 */
    unsigned char ch_hex[1024]; /* ClientHello 前 512 字节的 hex 文本 */
} cm_dtls_ctx;

static void log_ssl_errors(const char *what, int id) {
    unsigned long e;
    char first = 0;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        CM_LOGE("#%d %s: %s", id, what, buf);
        first = 1;
    }
    if (!first) CM_LOGE("#%d %s (no ssl error queued)", id, what);
}

/* 握手报文级别的收发日志：没有 msg_callback 就只能猜包发没发出去 */
static void msg_cb(int write_p, int version, int content_type, const void *buf, size_t len,
                   SSL *ssl, void *arg) {
    cm_sess *s = (cm_sess *)arg;
    static const char *ct_name[] = {"change_cipher_spec", "alert", "handshake",
                                    "application_data", "heartbeat"};
    const char *nm = "?";
    int htype = -1;
    cm_dtls_ctx *d;

    if (!s) return;
    d = (cm_dtls_ctx *)s->dtls_priv;
    if (content_type >= 0 && content_type <= 4) nm = ct_name[content_type];

    /* SSL3_RT_HEADER(0x100)：OpenSSL 每读到一条 DTLS 记录头就回调一次，
     * len 恒为 13。这是普通读路径（ssl3_record.c 的 dtls1_get_record），
     * 不是 DTLSv1_listen 专用。打印 13 字节 hex 是判断"到底收的是不是
     * 真 DTLS 记录"的唯一硬证据 —— 之前只打 len=13/type=-1，无法区分
     * 「记录头合法但 body 丢」与「STUN 被当记录头」。 */
    if (content_type == 0x100 /* SSL3_RT_HEADER */) {
        const unsigned char *h = (const unsigned char *)buf;
        if (buf && len >= 13) {
            int ep = (h[3] << 8) | h[4];
            int rl = (h[11] << 8) | h[12];
            if (!write_p && d) {
                d->hdr_msgs++;
                if (d->hdr_msgs <= 8) {
                    CM_LOGI("#%d DTLS hdr[%d] ct=%d ver=%04x epoch=%04x len=%d "
                            "raw=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                            s->id, d->hdr_msgs, h[0], (h[1] << 8) | h[2], ep, rl,
                            h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9],
                            h[10], h[11], h[12]);
                } else {
                    CM_LOGD("#%d DTLS hdr[%d] ct=%d ver=%04x epoch=%04x len=%d",
                            s->id, d->hdr_msgs, h[0], (h[1] << 8) | h[2], ep, rl);
                }
                /* 记录一个"最大垃圾头"样本用于对账：ct 不是 20..23 的都不是 DTLS */
                if (h[0] < 20 || h[0] > 23) {
                    d->bad_hdr++;
                    if (d->bad_hdr <= 4) {
                        CM_LOGW("#%d DTLS received NON-DTLS record content type %d "
                                "(13-byte header polluted the record layer)", s->id, h[0]);
                    }
                }
            }
            return;
        }
    }

    const unsigned char *buf_p = (const unsigned char *)buf;
    if (content_type == 22 /* handshake */ && buf && len >= 1) htype = buf_p[0];
    (void)version;
    (void)ssl;

    if (write_p) {
        if (d) d->out_msgs++;
        /* 指纹取证：把我们发出的 ClientHello（handshake type=1）整包 dump 成 hex。
         * 每个 DTLS 报文可能被 OpenSSL 分片回调多次，这里只记录第一片 + 总长。 */
        if (htype == 1 && d && !d->ch_dumped) {
            size_t n = len < sizeof(d->ch_hex) / 2 ? len : sizeof(d->ch_hex) / 2;
            static const char *hx = "0123456789abcdef";
            for (size_t i = 0; i < n; i++) {
                d->ch_hex[i * 2] = hx[(buf_p[i] >> 4) & 0xf];
                d->ch_hex[i * 2 + 1] = hx[buf_p[i] & 0xf];
            }
            d->ch_hex[n * 2] = '\0';
            d->ch_dumped = (int)len; /* 先记第一片长度 */
            CM_LOGI("#%d DTLS ClientHello first-frag len=%d total-frag=%d hex=%s",
                    s->id, (int)n, (int)len, d->ch_hex);
        } else if (htype == 1 && d && d->ch_dumped) {
            CM_LOGI("#%d DTLS ClientHello more-frag len=%d", s->id, (int)len);
        }
        CM_LOGD("#%d DTLS >> %s len=%d type=%d", s->id, nm, (int)len, htype);
    } else {
        if (d) {
            d->in_msgs++;
            d->in_bytes += (int)len;
            if (d->first_in_len == 0 && buf && len) {
                int n = (int)(len < sizeof(d->first_in) ? len : sizeof(d->first_in));
                memcpy(d->first_in, buf, (size_t)n);
                d->first_in_len = n;
            }
        }
        CM_LOGI("#%d DTLS << %s len=%d type=%d", s->id, nm, (int)len, htype);
    }
}

/*
 * 握手前把 socket 清干净。
 *
 * 老版本只 drain 32 次就退出，且不区分内容；实测网关在 ICE 连通后
 * 持续发 STUN keepalive，30 次 drain 之后照样有新包进来。现在改成
 * "一直清到 300ms 内没有新包"，并且顺手把 STUN 应答掉（keepalive 不回
 * 网关可能判定 ICE 断开）。DTLS 记录不消费，留给 OpenSSL。
 */
static void drain_socket(cm_sess *s) {
    cm_dtls_ctx *d = (cm_dtls_ctx *)s->dtls_priv;
    uint64_t quiet_deadline;
    unsigned char tmp[2048];
    char ip[64];
    int port;
    int guard;
    int drained = 0;

    for (guard = 0; guard < 512; guard++) {
        int n = cm_udp_recv(s->sock, tmp, sizeof(tmp), 0, ip, &port);
        if (n <= 0) break;
        if (cm_stun_is(tmp, n)) {
            cm_stun_handle(s, tmp, n, ip, port);
            if (d) d->filtered_stun++;
            CM_LOGD("#%d drained STUN %d bytes pre-DTLS (answered)", s->id, n);
        } else if (n >= 13 && tmp[0] >= 20 && tmp[0] <= 23 && tmp[1] == 0xFE) {
            /* 竟然已经有 DTLS 记录（网关主动发？）：停手，别把它吃了 */
            CM_LOGW("#%d DTLS record already queued pre-handshake (%d bytes), stopping drain",
                    s->id, n);
            break;
        } else {
            CM_LOGD("#%d drained junk %d bytes pre-DTLS", s->id, n);
        }
        drained++;
    }

    /* 再清到 300ms 静默，覆盖"网关 keepalive 正好在路上"的窗口 */
    quiet_deadline = cm_now_ms() + 300;
    while (cm_now_ms() < quiet_deadline) {
        int n;
        if (s->cancelled) break;
        n = cm_udp_prepare_dtls(s, 50); /* 分类 + 清 STUN/垃圾 */
        if (n == 1) {
            CM_LOGW("#%d DTLS data arrived while draining, leaving it for OpenSSL", s->id);
            break;
        }
        if (n < 0) break;
    }
    if (drained) CM_LOGI("#%d drained %d pre-DTLS datagrams", s->id, drained);
}

/*
 * 握手主循环。
 *
 * 不再"纯 sleep 600×20ms"：每轮先问 OpenSSL 下次重传还剩多久
 * （DTLSv1_get_timeout），用 poll() 等 socket 或等这个时长，
 * 到期就 DTLSv1_handle_timeout 让它重发 ClientHello。
 * 首包丢了也能自己救回来。
 */
static int dtls_handshake(SSL *ssl, cm_sess *s, int *out_err) {
    uint64_t deadline = cm_now_ms() + HANDSHAKE_BUDGET_MS;
    cm_dtls_ctx *d = (cm_dtls_ctx *)s->dtls_priv;

    *out_err = 0;
    for (;;) {
        int r, e;

        if (s->cancelled) { *out_err = -100; return 0; }
        if (cm_now_ms() > deadline) break;

        ERR_clear_error();
        r = SSL_do_handshake(ssl);
        if (r == 1) return 1;
        e = SSL_get_error(ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            *out_err = e;
            CM_LOGE("#%d handshake fail e=%d r=%d errno=%d(%s)", s->id, e, r, errno, strerror(errno));
            log_ssl_errors("handshake", s->id);
            return 0;
        }

        {
            struct timeval tv;
            int wait_ms = 200;
            int ready;
            if (DTLSv1_get_timeout(ssl, &tv)) {
                int ms = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
                wait_ms = (ms <= 0) ? 1 : ms;
                if (wait_ms > 200) wait_ms = 200;
            }
            /* 关键：这里不能直接 poll(sock) 然后让 OpenSSL 自己读。
             * 网关握手期间会持续发 STUN keepalive，那些包首字节 0x00，
             * 被 dtls1_get_record 当成非法 content type 静默丢弃，
             * 而 DTLS 定时器还在重传 ClientHello —— 结果就是
             * "in=14 in_bytes=182 全是 13 字节记录头、25 秒超时"。
             * cm_udp_prepare_dtls 把 STUN 应答掉、垃圾丢掉，
             * 只把真正的 DTLS 记录留给 OpenSSL。 */
            ready = cm_udp_prepare_dtls(s, wait_ms);
            if (ready < 0) {
                *out_err = -102;
                CM_LOGE("#%d socket error while waiting for DTLS", s->id);
                return 0;
            }
            if (ready == 0 && d && s->stats.rx_stun > 0) d->filtered_stun = (int)s->stats.rx_stun;
            if (DTLSv1_handle_timeout(ssl) > 0) {
                if (d) d->retx++;
                CM_LOGD("#%d DTLS timer expired -> retransmit #%d", s->id, d ? d->retx : -1);
            }
        }
    }

    *out_err = -101;
    CM_LOGE("#%d handshake timeout after %d ms", s->id, HANDSHAKE_BUDGET_MS);
    if (d) {
        CM_LOGE("#%d handshake diag: out=%d in=%d (in_bytes=%d) retx=%d "
                "hdr=%d badHdr=%d stunFiltered=%d rxPkts=%llu rxStun=%llu rxJunk=%llu",
                s->id, d->out_msgs, d->in_msgs, d->in_bytes, d->retx,
                d->hdr_msgs, d->bad_hdr, (int)s->stats.rx_stun,
                (unsigned long long)s->stats.rx_pkts,
                (unsigned long long)s->stats.rx_stun,
                (unsigned long long)s->stats.rx_other);
    }
    return 0;
}

/* 把内嵌的 PEM 证书/私钥装到 ctx 上。
 * 装不上也不致命（SSL_VERIFY_NONE），但会缺证书 ⇒ 网关可能拒绝。 */
static int load_cert(SSL_CTX *ctx, int id) {
    BIO *cb = NULL, *kb = NULL;
    X509 *x = NULL;
    EVP_PKEY *k = NULL;
    int ok = 0;

    cb = BIO_new_mem_buf((void *)cm_cert_pem, -1);
    if (!cb) { log_ssl_errors("BIO_new_mem_buf(cert)", id); goto done; }
    x = PEM_read_bio_X509(cb, NULL, NULL, NULL);
    if (!x) { log_ssl_errors("PEM_read_bio_X509", id); goto done; }

    kb = BIO_new_mem_buf((void *)cm_key_pem, -1);
    if (!kb) { log_ssl_errors("BIO_new_mem_buf(key)", id); goto done; }
    k = PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL);
    if (!k) { log_ssl_errors("PEM_read_bio_PrivateKey", id); goto done; }

    if (SSL_CTX_use_certificate(ctx, x) != 1) { log_ssl_errors("SSL_CTX_use_certificate", id); goto done; }
    if (SSL_CTX_use_PrivateKey(ctx, k) != 1) { log_ssl_errors("SSL_CTX_use_PrivateKey", id); goto done; }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        CM_LOGW("#%d cert/private key mismatch", id);
    }
    ok = 1;

done:
    if (x) X509_free(x);
    if (k) EVP_PKEY_free(k);
    if (cb) BIO_free(cb);
    if (kb) BIO_free(kb);
    return ok ? 0 : -1;
}

int cm_dtls_run(cm_sess *s) {
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    BIO *bio = NULL;
    cm_dtls_ctx *d = NULL;
    int ok = 0;
    int err = 0;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    d = (cm_dtls_ctx *)calloc(1, sizeof(*d));
    if (!d) return -1;
    s->dtls_priv = d; /* msg_cb 要用，先挂上 */

    drain_socket(s);

    /* 关键：OpenSSL 的 dgram BIO 写记录走 sendmsg/sendmmsg，未 connect 的
     * UDP socket 上 msg_name 为 NULL，内核直接返回 EDESTADDRREQ(89)。
     * BIO_CTRL_DGRAM_SET_PEER 只记住地址，不能替代 connect()。
     * 实测设备上表现为：ClientHello 的 13+203 字节写出后立刻
     *   "handshake fail e=5 r=-1 errno=89(Destination address required)"。
     * ICE 已结束、peer 已定稿，这里连接它。 */
    if (cm_udp_connect_peer(s) != 0) {
        CM_LOGE("#%d cannot connect UDP peer, DTLS would fail to send", s->id);
        goto done;
    }

    ctx = SSL_CTX_new(DTLS_client_method());
    if (!ctx) { log_ssl_errors("SSL_CTX_new", s->id); goto done; }
    if (SSL_CTX_set_tlsext_use_srtp(ctx, "SRTP_AES128_CM_SHA1_80") != 0) {
        CM_LOGW("#%d set_tlsext_use_srtp returned non-zero", s->id);
    }
    /* A/B 实验：chromeFp=1 时把 cipher_list 换成 Chrome 序（TLS1.2 现代
     * 套件、无静态 RSA）。对照 docs/DTLS-CLIENTHELLO-FINGERPRINT.md：
     * 我们默认 28 个 OpenSSL 套件含 c00a/c014/0039/0033/0035/002f/00ff，
     * Chrome 只发 ~15 个现代套件且顺序不同。扩展集合与 use_srtp 不动。 */
    if (s->cfg.chrome_fp &&
        SSL_CTX_set_cipher_list(ctx,
            "ECDHE-ECDSA-CHACHA20-POLY1305:"
            "ECDHE-RSA-CHACHA20-POLY1305:"
            "ECDHE-ECDSA-AES128-GCM-SHA256:"
            "ECDHE-RSA-AES128-GCM-SHA256:"
            "ECDHE-ECDSA-AES256-GCM-SHA384:"
            "ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-AES128-SHA:"
            "ECDHE-RSA-AES128-SHA:"
            "ECDHE-ECDSA-AES256-SHA:"
            "ECDHE-RSA-AES256-SHA:"
            "AES128-GCM-SHA256:"
            "AES256-GCM-SHA384:"
            "AES128-SHA:"
            "AES256-SHA") == 0) {
        CM_LOGW("#%d chrome_fp set_cipher_list failed, keep default", s->id);
    } else if (s->cfg.chrome_fp) {
        CM_LOGI("#%d chrome_fp cipher list applied (14 suites, Chrome order)", s->id);
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_read_ahead(ctx, 1); /* DTLS 必须，否则同一 datagram 里的多报文会丢 */
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    SSL_CTX_set_msg_callback(ctx, msg_cb);
    SSL_CTX_set_msg_callback_arg(ctx, s);

    if (load_cert(ctx, s->id) != 0) {
        CM_LOGW("#%d running without a client certificate", s->id);
    }

    ssl = SSL_new(ctx);
    if (!ssl) { log_ssl_errors("SSL_new", s->id); goto done; }

    bio = BIO_new_dgram(s->sock, BIO_NOCLOSE);
    if (!bio) { log_ssl_errors("BIO_new_dgram", s->id); goto done; }
    BIO_set_nbio(bio, 1);

    /* 关键 1：不设对端 ⇒ 握手 -1 errno=22 */
    if (BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_PEER, 0, &s->peer) <= 0) {
        CM_LOGE("#%d BIO_CTRL_DGRAM_SET_PEER failed", s->id);
        goto done;
    }
    /* 关键 2：告诉 BIO 这条 dgram 已 connected */
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &s->peer);
    /* 关键 3：写死 MTU，别做 PMTU 探测（真机 NAT 上 PTB 永远回不来） */
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_MTU, DTLS_MTU, NULL);

    SSL_set_bio(ssl, bio, bio);
    bio = NULL; /* 归 ssl 所有 */
    SSL_set_connect_state(ssl);
    SSL_set_options(ssl, SSL_OP_NO_QUERY_MTU);
    /* 关键 4：SSL_OP_NO_QUERY_MTU 之后 OpenSSL 不再自行问 BIO 要 MTU，
     * 必须显式喂一个，否则 s->d1->mtu 保持 0，写第一条记录就
     * SSL_do_handshake 返回 -1 / SSL_ERROR_SYSCALL(5) / errno=0。
     * SSL_set_mtu 只是宏（SSL_ctrl SSL_CTRL_SET_MTU=17），符号存在。 */
    SSL_set_mtu(ssl, DTLS_MTU);

    if (!dtls_handshake(ssl, s, &err)) goto done;

    {
        const SRTP_PROTECTION_PROFILE *p = SSL_get_selected_srtp_profile(ssl);
        if (p && p->name) {
            snprintf(s->srtp_profile, sizeof(s->srtp_profile), "%s", p->name);
        } else {
            snprintf(s->srtp_profile, sizeof(s->srtp_profile), "SRTP_AES128_CM_SHA1_80");
        }
        CM_LOGI("#%d DTLS OK version=%s cipher=%s srtp=%s", s->id,
                SSL_get_version(ssl), SSL_get_cipher(ssl), s->srtp_profile);
    }

    /* RFC 5764 §4.2: exporter label 是字面量 "EXTRACTOR-dtls_srtp"，
     * 长度必须是 strlen(label) == 19。早先这里误写成 21（把字符串结尾的
     * NUL 又多算了 2 字节），label_len 直接进 TLS 1.2 PRF 的 seed，
     * 差一个字节导出的 keying material 就完全不同 —— 表现为 DTLS 握手正常、
     * KM 正常导出、但网关的每个 SRTP 包 HMAC 都验不过。
     * 用 sizeof-1 而不是硬编码常量，避免再出现同类错误。 */
    if (SSL_export_keying_material(ssl, d->km, KM_LEN, "EXTRACTOR-dtls_srtp",
                                   sizeof("EXTRACTOR-dtls_srtp") - 1,
                                   NULL, 0, 0) != 1) {
        CM_LOGE("#%d SSL_export_keying_material failed", s->id);
        log_ssl_errors("export_keying_material", s->id);
        goto done;
    }
    d->have_km = 1;
    {
        int i;
        char hex[KM_LEN * 3 + 4];
        int o = 0;
        for (i = 0; i < KM_LEN && o < (int)sizeof(hex) - 3; i++)
            o += snprintf(hex + o, sizeof(hex) - (size_t)o, "%02x", d->km[i]);
        CM_LOGI("#%d keying material (%d bytes) = %s", s->id, KM_LEN, hex);
    }
    ok = 1;

done:
    if (bio) BIO_free(bio);
    if (ssl) {
        SSL_free(ssl); /* 会连带释放 BIO */
    }
    if (ctx) SSL_CTX_free(ctx);
    if (!ok) {
        free(d);
        s->dtls_priv = NULL;
    }
    return ok ? 0 : -1;
}

/* 供 cm_srtp.c 取 keying material */
int cm_dtls_get_km(cm_sess *s, unsigned char *km_out, int cap) {
    cm_dtls_ctx *d = (cm_dtls_ctx *)s->dtls_priv;
    if (!d || !d->have_km || cap < KM_LEN) return -1;
    memcpy(km_out, d->km, KM_LEN);
    return KM_LEN;
}

void cm_dtls_free(cm_sess *s) {
    if (s->dtls_priv) {
        free(s->dtls_priv);
        s->dtls_priv = NULL;
    }
}

/* 我方证书的 sha-256 指纹，供 answer SDP 的 a=fingerprint 使用 */
/*
 * 把握手期记录层诊断暴露给测试工具（dselftest / cgmtest）。
 * hdr   = OpenSSL 回调到的 13 字节记录头数量
 * bad   = 其中 content type 不是 20..23 的数量（>0 说明非 DTLS 报文
 *         还是漏进了记录层，过滤没生效）
 * 过滤生效时 bad 应该恒为 0，而 STUN keepalive 只体现在 stats.rx_stun。
 */
void cm_dtls_diag(cm_sess *s, int *hdr, int *bad, int *retx,
                  int *in_msgs, int *in_bytes, int *stun_filtered) {
    cm_dtls_ctx *d = s ? (cm_dtls_ctx *)s->dtls_priv : NULL;
    if (hdr) *hdr = d ? d->hdr_msgs : 0;
    if (bad) *bad = d ? d->bad_hdr : 0;
    if (retx) *retx = d ? d->retx : 0;
    if (in_msgs) *in_msgs = d ? d->in_msgs : 0;
    if (in_bytes) *in_bytes = d ? d->in_bytes : 0;
    if (stun_filtered) *stun_filtered = d ? d->filtered_stun : 0;
}

const char *cm_dtls_fingerprint(void) { return CM_CERT_FINGERPRINT; }
