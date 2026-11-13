/*
 * cm_cmd.c —— cmd(id, cmdJson) 的实现 + RTCP PLI / Receiver Report 发送
 *
 * 支持的 op：
 *   setRect        {rect:{x,y,w,h}} 或扁平 {x,y,w,h}   改 kmssink render-rectangle
 *   stats                                               立刻吐一个 stat 事件
 *   requestKeyFrame                                     发 RTCP PLI 给网关
 *   setLogLevel    {level}
 *   setAudio       {on:0|1}
 *   setAudioEnabled 同 setAudio（别名）
 *
 * RTCP PLI（RFC 4585 PSFB, FMT=1）最小形态 12 字节：
 *   0x81, 0xCE(PT=206), length=2, sender SSRC, media SSRC
 * 因为我们用 SRTP，PLI 必须走 SRTCP 加密（否则网关多半丢弃）。
 *
 * ---- 为什么补上 Receiver Report（PT=201）----
 *
 * ★ 先说结论：**RR 不是 bug1（约四分钟停推）的修复方案。**
 *
 * 第一版假设是「我们从不发 RR，SFU 靠 RR 判活，分钟级超时后把我们 pause 掉」，
 * 现场（真机 2026-11-12 22:43-22:58，原神 yssy）：起播 fps=29、rx 稳步涨，
 * 约 5 分钟后 rx 冻结、fps 归零，此后 10 分钟网关一个 RTP 包都不发，与此同时
 * WebSocket 健康、屏幕保活正常、PLI 每 2 秒照发但网关完全不理。
 *
 * 但后续证据（2026-11-13）把这个假设**大幅降级**，三条都不利于它：
 *   1. 网关从始至终不回任何 RTCP（`grep -a RTCP` / `rtcpIn` 全空），
 *      说明它对这个订阅者本来就不做 RTCP 双向往来；
 *   2. 停推是**音视频同时停**，pt histogram 98:7362 + 111:11339 = 18701，
 *      正好等于 probe 冻结时的 rx；音频 11339 包 / 50pkt/s ≈ 227 秒；
 *   3. 三次独立复现落在 4m19s / 4m18s / 4m18s，误差 1 秒 —— "SFU 靠 RR
 *      判活"的超时通常松散（几十秒量级抖动），这种精度更像服务端定时器。
 *   现在的最强解释是官方 SDK 的挂机 kickout（RtcKeepPlaying / RtcKeepaliveCfg，
 *   我们零实现），完整分析见 docs/BUGFIX-bug1-rtcp-rr.md。
 *
 * 那还发 RR 吗？发。理由收回成一条**纯协议正确性**：
 *   RFC 3550 §6.4 要求接收端周期上报接收质量，我们整局一个都不发是残缺的；
 *   任何合规 SFU 都可能拿 RR 做带宽估计与丢包反馈（决定要不要补关键帧、
 *   要不要降码率）。代价是 1Hz、46~70 字节（约 0.4~0.6 kbps），无副作用。
 *   所以这里是"补齐协议义务"，不是"修 bug1"。
 *
 * 实现要点：每秒一条 RR，携带视频+音频各一个 reception report block，
 * 与 PLI 走**同一条 SRTCP 加密路径**（媒体面是 SRTP，明文 RTCP 会被丢弃）。
 * 报文逐字段构造与"为什么这么填"见 docs/BUGFIX-bug1-rtcp-rr.md。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm_session.h"

/* RR 发送周期：1 秒。云游戏场景下 1Hz 是常见取值（libwebrtc 在小规模
 * 会话里也是 1s 量级；RFC 3550 §6.4 的带宽折算公式给出的是下限 5s，
 * 1s 更密但代价可忽略：明文 32/56 字节，SRTCP 后 +14 字节）。
 * 既能满足 SFU 的存活判定，又不会把上行搞满。不要更密。 */
#define RR_INTERVAL_MS 1000
/* 日志限速：每秒一条会淹没真事件（现场日志里 PLI 已经刷了很多）。
 * 首次必打，之后每 10 秒一条 —— lost/jitter 都在同一行里，够对账了。 */
#define RR_LOG_INTERVAL_MS 10000

