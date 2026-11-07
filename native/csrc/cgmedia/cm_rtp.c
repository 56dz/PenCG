/*
 * cm_rtp.c —— RTP 拆包（H.264 分片重组 + Opus 直通）+ keyframe 跟踪
 *
 * H.264 over RTP（RFC 6184）三种情况：
 *   1) 单 NAL：payload 第 1 字节就是 NAL 头，整包直接送
 *   2) STAP-A（nal_type 24）：跳过 1 字节后循环 [2 字节长度][NAL]
 *   3) FU-A（nal_type 28）：1 字节 FU indicator + 1 字节 FU header
 *        fu_indicator & 0xE0  -> 原 NAL 头的 F|NRI 位
 *        fu_header & 0x1F     -> 原 NAL type
 *        fu_header & 0x80     -> start，& 0x40 -> end
 *      start 时补回 NAL 头 (fu_indicator & 0xE0) | (fu_header & 0x1F)，
 *      end 时整帧 flush 到 appsrc。
 *
 * 一个 NAL 前面要加 Annex-B 起始码 00 00 00 01，因为 appsrc 的 caps 是
 * stream-format=byte-stream。SPS/PPS 也直接送，h264parse 会把它们归入 codec_data。
 *
 * 另外从 SPS 里解出真实分辨率（1280x720 之类），用于 media 事件和统计。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm_session.h"

/* 一帧最大 4MB（1080p key frame 足够） */
#define FRAME_CAP (4 * 1024 * 1024)

typedef struct {
    cm_buf frame;   /* 正在拼的帧（含 Annex-B 起始码） */
    int in_fu;
    int have_data;
    uint32_t last_ts;
} rtp_ctx;

static rtp_ctx g_rtp; /* 单会话足够：cgmedia 一次只跑一路云游戏 */
static pthread_mutex_t g_rtp_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_rtp_init = 0;

/* ==================== SPS 解析（拿真实分辨率） ==================== */

typedef struct {
    const unsigned char *p;
    int bits;
    int pos;
    int err;
} bitr;

static int br_u(bitr *b, int n) {
    int v = 0, i;
    if (b->err) return 0;
    if (b->pos + n > b->bits) { b->err = 1; return 0; }
    for (i = 0; i < n; i++) {
        int byte = b->p[(b->pos + i) >> 3];
        int bit = (byte >> (7 - ((b->pos + i) & 7))) & 1;
        v = (v << 1) | bit;
    }
    b->pos += n;
    return v;
}

static unsigned int br_ue(bitr *b) {
    int zeros = 0;
    unsigned int v;
    while (!b->err && br_u(b, 1) == 0) {
        zeros++;
        if (zeros > 32) { b->err = 1; return 0; }
    }
    if (zeros == 0) return 0;
    v = (unsigned int)br_u(b, zeros);
    return (unsigned int)((1u << zeros) - 1u + v);
}

static int br_se(bitr *b) {
    unsigned int u = br_ue(b);
    return (u & 1) ? (int)((u + 1) / 2) : -(int)(u / 2);
}

/* 去掉 H.264 防竞争字节 00 00 03 -> 00 00 */
static int unescape_rbsp(const unsigned char *src, int n, unsigned char *dst, int cap) {
    int i, o = 0, zeros = 0;
    for (i = 0; i < n; i++) {
        if (zeros >= 2 && src[i] == 0x03) { zeros = 0; continue; }
        if (o >= cap) break;
        dst[o++] = src[i];
        if (src[i] == 0) zeros++; else zeros = 0;
    }
    return o;
}

/* 解析 SPS NAL（含 1 字节 NAL 头），返回 0 成功 */
/* out_profile / out_level 可传 NULL。
 * 为什么要导出 profile：换画质时分辨率恒为 1280x720（真机日志实测），
 * 所以「马赛克/纯绿只出现在非流畅档」只能从码流参数变化上找原因 ——
 * 最可疑的就是 profile 从 baseline(66) 变成 high(100)，而 mppvideodec
 * 仍按旧 SPS 初始化。 */
