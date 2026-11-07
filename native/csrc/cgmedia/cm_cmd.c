/*
 * cm_cmd.c —— cmd(id, cmdJson) 的实现 + RTCP PLI 发送
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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm_session.h"

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