void cm_send_pli(cm_sess *s) {
    unsigned char pkt[64];
    int len = 12;
    int enc = -1;

    if (!s || s->sock < 0 || !s->cfg.video_ssrc) return;

    /* sender SSRC 必须是我们自己的标识，绝不能等于 media SSRC ——
     * 网关看到「自己发的包」会直接丢弃（RTCP 环路检测），
     * 这也是之前连发 78 次 PLI 却一直拿不到 IDR 的原因。 */
    if (!s->cfg.tx_ssrc) s->cfg.tx_ssrc = 0x50454E43u; /* 'PENC' */

    pkt[0] = 0x81;                       /* V=2, P=0, RC=1 */
    pkt[1] = 206;                        /* PT=PSFB */
    cm_wr16(pkt + 2, 2);                 /* length = 3 words - 1 */
    cm_wr32(pkt + 4, s->cfg.tx_ssrc);        /* sender SSRC（我方） */
    cm_wr32(pkt + 8, s->cfg.video_ssrc); /* media source SSRC（网关） */

    /* 优先 SRTCP 加密；密钥未就绪时退回明文（网关有时宽容） */
    if (cm_srtcp_ready(s)) {
        enc = cm_srtcp_protect(s, pkt, len, (int)sizeof(pkt));
        if (enc > 0) len = enc;
    }

    if (cm_udp_send(s->sock, pkt, len, s->cfg.remote_ip, s->cfg.remote_port) > 0) {
        s->pli_count++;
        CM_LOGI("#%d PLI sent (%d bytes, %s)", s->id, len, enc > 0 ? "srtcp" : "plain");
    } else {
        CM_LOGW("#%d PLI send failed", s->id);
    }
}

/* 每秒/每 2 秒调一次：缺 keyframe 就补 PLI，首流先连发 3 次（间隔由调用方节奏控制） */
void cm_kf_tick(cm_sess *s) {
    uint64_t now = cm_now_ms();
    if (!s) return;
    if (s->pli_burst < 3) {
        /* 首流：连续 3 次，每次间隔 >= 150ms */
        if (now >= s->next_pli_ms) {
            cm_send_pli(s);
            s->pli_burst++;
            s->next_pli_ms = now + 200;
        }
        return;
    }
    if (s->have_kf && (now - s->last_kf_ms) < 2000) return;
    if (now < s->next_pli_ms) return;
    cm_send_pli(s);
    s->next_pli_ms = now + 2000;
}

/* ---------------- RTCP Receiver Report（RFC 3550 §6.4.2, PT=201） ----------------
 *
 * ★ 定位说明（重要）：发 RR 是**补齐协议义务**，不是 bug1 的修复方案。
 *   后续真机证据（2026-11-13）表明「起播约 4 分 18 秒停推」指向服务端挂机
 *   kickout 定时器（官方 SDK 的 RtcKeepPlaying / RtcKeepaliveCfg 机制，我们
 *   零实现），与 RR 无关。详见 docs/BUGFIX-bug1-rtcp-rr.md。
 *   保留 RR 的理由只有一个：RFC 3550 要求接收端周期上报，任何合规 SFU 都
 *   可能用它做带宽估计与丢包反馈，代价极低（1Hz、46~80 字节）且无副作用。
 *
 * 报文布局（每行 = 4 字节一个字）：
 *   word0  V=2 P=0 RC=n PT=201 length  -> 81 C9 00 len
 *          RTCP 头只有 1 个字：首字节是 V/P/RC、次字节是 PT、后两字节是 length。
 *          length 以 32 位字为单位且**不含头字自身**：
 *            n=1 -> 8 words  -> length = 7  -> 明文 32 字节
 *            n=2 -> 14 words -> length = 13 -> 明文 56 字节
 *   word1  sender SSRC          -> s->cfg.tx_ssrc（我方标识，'PENC'）
 *   ---- 每路流 6 个字（24 字节）= 一个 reception report block ----
 *   wordX  SSRC_n               -> 被报告的媒体源（视频先、音频后）
 *   wordX  fraction lost(8) | cumulative lost(24, 有符号补码)
 *   wordX  extended highest sequence number received
 *   wordX  interarrival jitter（RTP 时间戳单位，已 >>4）
 *   wordX  LSR（最近一个 SR 的 NTP 中间 32 位）
 *   wordX  DLSR（自该 SR 起到现在的延时，单位 1/65536 秒）
 *
 * 为什么音视频各报一段（RC=2）而不是只报视频：report block 是"每个被报告的
 * SSRC 一段"，音视频是两个独立 SSRC；现场 PT 直方图是 98:7362 / 111:11339，
 * 音频包量是视频的 1.5 倍多，只报视频等于把网关最需要反馈的那条流留白。
 *
 * 三个容易踩的点，都在这里处理：
 *   1. SRTCP 要求 sender SSRC 在 pkt+4（cm_srtp.c 的 CTR IV 直接取它），
 *      所以 RR 与 PLI 的头 8 字节布局天然一致，不需要额外适配。
 *   2. report block 的 SSRC **绝不能是 0**：网关会把整条 RR 当无效丢掉。
 *      某一路没有 SSRC 就少报一段（RC 减 1），两路都没有则整条不发。
 *   3. fraction lost 必须按"本 RR 与上一个 RR 之间"的区间算（RFC A.3），
 *      用累计值直接除会得到开机以来的平均丢包率，网关看不出"现在突然变差"。
 */

