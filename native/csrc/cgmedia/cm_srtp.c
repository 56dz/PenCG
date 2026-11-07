/*
 * cm_srtp.c —— SRTP 解密（AES128_CM_SHA1_80）
 *
 * aes_cm_prf / ctr_xor / HMAC 校验三件事都照抄真机 PASS 的 /tmp/probe/dtls2.c。
 *
 * KDF（RFC 3711 §4.3）：
 *   key  = PRF(master_key, master_salt, 0x00)
 *   auth = PRF(master_key, master_salt, 0x01)
 *   salt = PRF(master_key, master_salt, 0x02)
 *
 * 我们是 DTLS client ⇒ 解密入站流用 **server_write_***：
 *   master_key  = km[16..31]
 *   master_salt = km[46..59]
 *
 * 解密：CTR 模式（IV 由 salt ^ SSRC ^ index 得到），
 * 校验：HMAC-SHA1(master_key||salt 派生的 auth_key) 覆盖 RTP 头 + 密文 + 4 字节 ROC，
 *       取前 10 字节（SHA1_80）。
 */
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cm_session.h"

typedef struct {
    unsigned char key[16];
    unsigned char salt[14];
    unsigned char auth[20];
} srtp_ctx;

typedef struct {
    srtp_ctx rx;   /* 入站：server_write_* */
    srtp_ctx tx;   /* 出站（SRTCP PLI）：client_write_* */
    int ready;
    uint32_t srtcp_index;
    /* 每个 SSRC 独立维护 ROC / 最大序号 */
    struct {
        uint32_t ssrc;
        uint32_t roc;
        uint16_t max_seq;
        int inited;
    } streams[8];
    int nstreams;
    pthread_mutex_t mu;
} cm_srtp_state;

/* ---------------- 已验证的原语（照抄 dtls2.c） ---------------- */

static void aes_cm_prf(const unsigned char *mk, const unsigned char *ms, int label,
                       unsigned char *out, int len) {
    unsigned char iv[16];
    memset(iv, 0, 16);
    memcpy(iv, ms, 14);
    iv[7] ^= (unsigned char)label;
    {
        int done = 0;
        unsigned short ctr = 0;
        while (done < len) {
            unsigned char blk[16];
            int ol = 0;
            EVP_CIPHER_CTX *e;
            int n;
            iv[14] = (unsigned char)(ctr >> 8);
            iv[15] = (unsigned char)ctr;
            e = EVP_CIPHER_CTX_new();
            if (!e) return;
            EVP_EncryptInit_ex(e, EVP_aes_128_ecb(), NULL, mk, NULL);
            EVP_CIPHER_CTX_set_padding(e, 0);
            EVP_EncryptUpdate(e, blk, &ol, iv, 16);
            EVP_CIPHER_CTX_free(e);
            n = len - done;
            if (n > 16) n = 16;
            memcpy(out + done, blk, (size_t)n);
            done += n;
            ctr++;
        }
    }
}

static void srtp_init(srtp_ctx *c, const unsigned char *k, const unsigned char *s) {
    aes_cm_prf(k, s, 0, c->key, 16);
    aes_cm_prf(k, s, 2, c->salt, 14);
    aes_cm_prf(k, s, 1, c->auth, 20);
}

/* ---------------- 现场取证：把真实包与密钥落盘 ---------------- */
/*
 * cm_srtp_unprotect 反复 -7（HMAC 不匹配）时，光靠日志里的前 4 字节无法
 * 区分「ROC 猜错 / 密钥方向反 / AEAD / 明文 RTP / 有 MKI」这五种情况。
 * 这里把前 CM_SRTP_DUMP_MAX 个失败包的原始字节和 60 字节 KM（含派生后的
 * 三把 session key）落到设备文件，供主机侧用权威实现独立复算。
 *
 * 文件格式（全部小端 uint32 长度前缀）：
 *   [4]=60  [60]=keying material
 *   [4]=50  [50]=rx key16 || rx salt14 || rx auth20
 *   之后重复：[4]=pkt_len [pkt_len]=原始 SRTP 包
 */
#define CM_SRTP_DUMP_PATH "/userdisk/miniapp/data/srtp_dump.bin"
#define CM_SRTP_DUMP_MAX  8

