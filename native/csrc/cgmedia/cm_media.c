/*
 * cm_media.c —— 媒体主循环
 *
 * 收包线程进入这里之后就不再回去：每轮 poll(sock, 20ms)，
 * 拿到的包先判断是不是 RTCP，然后 SRTP 解密 → RTP 拆包 → appsrc。
 * 每 2 秒吐一个 stat 事件（ARCH.md 的 stat schema）。
 * 顺带跑 keyframe 看门狗（cm_kf_tick）：一直没 IDR 就发 RTCP PLI。
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm_session.h"

#define STAT_INTERVAL_MS 2000
#define FPS_WINDOW_MS 2000

/* 小程序心跳看门狗（真机必需，不是保险丝）。
 *
 * 背景：KMS 视频平面是硬件 overlay，zpos 高于小程序合成面、也高于系统桌面。
 * 只要 kmssink 还挂着 plane，最后一帧就会一直盖在系统 UI 上。
 * 而 Falcon 在小程序被切走后**不保证**回调 onHide/onUnload（真机实测：
 * `miniapp_cli start <别的 appid>` 之后日志里一条 onHide/cleanup 都没有），
 * QuickJS 的定时器也随之停摆 —— JS 侧再怎么写兜底都跑不到。
 *
 * 所以兜底只能放在 native：media.js 的 poll 循环每 120ms 调一次 js_poll()，
 * 它刷新 last_poll_ms；超过 WATCHDOG_MS 没刷新就认定小程序已经不在前台，
 * 自己把 pipeline 拆掉、把 plane 还回去，然后退出媒体循环。
 *
 * 阈值取 6 秒：正常 120ms 一次，偶发卡顿顶多几百毫秒，6 秒足够宽松，
 * 又远小于用户能察觉"画面卡住不动"的时间。 */
#define WATCHDOG_MS 60000
#define WATCHDOG_CHECK_MS 500

static void emit_stat(cm_sess *s) {
    uint64_t now = cm_now_ms();
    uint64_t dt = now - s->stat_base_ms;
    uint64_t db = s->stats.bytes_rx - s->stat_base_bytes;
    uint64_t kbps = 0;
    uint64_t df = s->stats.frames - s->fps_base_frames;
    uint64_t dms = now - s->fps_base_ms;

    if (dt > 0) kbps = (db * 8ULL) / dt;
    if (dms >= 500) {
        s->fps = (int)((df * 1000ULL) / dms);
        s->fps_base_frames = s->stats.frames;
        s->fps_base_ms = now;
    }
    cm_emit(s,
            "{\"t\":\"stat\",\"rx\":%llu,\"lost\":%llu,\"jitterMs\":0,\"bitrateKbps\":%llu,"
            "\"decode\":%llu,\"frames\":%llu,\"authFail\":%llu,\"bad\":%llu,"
            "\"kf\":%llu,\"pli\":%llu,\"w\":%d,\"h\":%d,\"fps\":%d,"
            "\"rxPkts\":%llu,\"rxStun\":%llu,\"rxKept\":%llu,\"rxDtls\":%llu,"
            "\"rxOther\":%llu,\"aPkts\":%llu,\"aBytes\":%llu}",
            (unsigned long long)s->stats.rtp_rx,
            (unsigned long long)0,
            (unsigned long long)kbps,
            (unsigned long long)s->stats.nal_out,
            (unsigned long long)s->stats.frames,
            (unsigned long long)s->stats.srtp_auth_fail,
            (unsigned long long)s->stats.rtp_bad,
            (unsigned long long)s->kf_count,
            (unsigned long long)s->pli_count,
            s->video_w, s->video_h, s->fps,
            (unsigned long long)s->stats.rx_pkts,
            (unsigned long long)s->stats.rx_stun,
            (unsigned long long)s->stats.rx_kept,
            (unsigned long long)s->stats.rx_dtls,
            (unsigned long long)s->stats.rx_other,
            (unsigned long long)s->stats.audio_pkts,
            (unsigned long long)s->stats.audio_bytes);
    /* 每 10 秒打一次 PT 直方图（只打非零项），用来确认音频到底有没有到。 */
    {
        static uint64_t last_hist_ms = 0;
        if (now - last_hist_ms >= 10000) {
            char buf[192];
            int i, off = 0;
            last_hist_ms = now;
            buf[0] = 0;
            for (i = 0; i < 128 && off < 160; i++) {
                if (s->stats.pt_hist[i])
                    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                                    "%d:%u ", i, s->stats.pt_hist[i]);
            }
            CM_LOGI("#%d pt histogram %s(want v=%d a=%d)", s->id, buf,
                    s->cfg.video_pt, s->cfg.audio_pt);
        }
    }
    s->stat_base_ms = now;
    s->stat_base_bytes = s->stats.bytes_rx;
    s->last_stat_ms = now;
}