/* 填一个 reception report block（24 字节），返回真值表示填了。
 *
 * 用指针传两路各自的统计状态，是因为视频/音频在 cm_stats 里是两组同构字段；
 * 复制两份 40 行报文逻辑极易写歪一边（尤其是有符号 24 位那几行）。
 * 本函数只读统计、写报文和 prior 快照，不碰任何判定逻辑。
 *
 * lost / jitter 通过出参带回给日志用（两路都要能看见）。 */
static int rr_fill_block(unsigned char *b, uint32_t ssrc, uint32_t ext_max, uint32_t ext_base,
                         int have_ext, uint64_t received, uint32_t *prior_exp, uint64_t *prior_rcv,
                         uint64_t jitter_q4, uint32_t lsr, uint32_t dlsr, uint64_t *out_lost,
                         uint32_t *out_jit) {
    uint32_t expected;
    uint64_t lost_total, exp_iv, rcv_iv;
    int32_t lost_signed;
    uint32_t frac, jitter;

    if (!ssrc) return 0; /* 绝不发 SSRC_n = 0 */

    /* ---- 序号/丢包统计（RFC 3550 A.1/A.3）----
     * expected = 最高扩展序号 - 首个扩展序号 + 1；cumulative lost = expected - received。
     * received 口径含重复包与乱序包（A.3 的约定），所以差值就是网络丢包。 */
    ext_max = have_ext ? ext_max : 0;
    if (!have_ext) {
        expected = 0;
        received = 0;
    } else {
        expected = ext_max - ext_base + 1u;
    }
    lost_total = (expected > (uint32_t)received) ? (uint64_t)(expected - (uint32_t)received) : 0;
    /* cumulative lost 在报文里是 24 位**有符号**：正向丢包填正数，
     * 重复包过多导致负值时填二进制补码（RFC 3550 §6.4.1）。 */
    lost_signed = (expected >= (uint32_t)received) ? (int32_t)lost_total
                                                  : -(int32_t)((uint32_t)received - expected);
    if (lost_signed > 0x7FFFFF) lost_signed = 0x7FFFFF;
    if (lost_signed < -0x800000) lost_signed = -0x800000;

    /* 区间丢包率：区间内"预期新增 - 实收新增"除以"预期新增"。 */
    exp_iv = ext_max - *prior_exp;
    rcv_iv = (received >= *prior_rcv) ? (received - *prior_rcv) : 0;
    if (exp_iv == 0 || exp_iv < rcv_iv) {
        frac = 0; /* 区间内没收到预期包（或重复过多）：按 RFC 置 0，别报虚假 100% */
    } else {
        uint64_t l = exp_iv - rcv_iv;
        l = (l << 8) / exp_iv;
        frac = (l > 255) ? 255 : (uint32_t)l;
    }
    *prior_exp = ext_max;
    *prior_rcv = received;

    /* jitter 内部是 Q4 定点，报文里要的是整数 RTP 单位 */
    jitter = (uint32_t)(jitter_q4 >> 4);

    b[0] = (unsigned char)((ssrc >> 24) & 0xFF);
    b[1] = (unsigned char)((ssrc >> 16) & 0xFF);
    b[2] = (unsigned char)((ssrc >> 8) & 0xFF);
    b[3] = (unsigned char)(ssrc & 0xFF);
    b[4] = (unsigned char)frac;
    /* 24 位有符号补码：先按无符号 24 位截断低位即可（补码语义一致） */
    b[5] = (unsigned char)(((uint32_t)lost_signed >> 16) & 0xFF);
    b[6] = (unsigned char)(((uint32_t)lost_signed >> 8) & 0xFF);
    b[7] = (unsigned char)((uint32_t)lost_signed & 0xFF);
    cm_wr32(b + 8, ext_max); /* extended highest seq received */
    cm_wr32(b + 12, jitter);
    cm_wr32(b + 16, lsr);
    cm_wr32(b + 20, dlsr);

    /* lost 用**有符号**回传：重复包多于丢包时真值是负数，日志里要和报文一致，
     * 否则对账时"日志说 0、pcap 说 -3"这种偏差会白烧半天。 */
    *out_lost = (uint64_t)(int64_t)lost_signed;
    *out_jit = jitter;
    return 1;
}