static int sps_parse_size(const unsigned char *nal, int n, int *out_w, int *out_h,
                          int *out_profile, int *out_level) {
    unsigned char rbsp[512];
    bitr b;
    int profile, level = 0, chroma = 1, frame_mbs_only, w, h;
    unsigned int w_mbs, h_units;

    if (n < 4 || (nal[0] & 0x1F) != 7) return -1;
    n = unescape_rbsp(nal + 1, n - 1, rbsp, (int)sizeof(rbsp));
    if (n < 3) return -1;

    b.p = rbsp; b.bits = n * 8; b.pos = 0; b.err = 0;
    profile = br_u(&b, 8);
    br_u(&b, 8);            /* constraint flags */
    level = br_u(&b, 8);    /* level_idc */
    br_ue(&b);              /* seq_parameter_set_id */

    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        int v = 0;
        chroma = (int)br_ue(&b);
        if (chroma == 3) v = br_u(&b, 1);   /* separate_colour_plane_flag */
        (void)v;
        br_ue(&b);          /* bit_depth_luma_minus8 */
        br_ue(&b);          /* bit_depth_chroma_minus8 */
        br_u(&b, 1);        /* qpprime_y_zero_transform_bypass_flag */
        if (br_u(&b, 1)) {  /* seq_scaling_matrix_present_flag */
            int i, lists = (chroma != 3) ? 8 : 12;
            for (i = 0; i < lists && !b.err; i++) {
                if (br_u(&b, 1)) { /* scaling_list_present */
                    int j, size = (i < 6) ? 16 : 64;
                    int last = 8, next = 8;
                    for (j = 0; j < size && !b.err; j++) {
                        if (next != 0) next = (last + br_se(&b) + 256) % 256;
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
    }
    br_ue(&b);              /* log2_max_frame_num_minus4 */
    {
        unsigned int poc_type = br_ue(&b);
        if (poc_type == 0) {
            br_ue(&b);      /* log2_max_pic_order_cnt_lsb_minus4 */
        } else if (poc_type == 1) {
            int i;
            br_u(&b, 1);    /* delta_pic_order_always_zero_flag */
            br_se(&b);      /* offset_for_non_ref_pic */
            br_se(&b);      /* offset_for_top_to_bottom_field */
            {
                unsigned int num = br_ue(&b);
                if (num > 255) { return -1; }
                for (i = 0; i < (int)num; i++) br_se(&b);
            }
        }
    }
    br_ue(&b);              /* max_num_ref_frames */
    br_u(&b, 1);            /* gaps_in_frame_num_value_allowed_flag */
    w_mbs = br_ue(&b) + 1;
    h_units = br_ue(&b) + 1;
    frame_mbs_only = br_u(&b, 1);
    if (!frame_mbs_only) br_u(&b, 1);   /* mb_adaptive_frame_field_flag */
    br_u(&b, 1);                        /* direct_8x8_inference_flag */
    w = (int)w_mbs * 16;
    h = (int)h_units * 16 * (2 - frame_mbs_only);

    if (br_u(&b, 1)) {                  /* frame_cropping_flag */
        unsigned int l = br_ue(&b), r = br_ue(&b), t = br_ue(&b), bo = br_ue(&b);
        int cw = (chroma == 1 || chroma == 2) ? 2 : 1;
        int ch = (chroma == 1) ? 2 : 1;
        int sub = frame_mbs_only ? 1 : 2;
        if (!b.err) {
            w -= (int)(l + r) * cw;
            h -= (int)(t + bo) * ch * sub;
        }
    }
    if (b.err || w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_profile) *out_profile = profile;
    if (out_level) *out_level = level;
    return 0;
}

/* ==================== 帧拼装 ==================== */

static void annexb_put(cm_buf *b, const unsigned char *nal, int n) {
    static const unsigned char sc[4] = {0, 0, 0, 1};
    cm_buf_put(b, sc, 4);
    cm_buf_put(b, nal, (size_t)n);
}

/* SPS 变化通知。
 *
 * 真机事实（2026-11-07 日志，逐次换档都对过）：换画质时 SPS 报的**分辨率恒为
 * 1280x720**，所以「档位越高分辨率越高」这个假设是错的 —— 马赛克/纯绿不是
 * 尺寸问题。剩下的唯二可能就是 (a) profile/level 变了而解码器仍按旧 SPS 初始化、
 * (b) 换档瞬间码流出现断点而 mppvideodec 没等到新 IDR。
 *
 * 所以这里做两件事：
 *   1. 把 profile/level 打进日志（以前只打宽高，profile 被丢弃，导致根本看不到变化）；
 *   2. profile/level/分辨率任一发生变化时，把 have_kf 清零并重发 PLI 突发，
 *      逼网关立刻给一个带新 SPS 的 IDR —— 这是「切档后画面花掉」最可能的解药。
 */
static void note_sps(cm_sess *s, int w, int h, int prof, int lev) {
    int changed;
    if (!s) return;
    changed = (s->sps_profile && (s->sps_profile != prof || s->sps_level != lev));
    s->sps_profile = prof;
    s->sps_level = lev;
    s->video_w = w;
    s->video_h = h;
    pthread_mutex_lock(&s->gst_mu);
    if (s->vbox) cm_box_set_caps_video(s->vbox, w, h);
    pthread_mutex_unlock(&s->gst_mu);
    CM_LOGI("#%d SPS reports %dx%d profile=%d level=%d%s", s->id, w, h, prof, lev,
            changed ? " (CHANGED)" : "");
    if (changed) {
        /* 换档：解码器必须从新的 SPS+PPS+IDR 重新起。清掉 have_kf 让 cm_kf_tick
         * 重走「连发 3 次 PLI」的突发节奏，否则它会以为关键帧还新鲜、不再要。 */
        s->have_kf = 0;
        s->kf_count = 0;
        s->pli_burst = 0;
        s->next_pli_ms = 0;
        CM_LOGW("#%d SPS profile changed (%d -> %d), request fresh IDR", s->id, 0, prof);
    }
}

static void mark_keyframe(cm_sess *s) {
    s->kf_count++;
    s->have_kf = 1;
    s->last_kf_ms = cm_now_ms();
}

/* media 事件：首个视频帧上屏后立刻发（带分辨率 + fps），之后不再重复。
 *
 * ★ 这里原来要求「音频也 ready」才发（if (want_audio && !s->audio_ok) return;），
 *   真机踩过：第五人格那一局网关只发视频不发音频包（日志里没有
 *   "first audio packet pushed"），于是 media 事件永远发不出去，
 *   JS 侧 vizOn 一直是 false —— 而 play.vue 的 onTS 第一行就是
 *   `if (!this.vizOn) return;`，结果玩家点击**全部被静默丢弃**，
 *   表现为「画面在播但怎么点都没反应」。
 *
 *   画面能不能看只取决于视频；音频是附带的。所以现在只等 video_ok，
 *   audio 只作为事件里的布尔标志上报，不再阻塞。
 *   若之后音频才就绪、而首次上报的是 false，允许再补发一次（见 audio 分支）。 */
static void emit_media_if_needed(cm_sess *s) {
    if (!s->video_ok) return;
    /* media_ev_sent: 0=没发过 1=发过但那时没音频 2=已带音频发过
       音频晚到时补发一次，让 JS 侧把「静音」标志纠正过来。 */
    if (s->media_ev_sent == 2) return;
    if (s->media_ev_sent == 1 && !s->audio_ok) return;
    s->media_ev_sent = s->audio_ok ? 2 : 1;
    cm_emit(s,
            "{\"t\":\"media\",\"video\":true,\"audio\":%s,\"videoW\":%d,\"videoH\":%d,\"fps\":%d}",
            s->audio_ok ? "true" : "false",
            s->video_w > 0 ? s->video_w : 1280,
            s->video_h > 0 ? s->video_h : 720,
            s->fps > 0 ? s->fps : 60);
}

/* 把攒好的帧推给视频 box */
static void flush_frame(cm_sess *s, int key) {
    if (!g_rtp.have_data || g_rtp.frame.len == 0) return;
    if (key) mark_keyframe(s);
    /* gst_mu 保护：JS 线程的 setVideoVisible(0/1) 会 set_rect 甚至销毁重建 vbox，
     * 收包线程这时正好在 push —— 不加锁就是 use-after-free。 */
    pthread_mutex_lock(&s->gst_mu);
    if (s->vbox) {
        if (cm_box_push(s->vbox, g_rtp.frame.p, (int)g_rtp.frame.len,
                        (uint64_t)g_rtp.last_ts * 1000000ULL / 90, key) == 0) {
            s->stats.frames++;
            if (!s->video_ok) {
                s->video_ok = 1;
                CM_LOGI("#%d first video frame pushed (%llu bytes, %dx%d, key=%d)",
                        s->id, (unsigned long long)g_rtp.frame.len,
                        s->video_w, s->video_h, key);
            }
        } else {
            CM_LOGW("#%d appsrc push failed", s->id);
        }
    }
    pthread_mutex_unlock(&s->gst_mu);
    s->stats.nal_out++;
    cm_buf_reset(&g_rtp.frame);
    g_rtp.have_data = 0;
    g_rtp.in_fu = 0;
    emit_media_if_needed(s);
}

static void handle_h264(cm_sess *s, const unsigned char *p, int n, int marker) {
    int nal_type;
    if (n < 1) return;
    nal_type = p[0] & 0x1F;

    if (nal_type == 24) { /* STAP-A */
        int o = 1;
        while (o + 2 <= n) {
            int sz = (p[o] << 8) | p[o + 1];
            o += 2;
            if (sz <= 0 || o + sz > n) break;
            if ((p[o] & 0x1F) == 7) {
                int w = 0, h = 0, prof = 0, lev = 0;
                if (sps_parse_size(p + o, sz, &w, &h, &prof, &lev) == 0) {
                    note_sps(s, w, h, prof, lev);
                }
            }
            annexb_put(&g_rtp.frame, p + o, sz);
            g_rtp.have_data = 1;
            o += sz;
        }
        if (marker) flush_frame(s, 1);
        return;
    }

    if (nal_type == 28) { /* FU-A */
        unsigned char fu_ind, fu_hdr;
        int start, end, typ;
        if (n < 3) return;
        fu_ind = p[0];
        fu_hdr = p[1];
        start = (fu_hdr & 0x80) != 0;
        end = (fu_hdr & 0x40) != 0;
        typ = fu_hdr & 0x1F;

        if (start) {
            unsigned char nh = (unsigned char)((fu_ind & 0xE0) | typ);
            static const unsigned char sc[4] = {0, 0, 0, 1};
            if (g_rtp.in_fu) cm_buf_reset(&g_rtp.frame); /* 上一帧丢了尾巴 */
            cm_buf_put(&g_rtp.frame, sc, 4);
            cm_buf_put(&g_rtp.frame, &nh, 1);
            g_rtp.in_fu = 1;
            g_rtp.have_data = 1;
            /* 关键帧计数只在 flush_frame 里做，避免 start + flush 双计 */
        }
        if (!g_rtp.in_fu) return;            /* 丢了 start，丢弃后续分片 */
        if (n > 2) cm_buf_put(&g_rtp.frame, p + 2, (size_t)(n - 2));
        if (end) {
            g_rtp.in_fu = 0;
            flush_frame(s, typ == 5);
        }
        return;
    }

    if (nal_type == 25 || nal_type == 26 || nal_type == 27) {
        /* STAP-B / MTAP / FU-B 在 packetization-mode=1 的网关里不会出现 */
        return;
    }

    if (nal_type == 7) {
        int w = 0, h = 0, prof = 0, lev = 0;
        if (sps_parse_size(p, n, &w, &h, &prof, &lev) == 0) {
            note_sps(s, w, h, prof, lev);
        }
    }

    /* 单 NAL（含 SPS=7 / PPS=8 / 普通切片 1、5） */
    annexb_put(&g_rtp.frame, p, n);
    g_rtp.have_data = 1;
    if (marker) flush_frame(s, nal_type == 5); /* flush_frame 内部记账关键帧 */
}

/* ---------------- 对外入口 ---------------- */

void cm_rtp_on_packet(cm_sess *s, unsigned char *pkt, int len, const char *from_ip,
                      int from_port) {
    int hlen, pt, marker;
    const unsigned char *payload;
    int plen;
    uint32_t ssrc;
    (void)from_ip;
    (void)from_port;

    if (!s || len < 12) return;
    if ((pkt[0] & 0xC0) != 0x80) return;

    hlen = 12 + (pkt[0] & 0x0f) * 4;
    if ((pkt[0] & 0x10) && len >= hlen + 4) {
        int ext_words = (pkt[hlen + 2] << 8) | pkt[hlen + 3];
        hlen += 4 + ext_words * 4;
    }
    if (hlen >= len) return;

    marker = (pkt[1] & 0x80) != 0;
    pt = pkt[1] & 0x7F;
    ssrc = cm_rd32(pkt + 8);
    payload = pkt + hlen;
    plen = len - hlen;

    s->stats.rtp_rx++;
    s->stats.bytes_rx += (uint64_t)len;
    if (pt < 128) s->stats.pt_hist[pt]++;

    if (pt == s->cfg.video_pt) {
        if (s->cfg.video_ssrc && ssrc != s->cfg.video_ssrc) return;
        pthread_mutex_lock(&g_rtp_mu);
        if (!g_rtp_init) { cm_buf_init(&g_rtp.frame); g_rtp_init = 1; }
        g_rtp.last_ts = cm_rd32(pkt + 4);
        if (g_rtp.frame.len > FRAME_CAP) {
            cm_buf_reset(&g_rtp.frame);
            g_rtp.have_data = 0;
            g_rtp.in_fu = 0;
        }
        handle_h264(s, payload, plen, marker);
        pthread_mutex_unlock(&g_rtp_mu);
        return;
    }

    if (pt == s->cfg.audio_pt) {
        /* 取证：先不管 ssrc 是否匹配，前 3 个音频包都记一笔，
         * 这样能区分「网关没发音频」/「ssrc 对不上被丢」/「payload 不是 opus」。 */
        /* 只在前 3 个音频包上打一行摘要（TOC 首字节足以判断是不是裸 Opus）。 */
        if (s->stats.pt_hist[pt] <= 3) {
            CM_LOGI("#%d audio#%u pl=%d toc=%02x ssrc=%08x want=%08x",
                    s->id, s->stats.pt_hist[pt], plen, plen > 0 ? payload[0] : 0,
                    ssrc, s->cfg.audio_ssrc);
        }
        if (s->cfg.audio_ssrc && ssrc != s->cfg.audio_ssrc) return;
        if (s->abox && plen > 0) {
            if (cm_box_push(s->abox, payload, plen,
                            (uint64_t)cm_rd32(pkt + 4) * 1000000ULL / 48000, 0) == 0) {
                s->stats.audio_pkts++;
                s->stats.audio_bytes += (uint64_t)plen;
                if (!s->audio_ok) {
                    s->audio_ok = 1;
                    CM_LOGI("#%d first audio packet pushed", s->id);
                }
                emit_media_if_needed(s);
            }
        }
        return;
    }

    /* 认不出 PT：把前 4 个字节和 PT 打出来（按 PT 去重，避免刷屏），
     * 用于确认网关实际用的视频 PT 是不是 offer 里声明的那个。 */
    if (s->stats.rtp_bad <= 12) {
        CM_LOGW("#%d RTP unknown pt=%d (want video=%d audio=%d) ssrc=%08x len=%d hdr=%02x%02x%02x%02x",
                s->id, pt, s->cfg.video_pt, s->cfg.audio_pt, ssrc, len,
                pkt[0], pkt[1], pkt[2], pkt[3]);
    }
    s->stats.rtp_bad++;
}