void cm_media_loop(cm_sess *s) {
    unsigned char buf[2048];
    char ip[64];
    int port;
    int n;

    /* 进循环前先建好 GStreamer 盒子（kmssink 一上来就要占住 KMS 平面） */
    pthread_mutex_lock(&s->gst_mu);
    s->vbox = cm_box_create(s, 1);
    if (!s->vbox) cm_emit(s, "{\"t\":\"error\",\"msg\":\"video pipeline create failed\",\"code\":-4}");
    if (s->cfg.audio_enabled) {
        s->abox = cm_box_create(s, 0);
        if (!s->abox) cm_emit(s, "{\"t\":\"error\",\"msg\":\"audio pipeline create failed\",\"code\":-5}");
    }
    pthread_mutex_unlock(&s->gst_mu);

    s->stat_base_ms = cm_now_ms();
    s->stat_base_bytes = 0;
    s->fps_base_ms = s->stat_base_ms;
    s->fps_base_frames = 0;
    s->next_pli_ms = 0;
    /* 心跳基准：进循环时先认一次「JS 还活着」，否则看门狗会在首个 poll 到来前误杀 */
    s->last_poll_ms = cm_now_ms();
    cm_emit(s, "{\"t\":\"pipelines\",\"video\":%s,\"audio\":%s}",
            s->vbox ? "true" : "false", s->abox ? "true" : "false");

    while (!s->rx_quit && !s->cancelled) {
        uint64_t now;
        int cls;
        n = 0;
        /* 统一收包：STUN keepalive 在泵里当场应答（不回网关会以为 ICE
         * 断了），DTLS 迟到记录丢掉，只把 RTP/RTCP 交上来。
         * 之前直接用 cm_udp_recv，STUN 会被当成 SRTP 解，白白计 authFail。 */
        cls = cm_udp_pump_media(s, buf, sizeof(buf), 20, ip, &port, &n);
        if (cls < 0) {
            CM_LOGW("#%d udp recv error, quitting media loop", s->id);
            break;
        }
        if (cls == 2) {
            /* RTCP 192..223（含 200-204）不解密，直接跳过 */
            CM_LOGD("#%d RTCP %d bytes pt=%d", s->id, n, n >= 2 ? buf[1] : -1);
        } else if (cls == 1 && n > 0) {
            int plen = cm_srtp_unprotect(s, buf, n);
            if (plen > 0) {
                cm_rtp_on_packet(s, buf, plen, ip, port);
            } else if (plen == -7) {
                CM_LOGD("#%d srtp auth fail", s->id);
            } else if (plen == -2 || plen == -5) {
                CM_LOGD("#%d short packet %d bytes", s->id, n);
            }
        }

        now = cm_now_ms();
        cm_kf_tick(s);
        if (now - s->last_stat_ms >= STAT_INTERVAL_MS) {
            /* 每个统计周期顺手抽一次 bus：opusdec/alsasink/kmssink 的致命错误
             * 只会出现在这里，不打出来就是「有包没声/没画」排查黑洞。 */
            if (s->abox) (void)cm_box_drain_bus(s->abox);
            if (s->vbox) (void)cm_box_drain_bus(s->vbox);
            emit_stat(s);
        }

        /* 看门狗：小程序被切走 / 被冻结后 QuickJS 定时器停摆，poll 断了。
         * 这时必须自己拆管线，否则 KMS 平面永远盖着系统 UI（用户报障的根因）。 */
        /* ★ 必须写成「now > last_poll 且差值超时」这种对无符号下溢安全的比较。
         * 踩过的坑：js_poll() 在 JS 线程里刷新 last_poll_ms，媒体线程刚取完 now
         * 之后 JS 线程正好又写了一个更大的值（同一毫秒内相差 1ms），
         * `now - last_poll_ms` 就下溢成 2^64-1，日志打印
         * `JS heartbeat lost (18446744073709551615 ms)` 并立刻拆掉正在玩的对局。
         * 这是 volatile 不够用的典型竞态——跨线程读一个 64 位值必须防下溢。 */
        if (s->last_poll_ms && now > s->last_poll_ms &&
            (now - s->last_poll_ms) > WATCHDOG_MS) {
            CM_LOGW("#%d JS heartbeat lost (%llu ms), tearing down pipeline + plane",
                    s->id, (unsigned long long)(now - s->last_poll_ms));
            cm_emit(s, "{\"t\":\"error\",\"msg\":\"JS heartbeat lost, session torn down\",\"code\":-6}");
            break;
        }
    }

    /* 退出：拆 pipeline，释放 KMS 平面 */
    pthread_mutex_lock(&s->gst_mu);
    if (s->vbox) { cm_box_destroy(s->vbox); s->vbox = NULL; }
    if (s->abox) { cm_box_destroy(s->abox); s->abox = NULL; }
    pthread_mutex_unlock(&s->gst_mu);
    cm_emit(s, "{\"t\":\"stop\",\"code\":0}");
    CM_LOGI("#%d media loop exit", s->id);
}