void cm_rtcp_tick(cm_sess *s) {
    unsigned char pkt[96];
    uint64_t now;
    uint32_t ssrc_v, ssrc_a, lsr = 0, dlsr = 0;
    uint64_t lost_v = 0, lost_a = 0;
    uint32_t jit_v = 0, jit_a = 0;
    int rc = 0, len, enc = -1;
    const char *trans;

    if (!s || s->sock < 0) return;

    now = cm_now_ms();
    if (s->next_rr_ms && now < s->next_rr_ms) return;
    s->next_rr_ms = now + RR_INTERVAL_MS;

    /* 被报告的源：优先 SDP 协商出的 SSRC；没有就用实际收到的第一个
     * SSRC（cm_rtp.c 记录）。某一路两者都没有说明我们还没收到过该路包，
     * 此时那一段跳过 —— 0 会被网关丢弃（见上面第 2 点）。 */
    ssrc_v = s->cfg.video_ssrc ? s->cfg.video_ssrc : s->stats.vid_ssrc_seen;
    ssrc_a = s->cfg.audio_ssrc ? s->cfg.audio_ssrc : s->stats.aud_ssrc_seen;
    if (!ssrc_v && !ssrc_a) return;

    if (!s->cfg.tx_ssrc) s->cfg.tx_ssrc = 0x50454E43u; /* 'PENC'，与 PLI 一致 */

    /* ---- LSR / DLSR ----
     * 只有真的收到过网关 SR 才填，否则两个字段留 0（RFC 允许，语义是"还没有
     * 可用于计算 RTT 的参考点"）。DLSR 单位是 1/65536 秒，用单调毫秒折算。
     * 注意：入站 RTCP 目前是 SRTCP 密文且不解密（cm_srtp.c 只解 RTP），
     * 所以 have_sr 恒为 0 —— 这是"合法且诚实"的状态，不是漏填。 */
    if (s->stats.have_sr) {
        lsr = s->stats.sr_ntp_mid;
        if (now > s->stats.sr_arr_ms) dlsr = (uint32_t)(((now - s->stats.sr_arr_ms) * 65536ULL) / 1000ULL);
    }

    memset(pkt, 0, sizeof(pkt));
    /* 先写头（RC 等块填完再回填），sender SSRC 固定在 word1 */
    cm_wr32(pkt + 4, s->cfg.tx_ssrc);

    rc += rr_fill_block(pkt + 8 + rc * 24, ssrc_v, s->stats.ext_seq_max,
                        s->stats.ext_seq_base, s->stats.have_ext_seq, s->stats.vid_rx,
                        &s->stats.rr_expected_prior, &s->stats.rr_received_prior,
                        s->stats.jitter_q4, lsr, dlsr, &lost_v, &jit_v);
    rc += rr_fill_block(pkt + 8 + rc * 24, ssrc_a, s->stats.aud_ext_seq_max,
                        s->stats.aud_ext_seq_base, s->stats.have_aud_ext_seq,
                        s->stats.aud_rx, &s->stats.aud_rr_expected_prior,
                        &s->stats.aud_rr_received_prior, s->stats.aud_jitter_q4, lsr, dlsr,
                        &lost_a, &jit_a);
    if (rc <= 0) return; /* 理论上不可达（上面已判空），兜底 */

    len = 8 + rc * 24;
    pkt[0] = (unsigned char)(0x80 | (rc & 0x1F)); /* V=2, P=0, RC=n */
    pkt[1] = 201;                                 /* PT=RR */
    cm_wr16(pkt + 2, (uint16_t)(len / 4 - 1));      /* length = words - 1 */

    /* SRTCP 加密：与 PLI 完全同一条路径。密钥未就绪时退回明文并说明原因。 */
    if (cm_srtcp_ready(s)) {
        enc = cm_srtcp_protect(s, pkt, len, (int)sizeof(pkt));
        if (enc > 0) len = enc;
    }
    trans = enc > 0 ? "srtcp" : "plain";

    if (cm_udp_send(s->sock, pkt, len, s->cfg.remote_ip, s->cfg.remote_port) <= 0) {
        CM_LOGW("#%d RR send failed", s->id);
        return;
    }

    s->rr_count++;
    /* 日志限速：首条必打（确认 RR 通道真的起来了），之后每 10 秒一条。
     * 每秒一条会把 PLI 和丢包告警埋掉，而那两条才是排障主线。
     * 两路的 lost/jitter 都印在同一行，方便和 pt histogram 对账。 */
    if (s->rr_log_ms == 0 || now - s->rr_log_ms >= RR_LOG_INTERVAL_MS) {
        s->rr_log_ms = now;
        s->rr_last_lost = (uint32_t)lost_v;
        s->rr_last_jit = jit_v;
        CM_LOGI("#%d RR sent (%d bytes, %s) rc=%d v[lost=%lld jit=%u] a[lost=%lld jit=%u]",
                s->id, len, trans, rc, (long long)lost_v, (unsigned)jit_v, (long long)lost_a,
                (unsigned)jit_a);
    }
}

