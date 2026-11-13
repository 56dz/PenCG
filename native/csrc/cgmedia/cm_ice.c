/*
 * cm_ice.c —— 最小 STUN/ICE（照抄 .tmp/cgprobe/ice4.mjs 的真机验证字节布局）
 *
 * 网关是 ice-lite：我们只要往它的 host candidate 发一个带认证的
 * Binding Request，收到 0x0101 Binding Success Response 就算连通，
 * 之后立刻停发 STUN、转 DTLS（不再做 connectivity checklist）。
 *
 * 关键细节（都是踩过的坑）：
 *   - USERNAME 必须 "<remote_ufrag>:<our_ufrag>"（offer 的 ufrag 在前）
 *   - MESSAGE-INTEGRITY 的 HMAC 覆盖 header(此时 length 已含 MI 24 字节) + body
 *   - FINGERPRINT = (CRC32(header+body) ^ 0x5354554e)，此时 length 再 +8
 *   - 最终 length 字段必须把 MI 和 FP 都算进去，否则网关丢弃
 */
#include <arpa/inet.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm_session.h"

#define STUN_MAGIC 0x2112A442u
#define STUN_HDR 20

/* ---------------- CRC32 (IEEE 802.3, 反射, poly 0xEDB88320) ---------------- */
static uint32_t crc32_ieee(const unsigned char *b, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < n; i++) {
        int k;
        c ^= b[i];
        for (k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

/* 追加一个 STUN 属性（4 字节头 + 值 + 4 字节对齐 padding） */
static int stun_attr(cm_buf *b, uint16_t type, const void *val, size_t n) {
    unsigned char hdr[4];
    static const unsigned char pad[4] = {0, 0, 0, 0};
    size_t padn = (4 - (n % 4)) % 4;
    cm_wr16(hdr, type);
    cm_wr16(hdr + 2, (uint16_t)n);
    if (cm_buf_put(b, hdr, 4) < 0) return -1;
    if (n && cm_buf_put(b, val, n) < 0) return -1;
    if (padn && cm_buf_put(b, pad, padn) < 0) return -1;
    return 0;
}

/*
 * 拼一个完整 STUN 消息。
 * 若 key 非空则追加 MESSAGE-INTEGRITY(0x0008) + FINGERPRINT(0x8028)。
 */
static int stun_build(uint16_t type, const cm_buf *attrs, const unsigned char *txn,
                      const char *key, cm_buf *out) {
    unsigned char hdr[STUN_HDR];
    unsigned char md[20];
    unsigned int mdlen = sizeof(md);

    cm_buf_reset(out);
    memset(hdr, 0, sizeof(hdr));
    cm_wr16(hdr, type);
    cm_wr16(hdr + 2, (uint16_t)attrs->len);
    cm_wr32(hdr + 4, STUN_MAGIC);
    memcpy(hdr + 8, txn, 12);
    if (cm_buf_put(out, hdr, STUN_HDR) < 0) return -1;
    if (attrs->len && cm_buf_put(out, attrs->p, attrs->len) < 0) return -1;

    if (key && key[0]) {
        /* length 先设为 body + MI(24)，因为 HMAC 覆盖的 header 里已经是这个值 */
        cm_wr16(out->p + 2, (uint16_t)(out->len - STUN_HDR + 24));
        if (!HMAC(EVP_sha1(), key, (int)strlen(key), out->p, out->len, md, &mdlen)) return -1;
        if (stun_attr(out, 0x0008, md, 20) < 0) return -1;
        /* 现在 length 要含 FINGERPRINT 的 8 字节，FP 覆盖这个 header */
        cm_wr16(out->p + 2, (uint16_t)(out->len - STUN_HDR + 8));
        {
            uint32_t fp = crc32_ieee(out->p, out->len) ^ 0x5354554eu;
            unsigned char fpb[4];
            cm_wr32(fpb, fp);
            if (stun_attr(out, 0x8028, fpb, 4) < 0) return -1;
        }
    }
    cm_wr16(out->p + 2, (uint16_t)(out->len - STUN_HDR));
    return 0;
}

/* 在 STUN 报文里找 XOR-MAPPED-ADDRESS 与 ERROR-CODE */
static void stun_parse(const unsigned char *b, int n, char *mapped_ip, int *mapped_port,
                       int *err_code) {
    int len, o;
    if (mapped_ip) mapped_ip[0] = '\0';
    if (mapped_port) *mapped_port = 0;
    if (err_code) *err_code = 0;
    if (n < STUN_HDR) return;
    len = cm_rd16(b + 2);
    o = STUN_HDR;
    while (o + 4 <= STUN_HDR + len && o + 4 <= n) {
        uint16_t t = cm_rd16(b + o);
        int l = cm_rd16(b + o + 2);
        const unsigned char *v = b + o + 4;
        if (o + 4 + l > n) break;
        if (t == 0x0020 && l >= 8) {
            int port = cm_rd16(v + 2) ^ 0x2112;
            unsigned char ip[4];
            int i;
            for (i = 0; i < 4; i++) ip[i] = v[4 + i] ^ (unsigned char)((STUN_MAGIC >> ((3 - i) * 8)) & 0xff);
            if (mapped_ip) snprintf(mapped_ip, 64, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            if (mapped_port) *mapped_port = port;
        } else if (t == 0x0009 && l >= 4) {
            /* RFC 5389 §15.6：ERROR-CODE 值 = class*100 + number。
             * v[2] 低 3 位是 class（3=3xx/4=4xx/5=5xx），v[3] 是 number。
             * 原来写成 300 + class*100 + number，把 487 打成 787，误导排查。 */
            if (err_code) *err_code = (v[2] & 0x07) * 100 + (v[3] & 0xff);
        }
        o += 4 + l + ((4 - (l % 4)) % 4);
    }
}

/*
 * 一次 Binding Request。
 *
 * 网关是 ice-lite（controlled），我们是 controlling：**必须**带
 * USE-CANDIDATE(0x0025, len=0) 它才会把这个 pair 置为 nominated 并开始
 * 监听 DTLS。只发 PRIORITY + ICE-CONTROLLING 只能拿到 0x0101 回应，
 * pair 不会提名 —— 这正是"STUN 通了但 DTLS 一个字节都收不到"的原因。
 */
static int ice_send_binding(cm_sess *s, const char *key, int with_change_req, int as_controlled) {
    cm_buf attrs, msg;
    unsigned char txn[12];
    char user[160];
    int rc = -1;
    int i;

    for (i = 0; i < 12; i++) txn[i] = (unsigned char)(rand() & 0xff);

    cm_buf_init(&attrs);
    /* USERNAME = "<remote_ufrag>:<our_ufrag>"，offer 的在前。
     * 我方 ufrag 由调用方保证非空（cm_ice_run 里兜底生成）。 */
    snprintf(user, sizeof(user), "%s:%s", s->cfg.remote_ufrag, s->cfg.ufrag);
    if (stun_attr(&attrs, 0x0006, user, strlen(user)) < 0) goto done;
    {
        unsigned char prio[4];
        cm_wr32(prio, 1845501695u);
        if (stun_attr(&attrs, 0x0024, prio, 4) < 0) goto done;
    }
    {
        /* ICE-CONTROLLING = 0x8029，ICE-CONTROLLED = 0x802A（RFC 8445 §7.1.2）。
         * 网关回 487 Role Conflict 说明它认为自己也该 controlling（部分媒体服
         * 不是 ice-lite），此时按 §7.2.5.1 切到 controlled 再试一次。 */
        unsigned char ctrl[8];
        cm_wr32(ctrl, 0x7F000000u);
        cm_wr32(ctrl + 4, 0x11223344u);
        if (stun_attr(&attrs, as_controlled ? 0x802A : 0x8029, ctrl, 8) < 0) goto done;
    }
    /* USE-CANDIDATE：ice-lite 网关靠它提名 pair，缺了它只回 0x0101 不开 DTLS。
     * controlled 角色不该提名（提名是 controlling 的职责），切角后不再发。 */
    if (!as_controlled) {
        if (stun_attr(&attrs, 0x0025, NULL, 0) < 0) goto done;
    }
    if (with_change_req) {
        if (stun_attr(&attrs, 0x000C, NULL, 0) < 0) goto done;
    }

    cm_buf_init(&msg);
    if (stun_build(0x0001, &attrs, txn, key, &msg) < 0) { cm_buf_free(&msg); goto done; }
    rc = cm_udp_send(s->sock, msg.p, (int)msg.len, s->cfg.remote_ip, s->cfg.remote_port);
    cm_buf_free(&msg);
done:
    cm_buf_free(&attrs);
    return rc;
}

/* =====================================================================
 * ICE consent refresh（RFC 8445 §11）
 *
 * 连通后网关（ice-lite）会周期发 Binding Request 给我们，我们在
 * cm_stun_handle 里回 0x0101 —— 那只证明「它问、我们答」。consent 是反方向：
 * **由我们主动证明自己还活着**。
 *
 * 浏览器侧真机实测（2026-11-13，20×5s getStats 采样）：
 *   candidate-pair.consentRequestsSent  5 → 40   ≈ 每 2.5s 一次，单调递增
 *   candidate-pair.responsesReceived    6 → 41   网关每次都回
 * 同时 inbound-rtp.pliCount / nackCount 全程为 0，remote-inbound-rtp 缺失，
 * 说明网关**不靠 RTCP 反馈判活**，唯一的周期双向报文就是这个 consent。
 *
 * 我们在 cm_ice_run 成功后立刻停发 STUN（见本文件头注释），因此这是与
 * 官方客户端最大的行为差异。bug1（约 4m18s 后网关单方面停推 RTP、
 * WS 仍 open、无任何 error/close op）的第一优先级修复就是补上它。
 *
 * 报文形态与初次连通完全一致：Binding Request + USERNAME + PRIORITY
 * + ICE-CONTROLLING + USE-CANDIDATE + MESSAGE-INTEGRITY + FINGERPRINT。
 * 不复用 with_change_req（CHANGE-REQUEST 只在探路阶段交替试）。
 * ===================================================================== */
#define ICE_CONSENT_INTERVAL_MS 2500
#define ICE_CONSENT_LOG_INTERVAL_MS 30000

void cm_ice_consent_tick(cm_sess *s) {
    uint64_t now;

    if (!s || s->sock < 0) return;
    if (s->ice_state != CM_ICE_CONNECTED) return;
    /* 凭据缺失说明还没走到 ICE，或有别的问题；不要发无效包打日志噪音 */
    if (s->cfg.pwd[0] == '\0' || s->cfg.remote_ufrag[0] == '\0') return;

    now = cm_now_ms();
    /* next==0 表示第一次进来：立刻发一次，然后按周期走 */
    if (s->next_ice_consent_ms && now < s->next_ice_consent_ms) return;
    s->next_ice_consent_ms = now + ICE_CONSENT_INTERVAL_MS;

    /* 与初次连通同形：沿用 cm_ice_run 最终协商的角色（多数网关 0=controlling，
     * 部分媒体服回 487 后切到 1=controlled）。角色不一致会让网关把 consent
     * 当成非法提名，宁可完全不发也不能发错。 */
    if (ice_send_binding(s, s->cfg.pwd, /*with_change_req=*/0, s->ice_controlled) <= 0) {
        CM_LOGW("#%d ICE consent send failed", s->id);
        return;
    }
    s->ice_consent_count++;

    /* 首条必打（确认通道真的起来了），之后 30s 一条，免得把 PLI/丢包埋掉 */
    if (s->ice_consent_log_ms == 0 ||
        now - s->ice_consent_log_ms >= ICE_CONSENT_LOG_INTERVAL_MS) {
        s->ice_consent_log_ms = now;
        CM_LOGI("#%d ICE consent sent (#%llu, every %dms) resp=%llu rxStun=%llu",
                s->id, (unsigned long long)s->ice_consent_count, ICE_CONSENT_INTERVAL_MS,
                (unsigned long long)s->stats.rx_consent_resp,
                (unsigned long long)s->stats.rx_stun);
    }
}

int cm_ice_run(cm_sess *s) {
    int attempt;
    const int max_attempts = 10;
    char mapped_ip[64];
    int mapped_port = 0;
    char from_ip[64];
    int from_port = 0;
    unsigned char rbuf[2048];
    /* ICE 角色：0=controlling（默认，ice-lite 网关要求），1=controlled。
     * 网关回 487 Role Conflict 时按 RFC 8445 §7.2.5.1 切角重试。 */
    int controlled = 0;

    if (cm_udp_open(s) != 0) return -1;

    /* 后端可能只给对端 ufrag 而不给我方（ice-lite 网关不校验我方 ufrag，
     * 但 USERNAME 必须是两段），这里兜底生成一个 8 字符的。 */
    if (s->cfg.ufrag[0] == '\0') {
        snprintf(s->cfg.ufrag, sizeof(s->cfg.ufrag), "%08x", (unsigned)(rand() ^ (int)cm_now_ms()));
        CM_LOGW("#%d our ufrag missing, generated '%s'", s->id, s->cfg.ufrag);
    }
    if (s->cfg.remote_ufrag[0] == '\0') {
        CM_LOGE("#%d remote ufrag missing, STUN cannot authenticate", s->id);
        return -1;
    }
    if (s->cfg.pwd[0] == '\0') {
        CM_LOGE("#%d remote ice-pwd missing, STUN cannot authenticate", s->id);
        return -1;
    }

    for (attempt = 0; attempt < max_attempts; attempt++) {
        int waited;
        if (s->cancelled) return -1;
        /* 前几次带 CHANGE-REQUEST 交替，和 ice4.mjs 的探针行为一致 */
        ice_send_binding(s, s->cfg.pwd, (attempt % 2) == 0 && !controlled, controlled);
        CM_LOGD("#%d STUN binding #%d sent", s->id, attempt + 1);

        /* 等最多 300ms，期间任何 STUN 0x0101 都算成功 */
        for (waited = 0; waited < 300; waited += 20) {
            int n;
            if (s->cancelled) return -1;
            n = cm_udp_recv(s->sock, rbuf, sizeof(rbuf), 20, from_ip, &from_port);
            if (n <= 0) continue;
            s->stats.rx_pkts++;
            if (cm_stun_is(rbuf, n)) {
                s->stats.rx_stun++;
                uint16_t type = cm_rd16(rbuf);
                if (type == 0x0101) {
                    int err = 0;
                    stun_parse(rbuf, n, mapped_ip, &mapped_port, &err);
                    CM_LOGI("#%d STUN Binding Success from %s:%d mapped=%s:%d",
                            s->id, from_ip, from_port, mapped_ip, mapped_port);
                    /* 如果网关从别的地址回，后续 DTLS 要发到那个地址 */
                    if (strcmp(from_ip, s->cfg.remote_ip) != 0 || from_port != s->cfg.remote_port) {
                        CM_LOGW("#%d responder %s:%d != cfg %s:%d, switching peer",
                                s->id, from_ip, from_port, s->cfg.remote_ip, s->cfg.remote_port);
                        s->peer.sin_port = htons((uint16_t)from_port);
                        inet_pton(AF_INET, from_ip, &s->peer.sin_addr);
                    }
                    cm_emit(s, "{\"t\":\"ice\",\"state\":\"connected\",\"localPort\":%d,\"mapped\":\"%s:%d\"}",
                            s->local_port, mapped_ip, mapped_port);
                    /* 补发 2 次带 USE-CANDIDATE 的 binding，确保网关把 pair
                     * 置成 nominated（有的网关第一次只当 connectivity check）。 */
                    {
                        int k;
                        for (k = 0; k < 2; k++) {
                            ice_send_binding(s, s->cfg.pwd, 0, controlled);
                            CM_LOGD("#%d USE-CANDIDATE confirm #%d sent", s->id, k + 1);
                            cm_sleep_ms(100);
                        }
                    }
                    /* 记下最终角色：会话期 consent 必须沿用（见 cm_ice_consent_tick） */
                    s->ice_controlled = controlled;
                    s->next_ice_consent_ms = cm_now_ms() + ICE_CONSENT_INTERVAL_MS;
                    return 0;
                } else if (type == 0x0111) {
                    int err = 0;
                    stun_parse(rbuf, n, NULL, NULL, &err);
                    CM_LOGW("#%d STUN error response code=%d (role=%s)",
                            s->id, err, controlled ? "controlled" : "controlling");
                    /* 487 Role Conflict：网关不认我们 controlling（它不是 ice-lite，
                     * 或已被别的会话占成 controlling）。按 RFC 8445 §7.2.5.1 切到
                     * controlled 重试，不再发 USE-CANDIDATE（提名是 controlling 的职责）。
                     * 这条只在部分媒体服出现：同一账号换到另一台网关就正常，所以
                     * 必须兼容，不能直接失败。 */
                    if (err == 487 && !controlled) {
                        controlled = 1;
                        CM_LOGW("#%d 487 role conflict, switching to controlled and retrying", s->id);
                        goto next_attempt;
                    }
                    cm_emit(s, "{\"t\":\"error\",\"msg\":\"stun error %d\",\"code\":%d}", err, err);
                    return -1;
                } else {
                    CM_LOGD("#%d STUN type=0x%04x from %s:%d", s->id, type, from_ip, from_port);
                }
            } else {
                CM_LOGD("#%d non-STUN %d bytes from %s:%d", s->id, n, from_ip, from_port);
            }
        }
        continue; /* 本轮没成功，下一轮 */
next_attempt:;
        /* 487 切角后立刻进入下一轮，不等满 300ms */
    }
    CM_LOGE("#%d STUN no response after %d attempts", s->id, max_attempts);
    return -1;
}

/* =====================================================================
 * STUN 判定 + keepalive 应答
 *
 * ICE 连通后网关会持续发 Binding Request（ice-lite 的保活）。这些包如果
 * 灌给 OpenSSL 会被当成非法 DTLS 记录静默丢弃，导致握手永远等不到
 * ServerHello —— 见 cm_net.c 顶部注释。所以必须：
 *   1. 识别出来（cm_stun_is）
 *   2. **回一个 Binding Success Response**，否则网关可能判定 ICE 断了
 *      转而停发媒体 / 踢会话
 * ===================================================================== */

int cm_stun_is(const unsigned char *pkt, int n) {
    if (!pkt || n < STUN_HDR) return 0;
    if ((pkt[0] & 0xC0) != 0x00) return 0;
    return cm_rd32(pkt + 4) == STUN_MAGIC;
}

/*
 * 处理一个入站 STUN 消息。
 * 返回 1=已处理（是 STUN）0=不是 STUN <0=错误
 */
int cm_stun_handle(cm_sess *s, const unsigned char *pkt, int n,
                   const char *from_ip, int from_port) {
    cm_buf attrs, msg;
    unsigned short type;
    unsigned char mapped[8];
    int rc = 0;
    uint32_t map_ip;
    uint16_t map_port;

    (void)from_ip;
    (void)from_port;
    if (!cm_stun_is(pkt, n)) return 0;

    type = cm_rd16(pkt);
    /* 我们只在意 Binding Request（0x0001）；响应（0x0101/0x0111）在 ICE
     * 阶段已由 cm_ice_run 消费，媒体期收到就忽略。 */
    if (type != 0x0001) {
        /* ★ 媒体期收到 0x0101 = 网关回了我们主动发的 ICE consent。
         * 这是判定「consent 通道是否真的双向可用」的唯一证据，必须计数，
         * 否则我们只能看到自己发出去了、不知道有没有人理。 */
        if (type == 0x0101) {
            s->stats.rx_consent_resp++;
            CM_LOGD("#%d consent response received (#%llu ok / #%llu sent)",
                    s->id, (unsigned long long)s->stats.rx_consent_resp,
                    (unsigned long long)s->ice_consent_count);
        } else {
            CM_LOGD("#%d STUN type=0x%04x (ignored in keepalive path)", s->id, type);
        }
        return 1;
    }

    /* 回 Binding Success Response：
     *   20 字节头 + XOR-MAPPED-ADDRESS(0x0020) + MESSAGE-INTEGRITY + FINGERPRINT
     * transaction id 原样回填（RFC 5389 §7.3.1）。
     * 网关不校验我们能回什么内容，只要它看到响应就认为链路活着。 */
    /* XOR-MAPPED-ADDRESS 的 8 字节布局（RFC 5389 §15.2）：
     *   [0]=保留 0  [1]=family 0x01  [2..3]=port^0x2112  [4..7]=addr^magic
     * 地址按网络序逐字节与 cookie 的 4 个字节（21 12 A4 42）异或。 */
    map_ip = s->peer.sin_addr.s_addr;
    map_port = ntohs(s->peer.sin_port);
    {
        unsigned char xip[4];
        const unsigned char *m = (const unsigned char *)&map_ip;
        int i;
        for (i = 0; i < 4; i++)
            xip[i] = m[i] ^ (unsigned char)((STUN_MAGIC >> ((3 - i) * 8)) & 0xff);
        mapped[0] = 0;
        mapped[1] = 0x01;
        cm_wr16(mapped + 2, (uint16_t)(map_port ^ 0x2112));
        memcpy(mapped + 4, xip, 4);
    }

    cm_buf_init(&attrs);
    if (stun_attr(&attrs, 0x0020, mapped, 8) < 0) { cm_buf_free(&attrs); return -1; }
    /* 响应的 MESSAGE-INTEGRITY 必须用「网关知道的那个密码」，也就是我们
     * 在 answer 里公布的 ice-pwd（RFC 8445 §7.2.2：响应用与请求相同的
     * key）。cfg.pwd 是网关的密码，只在 our_pwd 缺失时兜底。 */
    cm_buf_init(&msg);
    if (stun_build(0x0101, &attrs, pkt + 8,
                   s->cfg.our_pwd[0] ? s->cfg.our_pwd : s->cfg.pwd, &msg) < 0) {
        cm_buf_free(&msg);
        cm_buf_free(&attrs);
        return -1;
    }
    if (cm_udp_send_peer(s, msg.p, (int)msg.len) < 0) {
        CM_LOGW("#%d STUN keepalive reply send failed", s->id);
        rc = -1;
    } else {
        s->stats.rx_kept++;
        CM_LOGD("#%d STUN keepalive reply sent (%d bytes, #%llu)", s->id, (int)msg.len,
                (unsigned long long)s->stats.rx_kept);
        rc = 1;
    }
    cm_buf_free(&msg);
    cm_buf_free(&attrs);
    return rc;
}