static void dump_chunk(const void *data, int len) {
    int fd = open(CM_SRTP_DUMP_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    unsigned int n32;
    if (fd < 0) return;
    n32 = (unsigned int)len;
    if (write(fd, &n32, 4) != 4) { close(fd); return; }
    if (len > 0 && write(fd, data, (size_t)len) != len) { close(fd); return; }
    close(fd);
}

/* 清空文件（km 只应在会话开始时写一次） */
static void dump_truncate(void) {
    int fd = open(CM_SRTP_DUMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

static void ctr_xor(srtp_ctx *c, unsigned int ssrc, unsigned long long idx,
                    unsigned char *d, int len) {
    unsigned char iv[16];
    memset(iv, 0, 16);
    memcpy(iv, c->salt, 14);
    iv[4] ^= (unsigned char)(ssrc >> 24);
    iv[5] ^= (unsigned char)(ssrc >> 16);
    iv[6] ^= (unsigned char)(ssrc >> 8);
    iv[7] ^= (unsigned char)ssrc;
    iv[8] ^= (unsigned char)(idx >> 40);
    iv[9] ^= (unsigned char)(idx >> 32);
    iv[10] ^= (unsigned char)(idx >> 24);
    iv[11] ^= (unsigned char)(idx >> 16);
    iv[12] ^= (unsigned char)(idx >> 8);
    iv[13] ^= (unsigned char)idx;
    {
        int done = 0;
        unsigned short ctr = 0;
        while (done < len) {
            unsigned char blk[16];
            int ol = 0;
            EVP_CIPHER_CTX *e;
            int n, i;
            iv[14] = (unsigned char)(ctr >> 8);
            iv[15] = (unsigned char)ctr;
            e = EVP_CIPHER_CTX_new();
            if (!e) return;
            EVP_EncryptInit_ex(e, EVP_aes_128_ecb(), NULL, c->key, NULL);
            EVP_CIPHER_CTX_set_padding(e, 0);
            EVP_EncryptUpdate(e, blk, &ol, iv, 16);
            EVP_CIPHER_CTX_free(e);
            n = len - done;
            if (n > 16) n = 16;
            for (i = 0; i < n; i++) d[done + i] ^= blk[i];
            done += n;
            ctr++;
        }
    }
}

/* 计算 SRTP 认证标签（SHA1_80：取 HMAC 前 10 字节） */
static int srtp_tag(srtp_ctx *c, const unsigned char *p, int hl, int pl,
                    unsigned int roc, unsigned char *out) {
    unsigned char buf[2048];
    unsigned char md[20];
    unsigned int ml = sizeof(md);
    int total = hl + pl;
    if (total + 4 > (int)sizeof(buf) || total < 0) return -1;
    memcpy(buf, p, (size_t)total);
    buf[total + 0] = (unsigned char)(roc >> 24);
    buf[total + 1] = (unsigned char)(roc >> 16);
    buf[total + 2] = (unsigned char)(roc >> 8);
    buf[total + 3] = (unsigned char)roc;
    if (!HMAC(EVP_sha1(), c->auth, 20, buf, (size_t)(total + 4), md, &ml)) return -1;
    memcpy(out, md, 10);
    return 0;
}

/* 计算认证标签（覆盖给定字节，不追加 ROC 尾巴）—— SRTCP 用 */
static int srtp_tag_raw(srtp_ctx *c, const unsigned char *p, int n, unsigned char *out) {
    unsigned char md[20];
    unsigned int ml = sizeof(md);
    const unsigned char *use = p;
    unsigned char *tmp = NULL;
    if (n > 2048) {
        tmp = (unsigned char *)malloc((size_t)n);
        if (!tmp) return -1;
        memcpy(tmp, p, (size_t)n);
        use = tmp;
    }
    if (!HMAC(EVP_sha1(), c->auth, 20, use, (size_t)n, md, &ml)) {
        if (tmp) free(tmp);
        return -1;
    }
    memcpy(out, md, 10);
    if (tmp) free(tmp);
    return 0;
}

/*
 * 一次性穷举诊断（只在认证失败前几个包上跑，代价可忽略）。
 *
 * 认证标签对不上时，以下五种可能性在日志上长得一模一样：
 *   ① ROC 推断错   ② 密钥方向反（client/server write）
 *   ③ 其实是 SHA1_32 ④ 明文 RTP（没加密） ⑤ payload 与 tag 之间有 MKI
 * 与其一轮一轮试机位，这里把五种假设**当场全部试一遍**并打印命中项。
 */
static void brute_diag(cm_sess *s, cm_srtp_state *st, unsigned char *pkt, int len,
                       int hl, int pl, uint32_t ssrc, uint16_t seq) {
    unsigned char tag[10];
    int roc_rx = -1, roc_tx = -1, tag32_rx = -1, mki4_rx = -1;
    unsigned int roc;

    /* ① + ② 密钥方向 × ROC 0..64 */
    for (roc = 0; roc <= 64; roc++) {
        srtp_tag(&st->rx, pkt, hl, pl, roc, tag);
        if (!memcmp(tag, pkt + hl + pl, 10)) { roc_rx = (int)roc; break; }
    }
    if (roc_rx < 0) {
        for (roc = 0; roc <= 64; roc++) {
            srtp_tag(&st->tx, pkt, hl, pl, roc, tag);
            if (!memcmp(tag, pkt + hl + pl, 10)) { roc_tx = (int)roc; break; }
        }
    }
    /* ③ 假如实际是 SHA1_32：标签只有 4 字节，pl 就多算了 6 */
    if (roc_rx < 0 && pl >= 6) {
        int pl32 = pl + 6;
        for (roc = 0; roc <= 8; roc++) {
            srtp_tag(&st->rx, pkt, hl, pl32, roc, tag);
            if (!memcmp(tag, pkt + hl + pl32, 4)) { tag32_rx = (int)roc; break; }
        }
    }
    /* ⑤ 假如有 4 字节 MKI：标签起点后面再挪 4 */
    if (roc_rx < 0 && pl >= 4) {
        int pl2 = pl - 4;
        for (roc = 0; roc <= 8; roc++) {
            srtp_tag(&st->rx, pkt, hl, pl2, roc, tag);
            if (!memcmp(tag, pkt + hl + pl2, 10)) { mki4_rx = (int)roc; break; }
        }
    }

    CM_LOGE("#%d SRTP BRUTE seq=%u ssrc=%08x len=%d hl=%d pl=%d "
            "hit_roc_rx=%d hit_roc_tx=%d hit_sha1_32=%d hit_mki4=%d",
            s->id, (unsigned)seq, ssrc, len, hl, pl,
            roc_rx, roc_tx, tag32_rx, mki4_rx);

    /* ④ 明文检测：未加密时 payload 首字节应是合法 H264 NAL 头 */
    if (pl > 0) {
        unsigned char b = pkt[hl];
        unsigned int nt = b & 0x1f;
        if (nt == 5 || nt == 7 || nt == 8 || nt == 1 || nt == 6 || nt == 9 || nt == 28 || nt == 24)
            CM_LOGE("#%d SRTP BRUTE payload head=%02x (nal_type=%u) LOOKS LIKE PLAINTEXT H264",
                    s->id, (unsigned)b, nt);
        else
            CM_LOGE("#%d SRTP BRUTE payload head=%02x (not a plausible NAL: encrypted)",
                    s->id, (unsigned)b);
    }
}

/* ---------------- 会话集成 ---------------- */
int cm_srtp_open(cm_sess *s) {
    unsigned char km[60];
    cm_srtp_state *st;
    int n = cm_dtls_get_km(s, km, sizeof(km));
    if (n < 60) return -1;

    st = (cm_srtp_state *)calloc(1, sizeof(*st));
    if (!st) return -1;
    pthread_mutex_init(&st->mu, NULL);

    /* 我们做 DTLS client：
     *   收流入站解密用 server_write_*（对端的写密钥）
     *   发出去（RTCP PLI）用 client_write_*（我方的写密钥） */
    srtp_init(&st->rx, km + 16, km + 46);
    srtp_init(&st->tx, km + 0, km + 32);
    st->ready = 1;
    s->srtp_priv = st;
    CM_LOGI("#%d srtp ready (rx=server_write tx=client_write)", s->id);

    /* 现场取证：KM 与本端派生的 rx session key 落盘（每次会话覆盖） */
    dump_truncate();
    dump_chunk(km, 60);
    {
        unsigned char rxmat[50];
        memcpy(rxmat + 0, st->rx.key, 16);
        memcpy(rxmat + 16, st->rx.salt, 14);
        memcpy(rxmat + 30, st->rx.auth, 20);
        dump_chunk(rxmat, 50);
    }
    CM_LOGI("#%d dump: km(60) + rxkey16+rxsalt14+rxauth20 -> %s", s->id, CM_SRTP_DUMP_PATH);
    return 0;
}

void cm_srtp_close(cm_sess *s) {
    cm_srtp_state *st;
    if (!s || !s->srtp_priv) return;
    st = (cm_srtp_state *)s->srtp_priv;
    pthread_mutex_destroy(&st->mu);
    free(st);
    s->srtp_priv = NULL;
}

/* 找到/建立该 SSRC 的流状态 */
static void *stream_get(cm_srtp_state *st, uint32_t ssrc) {
    int i;
    for (i = 0; i < st->nstreams; i++) {
        if (st->streams[i].ssrc == ssrc) return &st->streams[i];
    }
    if (st->nstreams >= (int)(sizeof(st->streams) / sizeof(st->streams[0]))) return NULL;
    st->streams[st->nstreams].ssrc = ssrc;
    st->streams[st->nstreams].roc = 0;
    st->streams[st->nstreams].max_seq = 0;
    st->streams[st->nstreams].inited = 0;
    st->nstreams++;
    return &st->streams[st->nstreams - 1];
}

/*
 * 解密一个 SRTP 包（原地），返回明文 RTP 长度。
 * 失败返回 <0。
 */
int cm_srtp_unprotect(cm_sess *s, unsigned char *pkt, int len) {
    cm_srtp_state *st = (cm_srtp_state *)s->srtp_priv;
    int hl, pl, tag_ok;
    uint32_t ssrc;
    uint16_t seq;
    unsigned int roc = 0;
    unsigned char tag[10];
    void *sp;

    if (!st || !st->ready) return -1;
    if (len < 12 + 10 + 4) return -2;
    if ((pkt[0] & 0xC0) != 0x80) return -3; /* 不是 RTP/RTCP v2 */

    /* RTCP 走不同格式（PT 200-204，payload 从字节 4 起），直接丢给上层忽略 */
    if (pkt[1] >= 192 && pkt[1] <= 223) return -4;

    seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    ssrc = cm_rd32(pkt + 8);

    /* RTP 头长度：12 + 4*CSRC + extension */
    hl = 12 + (pkt[0] & 0x0f) * 4;
    if ((pkt[0] & 0x10) && len >= hl + 4) {
        int ext_words = (pkt[hl + 2] << 8) | pkt[hl + 3];
        hl += 4 + ext_words * 4;
    }
    if (hl > len - 10) return -5;

    pthread_mutex_lock(&st->mu);
    sp = stream_get(st, ssrc);
    if (!sp) { pthread_mutex_unlock(&st->mu); return -6; }
    {
        struct { uint32_t ssrc; uint32_t roc; uint16_t max_seq; int inited; } *ss = sp;
        /* ROC 推算（RFC 3711 附录 A 的简化版） */
        if (!ss->inited) {
            ss->roc = 0;
            ss->max_seq = seq;
            ss->inited = 1;
        } else if (seq < ss->max_seq && (uint16_t)(ss->max_seq - seq) > 0x8000u) {
            ss->roc++; /* 序号回绕 */
        }
        ss->max_seq = seq;
        roc = ss->roc;
    }
    pl = len - hl - 10;

    /* 现场取证：前若干包的解析参数，用来核对头长/扩展/PT/ROC 推断 */
    if (s->stats.srtp_auth_fail + (uint64_t)s->stats.rtp_rx < CM_SRTP_DUMP_MAX) {
        int extw = 0;
        if (pkt[0] & 0x10) extw = (pkt[hl - 2] << 8) | pkt[hl - 1];
        CM_LOGE("#%d SRTP DBG roc=%u seq=%u ssrc=%08x pt=%u x=%d extw=%d hl=%d len=%d pl=%d",
                s->id, roc, (unsigned)seq, ssrc, (unsigned)(pkt[1] & 0x7f),
                (pkt[0] & 0x10) ? 1 : 0, extw, hl, len, pl);
    }

    /* 先校验认证标签（对密文），通过后再解密 */
    srtp_tag(&st->rx, pkt, hl, pl, roc, tag);
    tag_ok = (memcmp(tag, pkt + hl + pl, 10) == 0);
    if (!tag_ok) {
        s->stats.srtp_auth_fail++;
        /* 现场取证：前 CM_SRTP_DUMP_MAX 个认证失败的包原样落盘，
         * 主机侧用权威实现独立复算 HMAC/ROC/密钥方向。 */
        if (s->stats.srtp_auth_fail <= CM_SRTP_DUMP_MAX) {
            dump_chunk(pkt, len);
            CM_LOGE("#%d DUMP fail#%llu len=%d seq=%u ssrc=%08x roc=%u hl=%d pl=%d",
                    s->id, (unsigned long long)s->stats.srtp_auth_fail, len,
                    (unsigned)seq, ssrc, roc, hl, pl);
            CM_LOGE("#%d DUMP hex: %02x %02x %02x %02x %02x %02x %02x %02x "
                    "tag_calc=%02x%02x%02x%02x%02x tag_pkt=%02x%02x%02x%02x%02x",
                    s->id, pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7],
                    tag[0], tag[1], tag[2], tag[3], tag[4],
                    pkt[hl + pl], pkt[hl + pl + 1], pkt[hl + pl + 2],
                    pkt[hl + pl + 3], pkt[hl + pl + 4]);
        }
        if (s->stats.srtp_auth_fail <= 2)
            brute_diag(s, st, pkt, len, hl, pl, ssrc, seq);
        pthread_mutex_unlock(&st->mu);
        return -7;
    }
    {
        /* 32 位包索引：(roc << 16) | seq */
        unsigned long long idx = ((unsigned long long)roc << 16) | seq;
        ctr_xor(&st->rx, ssrc, idx, pkt + hl, pl);
    }
    pthread_mutex_unlock(&st->mu);
    return hl + pl;
}

/* SRTCP 是否可用（密钥已就绪且允许加密发送） */
int cm_srtcp_ready(cm_sess *s) {
    cm_srtp_state *st = (cm_srtp_state *)s->srtp_priv;
    return (st && st->ready) ? 1 : 0;
}

/*
 * SRTCP 保护（用于发出 RTCP PLI/PLI-burst）。
 *
 * RFC 3711 §3.4：RTCP 包的前 8 字节（header + sender SSRC）不加密，
 * 之后加密到 FCI 结束；再追加 4 字节 SRTCP index（bit31 = E-flag 置 1），
 * 最后 10 字节认证标签覆盖「已加密正文 + index（含 E 位）」。
 *
 * 返回写入后的总长度（原 len + 14），失败返回 <0。
 */
int cm_srtcp_protect(cm_sess *s, unsigned char *pkt, int len, int cap) {
    cm_srtp_state *st = (cm_srtp_state *)s->srtp_priv;
    uint32_t ssrc, idx;
    unsigned char tag[10];
    int plen;

    if (!st || !st->ready) return -1;
    if (len < 8 || len + 14 > cap) return -2;

    ssrc = cm_rd32(pkt + 4);
    pthread_mutex_lock(&st->mu);
    idx = st->srtcp_index++ & 0x7FFFFFFFu;
    pthread_mutex_unlock(&st->mu);

    plen = len - 8;
    if (plen > 0) ctr_xor(&st->tx, ssrc, (unsigned long long)idx, pkt + 8, plen);

    /* SRTCP index，E 位固定为 1（表示本包已加密） */
    cm_wr32(pkt + len, idx | 0x80000000u);
    len += 4;

    /* 认证标签覆盖「RTCP 头 + 加密正文 + 4 字节 SRTCP index」，
     * 注意：SRTCP 不额外追加任何 4 字节（与 SRTP 的 ROC 尾巴不同） */
    if (srtp_tag_raw(&st->tx, pkt, len, tag) != 0) return -3;
    memcpy(pkt + len, tag, 10);
    return len + 10;
}