/* ============ RTCP Generic NACK（RFC 4585 §6.2.1，PT=205 FMT=1） ============
 *
 * 为什么必须有它（真机 2026-11-13 16:25-16:42，原神 yssy，高铁 Wi-Fi）：
 *   `still no IDR (rx=74418 kfReq=468 gateDrop=15511 auDrop=401)`
 * PLI 一直在发（kfReq=468），网关也在推（rx 在涨），但画面 fps 恒 0。
 * 根因不是网关不理我们，是**每个 IDR 都被丢包打碎**：720p 的 IDR 有
 * 30~60 个 RTP 包，5-8% 丢包率下几乎必缺一片；缺一片整帧报废 →
 * have_kf 清零 → 后面所有 P 帧被闸门丢掉（gateDrop 15511）→ 再要 IDR →
 * 再被撕。401 个残缺 AU 赔掉了 15511 个正常 P 帧，约 39:1。
 *
 * NACK 是打破这个死循环的唯一手段：缺的那几个包点名要回来，IDR 才有机会
 * 拼完整。注意它和 PLI 的分工 ——
 *   PLI  = 「整个关键帧我都没有，请重发一整帧」
 *   NACK = 「我只缺这几个包，请重发这几个包」
 * 弱网下后者便宜几个数量级（重传 2 个包 vs 重发 40 个包的 IDR），
 * 所以必须优先把 NACK 用好，PLI 只作兜底。
 */
/* 发送间隔：30ms。
 *
 * 取值理由：一个 RTT 内至少要能发两轮（丢包检测 20ms + 网关重传 RTT
 * 20~40ms），再密就是刷屏；再疏则重传回来时 AU 早已 flush 掉了。
 * 30ms 同时满足「够快」和「上行带宽可忽略」：一条 NACK 最多 ~120 字节，
 * 30ms 一条 = 32kbps，只有在真的持续丢包时才会到这个量级。 */
#define NACK_MIN_INTERVAL_MS 30
/* 单条 NACK 报文最多带多少个 FCI。每个 FCI 4 字节，加 12 字节头：
 * 30 个 FCI = 132 字节，远小于 MTU，也小于常见的中转最小分片。
 * 一个 FCI 用 BLP 能覆盖「PID + 之后 16 个连续 seq」共 17 个包，
 * 所以 30 个 FCI 理论上能覆盖 510 个包，远超一帧的包数。 */
#define NACK_MAX_FCI 30
/* 从 cm_rtp.c 的缺口表里一次最多取多少个 seq。取回后还要压成 FCI，
 * 30 个 FCI 最多覆盖 510 个包，所以取 256 个（= 缺口表容量上限，
 * cm_rtp.c:86 的 NACK_GAP_MAX）足够把整张表一次清空，不会留积压。 */
#define NACK_TAKE_MAX 256
/* 缺口优先级：与 cm_rtp.c:96-97 的 NACK_PRIO_IDR / NACK_PRIO_P 必须同值。
 * 单独定义是因为那两个宏是 cm_rtp.c 的内部实现细节，没有进头文件；
 * 这里是唯一需要读懂该字节的跨文件点（只为日志里的 idr= 计数）。 */
#define NACK_PRIO_IDR 2
/* 日志限速：NACK 是高频事件（弱网下每秒几十条），全打会淹没 PLI 和丢包告警。
 * 首次必打（确认通道起来了），之后每 5 秒一条。 */
#define NACK_LOG_INTERVAL_MS 5000

/* 把一串**已按 seq 升序排好**的缺口压成 FCI（PID + BLP）。
 *
 * BLP 语义（RFC 4585 §6.2.1）：bit i 置位表示 PID+i+1 这个包也要重传。
 * 也就是说一个 FCI 能表达「PID 以及它之后 16 个包」里的任意子集。
 * 做法：取当前最小的缺口当 PID，把 BLP 窗口内、同样在缺口列表里的 seq
 * 的对应 bit 置起来，然后从窗口后第一个缺口继续。
 *
 * 为什么值得压：IDR 的缺口往往是连续几十个（丢包是突发性的），
 * 不压的话一个 40 包的 IDR 缺口要写 40 个 FCI = 160 字节；
 * 压完只要 3 个 FCI = 12 字节。报文短了，丢的概率也低。
 *
 * 输入必须有序：乱序时 BLP 窗口会漏掉本该覆盖的字节，压不紧。
 * 调用方（cm_nack_tick）在进来之前按 seq 排过序。 */
static int nack_pack_fci(const unsigned char *gaps, int gap_n,
                         unsigned char *out, int max_fci, int *used)
{
    int i = 0, nfci = 0;
    while (i < gap_n && nfci < max_fci) {
        uint16_t pid = (uint16_t)((gaps[i * 4] << 8) | gaps[i * 4 + 1]);
        uint32_t blp = 0;
        int j = i + 1;
        while (j < gap_n) {
            uint16_t s2 = (uint16_t)((gaps[j * 4] << 8) | gaps[j * 4 + 1]);
            int d = (int)(int16_t)(s2 - pid);
            if (d <= 0) { j++; continue; }         /* 重复 seq（理论上不该有） */
            if (d > 16) break;                     /* 超出 BLP 窗口，留给下一组 */
            blp |= (1u << (d - 1));
            j++;
        }
        out[nfci * 4 + 0] = (unsigned char)(pid >> 8);
        out[nfci * 4 + 1] = (unsigned char)(pid & 0xFF);
        out[nfci * 4 + 2] = (unsigned char)((blp >> 8) & 0xFF);
        out[nfci * 4 + 3] = (unsigned char)(blp & 0xFF);
        nfci++;
        (*used)++;                                 /* PID 本身 */
        {
            uint32_t b;
            for (b = 0; b < 16; b++) if (blp & (1u << b)) (*used)++;
        }
        i = j;
    }
    return nfci;
}

/* 按 seq 升序排缺口（插入排序，元素数 ≤256 且几乎有序时最优）。
 * 排序只影响打包紧凑度，不影响优先级 —— 优先级已经在
 * cm_rtp_take_nack_gaps 的「取哪些」阶段生效了（IDR 先占额度）。 */
static void nack_sort_gaps(unsigned char *gaps, int n) {
    int i, j;
    for (i = 1; i < n; i++) {
        unsigned char cur[4];
        uint16_t cs;
        memcpy(cur, gaps + i * 4, 4);
        cs = (uint16_t)((cur[0] << 8) | cur[1]);
        j = i - 1;
        while (j >= 0) {
            uint16_t js = (uint16_t)((gaps[j * 4] << 8) | gaps[j * 4 + 1]);
            if ((int16_t)(js - cs) <= 0) break;
            memcpy(gaps + (j + 1) * 4, gaps + j * 4, 4);
            j--;
        }
        memcpy(gaps + (j + 1) * 4, cur, 4);
    }
}

void cm_nack_tick(cm_sess *s) {
    unsigned char gaps[NACK_TAKE_MAX * 4];
    /* ★ N1 修正（REVIEW-report N1）：缓冲区必须给 SRTCP 尾巴留 14 字节
     * （4 字节 index + 10 字节 auth tag，见 cm_srtp.c 的 cm_srtcp_protect，
     * 它要求 len + 14 <= cap）。旧代码只有 12 + NACK_MAX_FCI*4 = 132，
     * nfci=27 时 len=120，120+14=134 > 132 → cm_srtcp_protect 返回 -2，
     * 而失败分支 `if (enc > 0) len = enc;` 会**照样按明文发出去**：
     * SRTP 会话里明文 RTCP 被网关直接丢弃，但 vid_nack_sent++ 和日志
     * 依然打「NACK sent」—— 计数器显示一切正常，实际全是死包。
     * 而 nfci≥27 恰好就是丢包最严重、最需要重传的时候。 */
    unsigned char pkt[12 + NACK_MAX_FCI * 4 + 16];
    uint64_t now;
    int n, nfci, len, enc = -1, used = 0, n_idr = 0, i;
    const char *trans;

    if (!s || s->sock < 0) return;

    now = cm_now_ms();
    if (s->next_nack_ms && now < s->next_nack_ms) return;

    n = cm_rtp_take_nack_gaps(s, gaps, NACK_TAKE_MAX);
    if (n <= 0) {
        /* 没有缺口：什么都不发。空 NACK 是有害的 —— 部分网关会把它当成
         * 「整个帧都要重传」，反而雪上加霜。 */
        s->next_nack_ms = now + NACK_MIN_INTERVAL_MS;
        return;
    }
    s->next_nack_ms = now + NACK_MIN_INTERVAL_MS;

    /* 先数 IDR 缺口（排序前数，排完序 prio 字段还在但顺序变了，
     * 这里只是为了日志里的 idr= 计数）。 */
    for (i = 0; i < n; i++) if (gaps[i * 4 + 2] == NACK_PRIO_IDR) n_idr++;

    nack_sort_gaps(gaps, n);
    nfci = nack_pack_fci(gaps, n, pkt + 12, NACK_MAX_FCI, &used);
    if (nfci <= 0) return;

    len = 12 + nfci * 4;
    /* ---- RTCP 头 ---- */
    pkt[0] = 0x81;                                 /* V=2, P=0, FMT=1 (Generic NACK) */
    pkt[1] = 205;                                  /* PT=RTPFB */
    cm_wr16(pkt + 2, (uint16_t)(len / 4 - 1));     /* length = words - 1 */
    cm_wr32(pkt + 4, s->cfg.tx_ssrc);              /* sender SSRC（我方） */
    /* media source SSRC：必须是**远端发送**的那一路。用 video_ssrc
     * （SDP 里协商的、网关发视频用的 SSRC），不是我们自己造的 tx_ssrc ——
     * 填错的话网关会当成「有人在要求重传别人的流」直接忽略。
     * 没有 video_ssrc 时退回实际收到的第一个视频 SSRC。 */
    {
        uint32_t media = s->cfg.video_ssrc ? s->cfg.video_ssrc : s->stats.vid_ssrc_seen;
        cm_wr32(pkt + 8, media);
    }

    /* SRTCP 加密：与 PLI / RR 完全同一条路径。媒体面整体是 SRTP，
     * 明文 RTCP 会被网关丢弃（cm_cmd.c 里 PLI 的注释已经踩过这个坑）。
     *
     * ★ N1 修正续：加密失败时**不发**，而不是退回明文。
     * 明文 RTCP 在这个会话里 100% 被丢，发出去只会污染统计
     * （vid_nack_sent 涨、nack_recovered 永远是 0，看起来像「网关不重传」，
     * 实际是我们根本没把请求送到）。失败就认，下一轮再试。 */
    if (cm_srtcp_ready(s)) {
        enc = cm_srtcp_protect(s, pkt, len, (int)sizeof(pkt));
        if (enc <= 0) {
            CM_LOGW("#%d NACK dropped: srtcp protect failed (%d), not sending plaintext",
                    s->id, enc);
            return;
        }
        len = enc;
        trans = "srtcp";
    } else {
        /* DTLS-SRTP 还没 ready（极少见：通常是刚起会话的头几百毫秒）。
         * 这时明文是唯一选择，也确实是网关能接受的形态（PLI 也这么干）。 */
        trans = "plain";
    }

    if (cm_udp_send(s->sock, pkt, len, s->cfg.remote_ip, s->cfg.remote_port) <= 0) {
        CM_LOGW("#%d NACK send failed", s->id);
        return;
    }

    s->stats.vid_nack_sent++;
    s->stats.vid_nack_req += (uint64_t)used;

    if (s->nack_log_ms == 0 || (now - s->nack_log_ms) >= NACK_LOG_INTERVAL_MS) {
        s->nack_log_ms = now;
        CM_LOGI("#%d NACK sent (%d bytes, %s) gaps=%d idr=%d fci=%d covered=%d",
                s->id, len, trans, n, n_idr, nfci, used);
    }
}

int cm_cmd_apply(cm_sess *s, const char *op, const char *json) {
    if (!s || !op) return -1;

    if (strcmp(op, "stats") == 0) {
        cm_emit(s,
                "{\"t\":\"stat\",\"rx\":%llu,\"lost\":0,\"jitterMs\":0,\"bitrateKbps\":0,"
                "\"decode\":%llu,\"frames\":%llu,\"authFail\":%llu,\"bad\":%llu,"
                "\"kf\":%llu,\"pli\":%llu,\"w\":%d,\"h\":%d,\"fps\":%d}",
                (unsigned long long)s->stats.rtp_rx,
                (unsigned long long)s->stats.nal_out,
                (unsigned long long)s->stats.frames,
                (unsigned long long)s->stats.srtp_auth_fail,
                (unsigned long long)s->stats.rtp_bad,
                (unsigned long long)s->kf_count,
                (unsigned long long)s->pli_count,
                s->video_w, s->video_h, s->fps);
        return 0;
    }

    if (strcmp(op, "setRect") == 0) {
        int x, y, w, h;
        char *r = cm_json_get_obj(json, "rect");
        const char *src = r ? r : json;    /* 嵌套优先，扁平兜底 */
        x = cm_json_get_int(src, "x", s->cfg.rect_x);
        y = cm_json_get_int(src, "y", s->cfg.rect_y);
        w = cm_json_get_int(src, "w", s->cfg.rect_w);
        h = cm_json_get_int(src, "h", s->cfg.rect_h);
        free(r);
        if (w <= 0) w = s->cfg.rect_w;
        if (h <= 0) h = s->cfg.rect_h;
        s->cfg.rect_x = x;
        s->cfg.rect_y = y;
        s->cfg.rect_w = w;
        s->cfg.rect_h = h;
        pthread_mutex_lock(&s->gst_mu);
        if (s->vbox) cm_box_set_rect(s->vbox, x, y, w, h);
        pthread_mutex_unlock(&s->gst_mu);
        cm_emit(s, "{\"t\":\"rect\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", x, y, w, h);
        return 0;
    }

    if (strcmp(op, "requestKeyFrame") == 0 || strcmp(op, "pli") == 0) {
        cm_send_pli(s);
        return 0;
    }

    if (strcmp(op, "setLogLevel") == 0) {
        int lv = cm_json_get_int(json, "level", 3);
        cm_log_set_level(lv);
        s->cfg.log_level = lv;
        return 0;
    }

    if (strcmp(op, "setAudio") == 0 || strcmp(op, "setAudioEnabled") == 0) {
        int on = cm_json_get_int(json, "on", 1);
        pthread_mutex_lock(&s->gst_mu);
        if (!on && s->abox) {
            cm_box_destroy(s->abox);
            s->abox = NULL;
            s->audio_ok = 0;
            /* ★ Bug5：cm_box_destroy 的音频分支已经还原过全局 mixer，
             * 这里再兜一次纯粹为了「盒子其实早就没了、但会话上的快照标记
             * 还挂着」这种边角情况（例如建盒中途失败）。函数幂等，
             * 没快照过就直接返回，重复调用没有任何副作用。 */
            cm_audio_route_restore(s);
        } else if (on && !s->abox) {
            s->abox = cm_box_create(s, 0);
        }
        pthread_mutex_unlock(&s->gst_mu);
        s->cfg.audio_enabled = on ? 1 : 0;
        return 0;
    }

    /* 云游戏内文本输入：临时隐藏 / 恢复视频平面。
     *
     * 为什么需要这条：KMS 视频平面 zpos=2，**盖在 Falcon 系统键盘所用的合成面之上**。
     * 远端游戏弹出输入框时网关会下发 op=input cmd="1"，我们必须弹系统键盘
     * （im.js 的 ImSession），但键盘会被视频平面整条盖住，用户根本看不见。
     *
     * 为什么隐藏用「缩到 1x1」而恢复必须「销毁重建」：
     * cm_box_set_rect 里的 render-rectangle 是只写属性，kmssink 只在 reconfigure
     * 那一刻读，所以走脉冲重配；而这是**单向棘轮** —— 缩到 1x1 之后靠放大回不去。
     * 真机上 w=h=0 会被 kmssink 当成 fullscreen（铺满 480x960 盖住整个 UI），
     * 只有 1x1 是真正不可见的合法几何。所以：
     *   隐藏 = set_rect(0,0,1,1)（apply_rect 对 <=1 的矩形不加 PANEL_X_OFFSET，
     *                            避免左侧残留 107px 条带）
     *   恢复 = 销毁 vbox 再重建（cm_box_create 会重新读 cfg.rect_*）
     * 重建后必须复位 video_ok / media_ev_sent（否则 JS 侧不会再收到 media 事件）
     * 并立刻补一次 PLI 让网关尽快发 IDR，否则恢复后会黑屏到下一个自然关键帧。 */
    if (strcmp(op, "setVideoVisible") == 0 || strcmp(op, "hideVideo") == 0 ||
        strcmp(op, "showVideo") == 0) {
        int on = cm_json_get_int(json, "on", 1);
        if (strcmp(op, "hideVideo") == 0) on = 0;
        if (strcmp(op, "showVideo") == 0) on = 1;
        pthread_mutex_lock(&s->gst_mu);
        if (!on) {
            if (s->vbox) cm_box_set_rect(s->vbox, 0, 0, 1, 1);
            s->video_hidden = 1;
            CM_LOGI("#%d video plane hidden (1x1)", s->id);
        } else {
            /* 恢复不能重建管线：新建的 appsrc 要等一个 IDR 才出画，而网关
             * 对 PLI 的响应实测补不上（真机 rx=0 frames=0 kf=0，只有每 2 秒
             * 一条 PLI 石沉大海），画面永远回不来。
             * 改用 READY 重置：不销毁 appsrc/caps，只让 kmssink 重新分配
             * plane 并重读 render-rectangle（这是放大 1x1 的唯一可靠途径，
             * 见 cm_box_reset_rect 的注释）。 */
            cm_box_reset_rect(s->vbox, s->cfg.rect_x, s->cfg.rect_y,
                              s->cfg.rect_w, s->cfg.rect_h);
            s->video_hidden = 0;
            /* READY 把管线里所有元素都 flush 了，视频侧要从下一帧重新起，
             * 所以视频就绪标志与 media 事件闸门都复位，让 JS 重新拿到出画事件。 */
            s->video_ok = 0;
            s->media_ev_sent = 0;
            /* READY 之后解码器要从新的 IDR 重新起，把关键帧标志也清掉，
             * 否则 cm_kf_tick 以为已经有过关键帧、不再补 PLI。 */
            s->have_kf = 0;
            s->kf_count = 0;
            /* 让 cm_kf_tick 重走「首流连发 3 次 PLI」的突发节奏：
             * 单发一次常常赶不上网关的关键帧调度，连发更容易被补上。 */
            s->pli_burst = 0;
            s->next_pli_ms = 0;
            CM_LOGI("#%d video plane shown (READY reset)", s->id);
        }
        pthread_mutex_unlock(&s->gst_mu);
        if (on) cm_send_pli(s);
        cm_emit(s, "{\"t\":\"videovis\",\"on\":%d}", on ? 1 : 0);
        return 0;
    }

    CM_LOGW("#%d unknown cmd op '%s'", s->id, op);
    return -1;
}
