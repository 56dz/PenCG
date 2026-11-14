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

/* 丢包触发的 PLI 最小间隔，防打爆网关（cm_kf_tick 的稳态节奏是 2 秒，
 * 这里要的是「一丢包立刻补一发」，两者互补） */
#define KF_REQ_MIN_MS 300
/* 前 N 条丢包日志详细打，之后每 100 条打一条，避免刷屏把真事件埋掉 */
#define LOSS_LOG_MAX 20
/* 连续丢多少个 AU 就升级为「强制等新 IDR」（1 个不算，单次丢包很常见） */
#define DROP_STREAK_FORCE_KF 3
/* ---- RTCP RR（bug1）统计用常量 ----
 * RR_MAX_DROPOUT：RFC 3550 A.1 的 MAX_DROPOUT。16 位序号差小于这个值就认为
 *   是「同一个 65536 周期内的正常前进」，否则当迟到/乱序包，不推进最高序号。
 * VIDEO_CLOCK_HZ：H.264 的 RTP 时钟固定 90000，抖动计算要把到达时刻换算到
 *   这个单位上（RFC 3550 A.8 的 transit 定义）。 */
#define RR_MAX_DROPOUT 3000u
#define VIDEO_CLOCK_HZ 90000u
/* AUDIO_CLOCK_HZ：Opus 的 RTP 时钟是 48000（RFC 7587 §4.1），抖动要换算到
 * 这个单位上。**不能**照抄视频的 90000 —— 混用会让音频 jitter 报出 1.875 倍
 * 的假值，网关据此做的 playout 调整就是错的。 */
#define AUDIO_CLOCK_HZ 48000u
/* 注意：这里**故意不做**「等 IDR 超时就放行 P 帧」的兜底。
 *
 * 试过、否掉了：没有 IDR 就没有参考帧，放行的 P 帧解出来必然是满屏绿块/花屏
 * —— 正好是 bug 4 用户报的那个症状，等于用「画面在动」换「画面全废」。
 * 现在的取舍是：没有完整 IDR 时宁可保持上一帧/黑场（冻结），也不往解码器里
 * 塞无参考帧的 P 帧。代价是「网关彻底不理 PLI 时会一直冻结」，此时靠
 * cm_media.c 的 "still no IDR ... gateway may be ignoring PLI" 告警定位，
 * 属于网关侧问题，不是本端能修的（见文档「遗留风险」第 1 条）。 */
/* ↑ 上面那段是 2026-11-12 的判断，**已被 2026-11-13 高铁现场证据推翻一半**。
 *
 * 现场（原神 yssy，高铁 Wi-Fi，丢包 5-8%）：
 *   `still no IDR (rx=74418 kfReq=468 gateDrop=15511 auDrop=401)`
 * 网关并没有不理我们（kfReq=468 说明 PLI 一直在发），是**每个 IDR 都被丢包
 * 打碎**：720p 的 IDR 有 30~60 个 RTP 包，5-8% 丢包率下几乎必缺一片，
 * 缺一片就整帧报废 → 再要一个 IDR → 再被撕。死循环。
 *
 * 于是「宁可冻结」变成了「永久冻结」：用户报的是「画面停了」，一停就是几分钟。
 * 结论：**冻结不能无限期**。现在的策略是三层：
 *   1) RTCP Generic NACK（cm_cmd.c 的 cm_nack_tick）把缺的包点名要回来，
 *      IDR AU 额外给一段 hold 窗口，让重传赶在丢弃之前到 —— 解决大多数；
 *   2) 坏掉的 P 帧不再清 have_kf（见 flush_frame），把「1 个坏 AU → 39 帧全丢」
 *      变回「只丢 1 帧」—— 这条收益最大；
 *   3) 真的连续 IDR_STARVE_RELAX_MS 拿不到任何 IDR 时，**放行 P 帧**：
 *      接受花屏，换回画面在动。花屏会被下一个 IDR 自愈，冻结不会。
 * 第 3 条只在 1、2 都救不回时触发，所以 vid_relax 长期为 0 是好现象。 */

/* IDR 饥饿兜底的放行阈值。
 *
 * 取值理由（3000~5000 之间权衡）：
 *   - 太短（<1s）：弱网下正常的「IDR 正在路上」会被误判，反而把好画面换成花屏；
 *     一个 IDR 在 5-8% 丢包下往往要重试 2~4 轮 NACK + 1~2 次 PLI 才拼全，
 *     实测耗时 1~3 秒。
 *   - 太长（>8s）：用户已经认定「卡死了」并开始戳屏幕。
 *   4000ms 覆盖了「NACK 两轮 + PLI 一次」的最坏情况，又远小于用户忍耐极限。 */
#define IDR_STARVE_RELAX_MS 4000

/* P1-7 修正（REVIEW-report P1-7）：饥饿放行后的「重新收紧」期限。
 *
 * 放行是用花屏换「画面在动」，但若网关此后只发 P 帧且都不坏，
 * have_kf 会永远是 1、闸门永久打开 —— 那就是无限花屏，
 * 而不是「用花屏换动画」。所以给放行一个上限：到期仍没有完整 IDR
 * 就重新关闸（会再冻结，但不会无限花屏），并再发一次 PLI。
 *
 * 10000ms 的依据：要比 IDR_STARVE_RELAX_MS(4s) 明显长——否则就变成
 * 「冻 4s → 放 10s → 再冻 4s」的频繁抖动；同时它要能覆盖
 * 「网关收到 PLI → 重建 GOP → IDR 到达」的最坏情况（通常 < 2s）。
 * 真机上若看到 relax 与收紧反复交替，说明网关根本不给 IDR，
 * 那就不是客户端能修的了。 */
#define RELAX_RESET_MS 10000

/* ---------- RTCP Generic NACK（RFC 4585 §6.2.1，PT=205 FMT=1） ---------- */

/* 待重传缺口表容量。256 条 ≈ 5-8% 丢包率下 2~3 秒的缺口量
 * （30fps × 40 包/帧 × 0.06 ≈ 72 包/秒），够覆盖好几轮重试。 */
#define NACK_GAP_MAX 256
/* 每个缺口最多发几轮重传请求。IDR 给 5 轮（丢一个 IDR 要赔 39 个 P 帧，
 * 值得多花几次请求），普通 P 帧 3 轮（丢一帧只赔 33ms，不值得纠缠）。
 * 轮数放在优先级里派生，不再单独存字段。 */
#define NACK_ROUNDS_P   3
#define NACK_ROUNDS_IDR 5
/* 缺口记录的存活时间：超过就认为网关不会重传（或重传也来不及了），
 * 从表里删掉，避免无限攒垃圾。300ms 约覆盖 9 帧，足够 NACK 往返两轮。 */
#define NACK_GAP_TTL_MS 300
/* 优先级：2 = 属于 IDR AU / 携带 SPS/PPS，1 = 普通 P 帧缺口。 */
#define NACK_PRIO_IDR 2
#define NACK_PRIO_P   1

/* IDR AU 在 marker 之后额外等待重传的窗口。
 *
 * 这是 NACK 真正能救回 IDR 的关键：一个 AU 的所有分片是连着到的，marker
 * 一到就可以 flush；被 NACK 点名的那几个包最快也要一个 RTT（云游戏网关
 * 通常 10~40ms）才回来。所以对 IDR 单独压住 30ms 不放，让补包有机会落位。
 * 只对 IDR 生效：P 帧压 30ms 就是纯延迟，丢一帧才 33ms，不值。 */
#define IDR_HOLD_MS 30

/* 当前 AU 的分片索引容量：一个 720p IDR 约 30~60 个包，1080p 大 IDR 可到
 * 数百。512 足够，超了就停止记录（退化成「不能填洞」，不影响正确性）。 */
#define AU_SEG_MAX 512
/* 当前 AU 最多同时缺多少片（用于判断「洞补完了没有」）。 */
#define AU_MISS_MAX 256


typedef struct {
    cm_buf frame;   /* 正在拼的帧（含 Annex-B 起始码） */
    int in_fu;
    int have_data;
    uint32_t last_ts;

    /* ---------- 以下为「丢包 → 半帧 → 马赛克残留」防护 ----------
     *
     * 这一整块是 bug 3 的根治点。历史代码（本文件旧版本）只按 NAL 类型
     * 拼接、从不看 RTP seq：丢一个 FU-A 中间片后，后面的片照样 append 进
     * 同一个 AU，end 时把**缺片的访问单元**推给 appsrc。mppvideodec 的
     * ignore-error 出厂是 true（skill: pipeline-video.md:262-282），解坏了
     * 不报错、直接用旧参考帧补齐错误宏块上屏 —— 用户看到的就是「上一帧的
     * 马赛克残留、局部卡住不动」。而且只要不再来 IDR，这块残留会一直留在
     * 参考帧里，所以症状是「持续」而不是「闪一帧」。
     *
     * 现在：任何缺口/乱序/重复都把当前 AU 标记为损坏，flush 时整帧丢弃，
     * 并在丢包瞬间补一次 PLI。**残缺的 AU 永远不进解码器。** */
    int      have_seq;      /* 是否已有可用序号基准 */
    uint16_t last_seq;      /* 上一个接受过的 RTP 序号 */
    int      au_broken;     /* 当前 AU 已缺片/乱序，flush 时必须整帧丢弃 */
    int      au_has_idr;    /* 当前 AU 含 IDR（NAL type 5）——只有这个才算关键帧 */
    int      au_has_sps;    /* 当前 AU 自带 SPS，不用补缓存 */
    int      au_has_pps;    /* 当前 AU 自带 PPS */
    int      au_has_slice;  /* 当前 AU 含切片（type 1/5）。只含参数集的 AU 不算一帧 */
    int      drop_streak;   /* 连续被丢弃的 AU 个数 */

    /* 最近一次 SPS / PPS（含 Annex-B 起始码）。丢包把参数集打掉时，
     * 下一个 IDR 前必须补插，否则解码器拿残缺参数集直接出绿块。 */
    cm_buf sps;
    cm_buf pps;
    int    sps_ok;
    int    pps_ok;
    cm_buf au_out;          /* 补插 SPS/PPS 时的输出暂存 */
    cm_buf au_sorted;       /* NACK 补洞后按 seq 重排的输出暂存 */

    /* ---------- 按 seq 定位的分片表（NACK 补洞的前提） ----------
     *
     * 旧版本只会「按到达顺序 append」。要让重传包能填回原位，必须记住
     * 每个分片在 frame 缓冲里的偏移，flush 时按 seq 重排再送解码器。
     *
     * 为什么不是「按序插入」：重传包永远晚于它后面的分片到达，按序插入
     * 意味着每次都要 memmove 半个 AU（一个 720p IDR 有 100KB+），在收包
     * 线程里做这个不划算。改成「先 append、flush 时插入排序」：正常情况
     * （无重传）分段表天然有序，直接走 frame 原样发送的快路径，零开销。 */
    struct {
        uint16_t seq;
        uint32_t off;   /* 在 frame 缓冲里的偏移 */
        uint32_t len;   /* 该分片字节数 */
    } segs[AU_SEG_MAX];
    int      seg_n;
    /* seg_ordered：分片是不是「按到达顺序就已经是 seq 顺序」。正常无重传时
     * 恒为 1，flush 直接送 frame（零拷贝、零排序）；一旦补洞包插进来就置 0，
     * flush 时走插入排序重建。 */
    int      seg_ordered;
    /* seg_overflow：分片表溢出（seg_n 撑满 AU_SEG_MAX）。一旦置位，flush
     * **放弃按 seq 重排**、直接用 frame 原始内容 —— 重排只输出登记过的段，
     * 溢出时重排会静默截断半个 AU（详见 au_seg_add 的注释）。 */
    int      seg_overflow;
    uint16_t au_first_seq;  /* 当前 AU 的最小/最大 seq（判定重传包归属） */
    uint16_t au_last_seq;
    int      au_have_range;
    /* au_broken_fixable：这个 AU 的损坏是「能靠重传补回来」的（缺口 / 丢片），
     * 而不是结构性的（STAP-A 长度字段越界）。前者在 au_missing 归零后可以
     * 把 au_broken 清掉继续用，后者只能整帧丢。 */
    int      au_broken_fixable;
    /* au_hold_state：IDR hold 窗口状态。0=没挂起 1=挂起等重传 2=已到期放行。
     * 见 IDR_HOLD_MS 的注释。 */
    int      au_hold_state;
    /* au_start_ms：当前 AU 的起始时刻。用来把「AU 开始之后登记的缺口」
     * 升级成 IDR 优先级 —— 缺口登记时我们还不知道这个包属于哪种 AU，
     * 等看见 IDR/SPS/PPS 才能补上优先级。 */
    uint64_t au_start_ms;
    /* 当前 AU 还缺哪些 seq（缺片补回来后从这里删）。au_missing 是计数，
     * 归零就说明「AU 其实完整了」—— 此时 au_broken 应当被清掉，
     * 而不是让一个已经被 NACK 救回来的 IDR 白白丢掉。 */
    uint16_t au_miss[AU_MISS_MAX];
    int      au_missing;
    /* IDR 的 hold 窗口：marker 到了但还在等重传时的放行时刻。
     * 0 = 没有挂起的 AU。见 IDR_HOLD_MS 的注释。 */
    uint64_t au_hold_until_ms;

    /* ---------- 待重传缺口表（收包侧写、发送侧读）----------
     * 每个条目 = 一个我们没收到、希望网关重发的 seq。带优先级和重发轮数，
     * 因为「IDR 缺一片」和「P 帧缺一片」的代价差 39 倍（真机实测），
     * 重传预算必须区别对待。 */
    struct {
        uint16_t seq;
        unsigned char prio;   /* NACK_PRIO_IDR / NACK_PRIO_P */
        unsigned char round;  /* 已经请求了几轮 */
        uint64_t t_ms;        /* 首次登记时刻（算 TTL / 判断该不该再要） */
    } gaps[NACK_GAP_MAX];
    int      gap_n;

    int sess_id;            /* 归属会话；换会话时整体复位（防跨局残留） */
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

static int annexb_put(cm_buf *b, const unsigned char *nal, int n) {
    static const unsigned char sc[4] = {0, 0, 0, 1};
    if (cm_buf_put(b, sc, 4) < 0) return -1;
    return cm_buf_put(b, nal, (size_t)n);
}

/* ---------------- 拆包状态复位 ---------------- */

/* 复位「当前访问单元」的拼装状态。注意不清 seq 基准，也不清参数集缓存。 */
static void rtp_reset_au(void) {
    cm_buf_reset(&g_rtp.frame);
    g_rtp.have_data = 0;
    g_rtp.in_fu = 0;
    g_rtp.au_broken = 0;
    g_rtp.au_has_idr = 0;
    g_rtp.au_has_sps = 0;
    g_rtp.au_has_pps = 0;
    g_rtp.au_has_slice = 0;
    /* 分片表 / 缺失表 / hold 窗口全部随 AU 一起清。
     * gap_n（待重传表）**不在这里清** —— 缺口属于「已经丢掉的那个 AU」，
     * 但重传包回来时我们仍然希望能把它补进去（哪怕只能推给解码器当参考帧
     * 碎片也无害）。让它按 NACK_GAP_TTL_MS 自然过期，比在 AU 边界一刀切更宽容。
     * 补洞成功后 gap_drop 会精确摘除。 */
    g_rtp.seg_n = 0;
    g_rtp.seg_ordered = 1;
    g_rtp.seg_overflow = 0;
    g_rtp.au_have_range = 0;
    /* ★ F1 修正（REVIEW-report F1）：au_broken_fixable / au_start_ms 必须跟着
     * AU 一起复位。旧代码清了 11 个字段却漏这两个，后果很隐蔽：
     *   上一帧因为丢包置了 au_broken_fixable=1 → AU 复位后它还在 →
     *   下一帧如果是**结构性损坏**（STAP-A 长度字段越界，只置 au_broken、
     *   不置 fixable）而 au_missing 恰好是 0，那么 flush_frame 的
     *   `if (au_broken && au_broken_fixable && au_missing == 0)` 三条全中，
     *   于是打印「AU repaired by retransmissions」并**把一个被截断的 AU
     *   推进解码器**，污染参考帧链 —— 正是 bug3/bug4 的形态。
     * au_start_ms 不清也一样：STAP-A 分支从不设它（见 handle_h264 里的补设），
     *   残留下来的旧时刻会让 gap_upgrade_since 把本来无关的缺口升成 IDR 优先级。 */
    g_rtp.au_broken_fixable = 0;
    g_rtp.au_start_ms = 0;
    g_rtp.au_missing = 0;
    g_rtp.au_hold_state = 0;
    g_rtp.au_hold_until_ms = 0;
}

/* 整会话复位。g_rtp 是进程级单例，而 cgmedia 一次只跑一路云游戏：
 * 换局/重连时必须清干净，否则上一局的 seq 基准、残缺 AU、旧 SPS 会留到
 * 新一局，症状是「新一局一开就花屏/绿屏」（bug 4 的可疑成因之一）。 */
static void rtp_reset_all(void) {
    rtp_reset_au();
    g_rtp.have_seq = 0;
    g_rtp.last_seq = 0;
    g_rtp.drop_streak = 0;
    g_rtp.sps_ok = 0;
    g_rtp.pps_ok = 0;
    g_rtp.gap_n = 0;
    g_rtp.au_hold_state = 0;
    g_rtp.au_hold_until_ms = 0;
    cm_buf_reset(&g_rtp.sps);
    cm_buf_reset(&g_rtp.pps);
    cm_buf_reset(&g_rtp.au_out);
    cm_buf_reset(&g_rtp.au_sorted);
}

/* ---------------- 待重传缺口表 ---------------- */

/* 登记一个「没收到、希望重传」的 seq。重复登记直接忽略（同一个 seq 在
 * 重传期间会被多轮 NACK 覆盖，但表里只留一条）。 */
static void gap_add(uint16_t seq, unsigned char prio) {
    int i;
    if (!g_rtp_init) return;
    for (i = 0; i < g_rtp.gap_n; i++) {
        if (g_rtp.gaps[i].seq == seq) {
            /* 已登记：若这次判定为更高优先级（比如 AU 后来才知道是 IDR），升级它 */
            if (prio > g_rtp.gaps[i].prio) g_rtp.gaps[i].prio = prio;
            return;
        }
    }
    if (g_rtp.gap_n >= NACK_GAP_MAX) {
        /* 表满：丢最老的一条。宁可漏掉一个旧缺口，也不能因为表满而
         * 拒绝登记新的 —— 新缺口更可能还来得及重传。 */
        memmove(&g_rtp.gaps[0], &g_rtp.gaps[1],
                sizeof(g_rtp.gaps[0]) * (size_t)(NACK_GAP_MAX - 1));
        g_rtp.gap_n = NACK_GAP_MAX - 1;
    }
    g_rtp.gaps[g_rtp.gap_n].seq = seq;
    g_rtp.gaps[g_rtp.gap_n].prio = prio;
    g_rtp.gaps[g_rtp.gap_n].round = 0;
    g_rtp.gaps[g_rtp.gap_n].t_ms = cm_now_ms();
    g_rtp.gap_n++;
}

/* 从缺口表里摘掉一个 seq（重传到达 / AU 已丢弃 / TTL 过期）。
 * 同时把它从当前 AU 的 au_miss[] 里摘掉，并递减 au_missing。 */
static void gap_drop(uint16_t seq) {
    int i;
    for (i = 0; i < g_rtp.gap_n; i++) {
        if (g_rtp.gaps[i].seq == seq) {
            memmove(&g_rtp.gaps[i], &g_rtp.gaps[i + 1],
                    sizeof(g_rtp.gaps[0]) * (size_t)(g_rtp.gap_n - i - 1));
            g_rtp.gap_n--;
            break;
        }
    }
    for (i = 0; i < g_rtp.au_missing; i++) {
        if (g_rtp.au_miss[i] == seq) {
            memmove(&g_rtp.au_miss[i], &g_rtp.au_miss[i + 1],
                    sizeof(g_rtp.au_miss[0]) * (size_t)(g_rtp.au_missing - i - 1));
            g_rtp.au_missing--;
            break;
        }
    }
}

/* 清掉超过 TTL 的缺口：网关要么不支持重传，要么重传也来不及了。
 * 这些 seq 留在表里只会让 NACK 报文越来越长、越来越没用。 */
static void gap_purge_expired(void) {
    uint64_t now = cm_now_ms();
    int i = 0;
    while (i < g_rtp.gap_n) {
        if (now > g_rtp.gaps[i].t_ms && (now - g_rtp.gaps[i].t_ms) > NACK_GAP_TTL_MS) {
            uint16_t sq = g_rtp.gaps[i].seq;
            /* ★ L2 修正（REVIEW-report L2）：摘缺口 + 同步摘 au_miss 这件事
             * 本来就由 gap_drop() 负责，旧代码却在这里内联重写了一份。
             * 三份重复实现正是「改一处忘另两处」的根源，现在统一走 gap_drop。
             * gap_drop 不会动 i（下面 continue 保持不增），与旧行为等价。 */
            gap_drop(sq);
            /* R5 修正（REVIEW-report R5）：只有**落在当前 AU 的 seq 范围里**的
             * 缺口过期，才说明这个 AU 确定残缺。旧代码摘掉任意过期缺口就置
             * au_broken=1，而 NACK_GAP_TTL_MS(300ms) 远大于帧间隔(33ms)：
             * 上一帧遗留的缺口一过期，就把当前这个**完整**的 AU 打成残帧整帧丢掉。
             * 判据用 au_first_seq/au_last_seq —— 它们本来就为「判定重传包归属」
             * 而存在，之前却全文件无读取者。 */
            if (g_rtp.au_have_range) {
                int16_t d0 = (int16_t)(sq - g_rtp.au_first_seq);
                int16_t d1 = (int16_t)(sq - g_rtp.au_last_seq);
                if (d0 >= 0 && d1 <= 0) g_rtp.au_broken = 1; /* 洞被放弃了，这个 AU 确定是残的 */
            }
            /* au_have_range == 0 表示此刻没有正在拼的 AU（帧间空隙）。
             * 这时**绝不能**置 au_broken：这个标记只在 flush_frame 里被消费，
             * 而 STAP-A 与单 NAL 两条分支都不会在开头清它，于是它会被带到
             * 下一个 AU 上，把一帧好画面无端判成残帧丢掉。没有 AU 就没有
             * 可损坏的对象，直接什么都不做。 */
            continue;
        }
        i++;
    }
}

/* 把一个缺口登记进当前 AU 的缺失表（用于判断「洞补完了没有」）。 */
static void au_mark_missing(uint16_t seq) {
    int i;
    if (g_rtp.au_missing >= AU_MISS_MAX) return;
    for (i = 0; i < g_rtp.au_missing; i++) {
        if (g_rtp.au_miss[i] == seq) return;
    }
    g_rtp.au_miss[g_rtp.au_missing++] = seq;
}

/* 取走缺口表给 NACK 发送侧。
 *
 * 排序：IDR 优先级（prio=2）排前面 —— 真机 401 个 auDrop 换掉 15511 个
 * P 帧，说明「IDR 被撕」才是灾难源头，重传预算必须先给它。发送侧按这个
 * 顺序打包，一旦本条 NACK 报文装满了，被挤掉的只会是低优先级的 P 帧缺口。 */
int cm_rtp_take_nack_gaps(cm_sess *s, unsigned char *out, int cap_entries) {
    int n = 0, i, pass;
    if (!s || !out || cap_entries <= 0) return 0;
    pthread_mutex_lock(&g_rtp_mu);
    gap_purge_expired();

    /* 两趟：pass=0 只出 IDR 缺口（prio=2），pass=1 出 P 帧缺口。
     * 每出一条就把它自己的重发轮数 +1；超过该优先级的轮数上限就直接从表里
     * 摘掉 —— 再要网关也不会给了，留着只会挤占别人的重传预算。
     * 这样即使 cap_entries 很小（报文长度受限），被挤掉的也总是低优先级的
     * P 帧缺口，IDR 永远排在前面。 */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < g_rtp.gap_n && n < cap_entries; i++) {
            int is_idr = (g_rtp.gaps[i].prio == NACK_PRIO_IDR);
            int maxround = is_idr ? NACK_ROUNDS_IDR : NACK_ROUNDS_P;
            if (pass == 0 && !is_idr) continue;
            if (pass == 1 && is_idr) continue;
            if (g_rtp.gaps[i].round >= (unsigned char)maxround) {
                /* 轮数用完：摘掉，并从 AU 缺失表里同步删除。
                 * ★ L2 修正：这件事本来就由 gap_drop() 负责（第三处重复实现）。
                 * gap_n-1 使下标 i 自然指向下一个元素，所以 i-- 保持不变。 */
                gap_drop(g_rtp.gaps[i].seq);
                i--;
                continue;
            }
            out[n * 4 + 0] = (unsigned char)(g_rtp.gaps[i].seq >> 8);
            out[n * 4 + 1] = (unsigned char)(g_rtp.gaps[i].seq & 0xFF);
            out[n * 4 + 2] = g_rtp.gaps[i].prio;
            out[n * 4 + 3] = g_rtp.gaps[i].round;
            g_rtp.gaps[i].round++;
            n++;
        }
        if (n >= cap_entries) break;
    }
    pthread_mutex_unlock(&g_rtp_mu);
    return n;
}

/* 缓存最近的 SPS/PPS（带 Annex-B 起始码的完整副本）。
 *
 * 为什么必须在 RTP 层缓存：网关并不保证每个 IDR 都自带 SPS/PPS ——
 * 常见形态是「开局一个 STAP-A 带 SPS+PPS，之后所有 IDR 都是裸 NAL」。
 * 一旦中途丢包把那个 STAP-A 丢了，或者在它到达之前就收到了 IDR，
 * 解码器就没有参数集可用。此时补插缓存是唯一能出画的办法。 */
static void cache_param_set(const unsigned char *nal, int n) {
    int t;
    if (n < 1) return;
    t = nal[0] & 0x1F;
    if (t == 7) {
        cm_buf_reset(&g_rtp.sps);
        g_rtp.sps_ok = (annexb_put(&g_rtp.sps, nal, n) == 0);
        g_rtp.au_has_sps = 1;
    } else if (t == 8) {
        cm_buf_reset(&g_rtp.pps);
        g_rtp.pps_ok = (annexb_put(&g_rtp.pps, nal, n) == 0);
        g_rtp.au_has_pps = 1;
    }
}

/* 丢包/乱序后立刻补一次 PLI。
 *
 * 为什么不依赖 cm_kf_tick（cm_cmd.c:55-71）：
 *   它的稳态节奏是「每 2 秒最多 1 次」，判据只有 `have_kf && now - last_kf_ms < 2000`，
 *   **完全不感知丢包**。所以丢包之后最多要等 2 秒才可能补 PLI，这期间解码器
 *   一直拿越来越旧的参考帧解，用户看到的就是「马赛克残留持续存在」。
 * 这里在丢包瞬间补一发，形成「急发 + cm_kf_tick 慢补」两层保险。
 * 限速 KF_REQ_MIN_MS：一段音频大小的丢包风暴不能把 PLI 打成 DoS。 */
static void kf_request(cm_sess *s, const char *why) {
    uint64_t now = cm_now_ms();
    if (!s) return;
    if (s->stats.vid_kf_req_ms && now > s->stats.vid_kf_req_ms &&
        (now - s->stats.vid_kf_req_ms) < KF_REQ_MIN_MS) {
        return; /* 限速中，cm_kf_tick 会在后面兜 */
    }
    s->stats.vid_kf_req_ms = now;
    s->stats.vid_kf_req++;
    CM_LOGW("#%d video gap (%s) -> request IDR (PLI #%llu)", s->id, why,
            (unsigned long long)s->stats.vid_kf_req);
    cm_send_pli(s);
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
    int old_prof, old_lev;
    if (!s) return;
    old_prof = s->sps_profile;
    old_lev = s->sps_level;
    changed = (s->sps_profile && (old_prof != prof || old_lev != lev));
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
        /* P1-7：换档后「饥饿计时 / 放行期限」都要归零重计，
         * 否则旧的 relax_until_ms 会让新档的头几帧被当成「已放行」。 */
        s->kf_starve_ms = 0;
        s->relax_until_ms = 0;
        /* ★ 旧版本这里把「旧 profile」硬编码成 0 打出来（`..., s->id, 0, prof`），
         * 于是日志永远是 "changed (0 -> 100)"，看日志根本判断不出是不是真的换了档。
         * 现在如实打印 old_prof/old_lev。 */
        CM_LOGW("#%d SPS profile changed (%d -> %d, level %d -> %d), request fresh IDR",
                s->id, old_prof, prof, old_lev, lev);
    }
}

static void mark_keyframe(cm_sess *s) {
    s->kf_count++;
    s->have_kf = 1;
    s->last_kf_ms = cm_now_ms();
    /* P1-7：真 IDR 到了，饥饿放行的收紧计时作废 —— 闸门现在有真参考帧支撑。 */
    s->relax_until_ms = 0;
    s->kf_starve_ms = 0;
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

/* ---------------- AU 流 dump（诊断用，只在开关文件存在时生效） ----------------
 *
 * 为什么需要这个：终末地「平面已绑定 + 我们每秒稳定推 29 个 AU + 解码器至少
 * 输出过一帧」但用户肉眼全黑。这时「我们喂的码流本身是不是坏的」和
 * 「mppvideodec/kmssink 这一层是不是坏的」必须切开，否则只能猜。
 * 把实际推给 appsrc 的字节原样落盘，再用设备自带 ffmpeg 解一帧，就能一刀切开：
 *   - ffmpeg 能解出正常画面 ⇒ 码流是好的，问题在 mppvideodec 这一层；
 *   - ffmpeg 也解不出/解出黑帧 ⇒ 问题在我们的 AU 组装 / SPS-PPS 注入。
 *
 * ★ 三条硬约束（lead 明确要求）：
 *   1) 必须**从完整 IDR 开始**记录。从一个 P 帧开始的码流没有参考帧，
 *      ffmpeg 也解不出来，判断会直接错。所以只有当 key==1 才开始。
 *   2) 必须与实际 cm_box_push 的 out_p/out_n **完全一致**（含注入的 SPS/PPS），
 *      所以调用点紧贴 push 成功之后，传同一个指针。
 *   3) 必须限帧数。设备 /userdata 只剩 55MB，300 帧 ≈ 3~5MB 是安全上限。
 *
 * 开关：`adb shell touch /userdisk/miniapp/data/dumpau.txt`
 * 产物：/userdisk/miniapp/data/au.h264
 * 验证：ffmpeg -f h264 -i /userdisk/miniapp/data/au.h264 -frames:v 1 /tmp/f.png
 *
 * 关掉：删开关文件 + 重启 app。dump 完 300 帧后自动停止并打一行 CM_LOGI。 */
#define AU_DUMP_SWITCH   "/userdisk/miniapp/data/dumpau.txt"
#define AU_DUMP_FILE     "/userdisk/miniapp/data/au.h264"
#define AU_DUMP_MAX_FRAMES 300
#define AU_DUMP_MAX_BYTES  (8u * 1024u * 1024u)

static int      g_dump_state = 0;   /* 0=未判定 1=关闭 2=记录中 3=已写完 */static FILE    *g_dump_fp = NULL;
static uint64_t g_dump_frames = 0;
static uint64_t g_dump_bytes = 0;

/* AU push 诊断日志的限速时间戳（1 秒一行）。放在文件级而不是 cm_sess 里，
 * 因为它是纯诊断，不值得占用会话结构、也不该进统计 JSON。 */
static uint64_t g_diag_ms = 0;

/* 每帧 AU 指纹的诊断开关状态（0=未判定 1=关闭 2=记录中）。
 * 只看 1 秒限速那行日志分不清「这一秒里每帧都一样」还是「每帧都不一样」，
 * 所以要有一个逐帧的开关。 */
static int      g_auld = 0;

/* 在 push 成功之后调用，p/n 必须就是推给 appsrc 的那一份字节。 */
static void au_dump_hook(const unsigned char *p, int n, int key) {
    if (g_dump_state == 1 || g_dump_state == 3) return;
    if (n <= 0 || !p) return;
    if (g_dump_state == 0) {
        FILE *sw = fopen(AU_DUMP_SWITCH, "r");
        if (!sw) { g_dump_state = 1; return; }   /* 没开关文件：永久关闭，零开销 */
        fclose(sw);
        g_dump_state = 2;
        CM_LOGW("AU dump enabled -> %s (start at first IDR, max %d frames / %u bytes)",
                AU_DUMP_FILE, AU_DUMP_MAX_FRAMES, (unsigned)AU_DUMP_MAX_BYTES);
    }
    if (g_dump_state != 2) return;
    /* ★ 只从关键帧开始：P 帧打头没有参考帧，ffmpeg 解不出来，判断会错。 */
    if (!g_dump_fp) {
        if (!key) return;
        g_dump_fp = fopen(AU_DUMP_FILE, "wb");
        if (!g_dump_fp) {
            CM_LOGW("AU dump: cannot open %s, disabled", AU_DUMP_FILE);
            g_dump_state = 3;
            return;
        }
        g_dump_frames = 0;
        g_dump_bytes = 0;
        CM_LOGW("AU dump: writing from IDR, %d bytes in first AU", n);
    }
    if (fwrite(p, 1, (size_t)n, g_dump_fp) != (size_t)n) {
        CM_LOGW("AU dump: short write, closing");
        fclose(g_dump_fp);
        g_dump_fp = NULL;
        g_dump_state = 3;
        return;
    }
    g_dump_frames++;
    g_dump_bytes += (uint64_t)n;
    if (g_dump_frames >= AU_DUMP_MAX_FRAMES || g_dump_bytes >= AU_DUMP_MAX_BYTES) {
        fclose(g_dump_fp);
        g_dump_fp = NULL;
        g_dump_state = 3;
        CM_LOGW("AU dump done: %llu frames, %llu bytes -> %s",
                (unsigned long long)g_dump_frames,
                (unsigned long long)g_dump_bytes, AU_DUMP_FILE);
    }
}

/* 把攒好的帧推给视频 box。
 *
 * ★ 这里现在承担三个「防残留/防绿屏」的职责，全部集中在这一处记账
 *   （skill: webrtc-realtime.md:264-266 明确要求记账只在一处做，否则
 *    FU-A start 与 flush 各记一次会把 kf 计数翻倍）：
 *
 *   1. 残缺 AU 一律不推。au_broken 置位表示这个访问单元缺了至少一个 RTP
 *      分片、或出现乱序/重复。把它推给解码器就是「上一帧马赛克残留」的
 *      直接成因 —— MPP 会拿旧参考帧补齐错误宏块，而且错误会污染参考帧，
 *      一直残留到下一个 IDR。
 *   2. 关键帧之前不推 P 帧。have_kf==0 说明这个会话还没有过任何完整 IDR，
 *      此时 P 帧引用的参考帧根本不存在，硬解出来的就是绿块/花屏
 *      （bug 4「终末地一连上就绿屏」的直接成因）。
 *   3. IDR 缺参数集时补插缓存。网关常见形态是「只有开局那个 STAP-A 带
 *      SPS+PPS，之后 IDR 全是裸 NAL」；一旦那个 STAP-A 丢了，或者新一局
 *      的 IDR 先到，解码器就没有参数集可用。
 */
static void flush_frame(cm_sess *s) {
    int key;
    const unsigned char *out_p;
    const unsigned char *base_p;
    int out_n;
    int base_n;
    int need_sps, need_pps;
    uint64_t now = cm_now_ms();

    if (!g_rtp.have_data || g_rtp.frame.len == 0) { rtp_reset_au(); return; }

    key = g_rtp.au_has_idr;

    /* --- 0) NACK 补洞成功：把「可修复」的损坏标记清掉 ---
     * au_broken_fixable 只在「缺口/丢 start」时置位；结构性损坏（STAP-A
     * 长度字段越界）置 0，那种情况补洞救不回来。 */
    if (g_rtp.au_broken && g_rtp.au_broken_fixable && g_rtp.au_missing == 0) {
        g_rtp.au_broken = 0;
        g_rtp.au_broken_fixable = 0;
        CM_LOGI("#%d AU repaired by retransmissions (%llu bytes)",
                s->id, (unsigned long long)g_rtp.frame.len);
    }

    /* --- 0.5) IDR hold：给重传留一个落位窗口 ---
     *
     * 一个 AU 的分片是连着到的，marker 一到就该 flush；但被 NACK 点名的
     * 那几个包最快也要一个 RTT 才回来。IDR 丢一片整帧就废，所以这里单独
     * 压住 IDR_HOLD_MS 不放。**只对 IDR 生效**：P 帧压 30ms 是纯延迟，
     * 丢一帧才 33ms，不值。au_hold_state：0=没挂起 1=挂起中 2=已到期放行。 */
    if (key && g_rtp.au_broken && g_rtp.au_hold_state == 0) {
        g_rtp.au_hold_state = 1;
        g_rtp.au_hold_until_ms = now + IDR_HOLD_MS;
        /* L1：升到 LOGI。这行是「IDR 正在等重传」的唯一信号，
         * 默认 log_level=3（WARN）时 CM_LOGD 不落盘，真机就完全看不到
         * 重传窗口有没有在起作用。每 IDR 最多一行，不会刷屏。 */
        CM_LOGI("#%d IDR AU incomplete (missing=%d), hold %dms for NACK retransmits",
                s->id, g_rtp.au_missing, IDR_HOLD_MS);
        return; /* 保留 AU 状态，等 cm_rtp_flush_held 或补洞完成 */
    }

    /* --- 1) 残缺 AU --- */
    if (g_rtp.au_broken) {
        s->stats.vid_au_drop++;
        if (key) {
            /* 坏掉的是 IDR：参考帧链从头就断了，必须清零重来。
             * （这是唯一「必须清 have_kf」的情况之一。） */
            s->have_kf = 0;
            s->kf_starve_ms = 0;
            s->relax_until_ms = 0;   /* P1-7：重新回到「等 IDR」，放行期限作废 */
            g_rtp.drop_streak = 0;
            CM_LOGW("#%d drop broken IDR AU (%llu bytes, missing=%d) -> request new IDR",
                    s->id, (unsigned long long)g_rtp.frame.len, g_rtp.au_missing);
            kf_request(s, "broken IDR");
        } else {
            /* ★ 坏掉的是 P 帧 —— 这是本轮最重要的语义修正。
             *
             * 旧行为：任何残缺 AU 都 drop_streak++，连续 3 个就 have_kf=0，
             * 于是后面所有正常 P 帧被闸门丢掉。真机数据（2026-11-13 高铁）：
             *   auDrop=401 / gateDrop=15511  → 平均每个坏 AU 赔掉 39 个 P 帧。
             *
             * 为什么不该一刀切：解码器手里的参考帧仍然有效，丢一帧 P 只损失
             * 33ms，下一个 P 帧用更旧的参考帧照样能解（H.264 允许前向参考），
             * 画面只会小幅卡一下。把 have_kf 清掉的代价是把后面几十帧全扔了，
             * 用户看到的是「卡死」而不是「顿一下」。
             *
             * 只有**连续**丢到 DROP_STREAK_FORCE_KF 个 P 帧 AU，才说明参考帧
             * 链真的接不上（每个坏帧都在旧参考帧上累积误差），此时才降级等 IDR。 */
            g_rtp.drop_streak++;
            /* L1：升到 LOGI（弱网下这是判断「画面为什么卡一下」的主信号），
             * 但要限速 —— 5~8% 丢包时每秒可能好几条。前 LOSS_LOG_MAX 条全打，
             * 之后每 100 次打一条，保证「有数据可看」又不刷爆 /userdata。 */
            s->stats.vid_p_drop++;
            if (s->stats.vid_p_drop <= LOSS_LOG_MAX ||
                (s->stats.vid_p_drop % 100u) == 0) {
                CM_LOGI("#%d drop broken P AU (%llu bytes, missing=%d, streak=%d, "
                        "total=%llu%s)",
                        s->id, (unsigned long long)g_rtp.frame.len, g_rtp.au_missing,
                        g_rtp.drop_streak,
                        (unsigned long long)s->stats.vid_p_drop,
                        s->have_kf ? "" : ", already waiting IDR");
            }
            if (s->have_kf && g_rtp.drop_streak >= DROP_STREAK_FORCE_KF) {
                s->have_kf = 0;
                s->kf_starve_ms = 0;
                s->relax_until_ms = 0;   /* P1-7：同上 */
                kf_request(s, "consecutive P AU drops");
            }
        }
        rtp_reset_au();
        return;
    }

    /* --- 只含参数集、不含切片的 AU：不当作一帧推 --- */
    if (!g_rtp.au_has_slice) {
        CM_LOGD("#%d parameter-set-only AU (%llu bytes), cached", s->id,
                (unsigned long long)g_rtp.frame.len);
        rtp_reset_au();
        return;
    }

    /* --- 3) 非关键帧且还没有过 IDR：闸门丢弃，但有时间上限 ---
     *
     * 2026-11-13 高铁现场：「网关不理 PLI」这个判断被推翻了 —— kfReq=468 说明
     * PLI 一直在发，是每个 IDR 都被丢包打碎。于是「宁可冻结」变成「永久冻结」，
     * 用户报「画面停了」。
     *
     * 现在的取舍变成：冻结有上限。连续 IDR_STARVE_RELAX_MS 拿不到任何 IDR，
     * 就放行 P 帧（接受花屏），并置 have_kf=1 避免下一秒又锁回去。花屏会被
     * 下一个 IDR 自愈，冻结不会。 */
    /* --- P1-7 重新收紧：饥饿放行已过期仍无 IDR，重新关闸 ---
     *
     * 放行是用花屏换「画面在动」。但它只能是**有限期**的交易：
     * 若网关此后一直只发完整的 P 帧，have_kf 就永远是 1，
     * 闸门永久打开 → 持续花屏（bug4 的形态回归，只是不冻结）。
     * 到期就关闸并补一次 PLI，让下一轮饥饿计时重新起算。 */
    if (!key && s->have_kf && s->relax_until_ms &&
        now > s->relax_until_ms && (now - s->relax_until_ms) >= 0) {
        s->have_kf = 0;
        s->relax_until_ms = 0;
        s->kf_starve_ms = 0;
        CM_LOGW("#%d relax window %.0fs expired without IDR, re-closing gate",
                s->id, (double)(RELAX_RESET_MS) / 1000.0);
        kf_request(s, "relax window expired");
    }

    if (!key && !s->have_kf) {
        if (s->kf_starve_ms && now > s->kf_starve_ms &&
            (now - s->kf_starve_ms) >= IDR_STARVE_RELAX_MS) {
            s->stats.vid_relax++;
            s->have_kf = 1;           /* 开门：后续 P 帧不再走闸门 */
            s->last_kf_ms = now;      /* 假装刚有过关键帧，避免 cm_kf_tick 立刻又刷 PLI */
            s->kf_starve_ms = 0;
            /* P1-7：记下收紧期限，到期仍无 IDR 就重新关闸 */
            s->relax_until_ms = now + RELAX_RESET_MS;
            CM_LOGW("#%d IDR starvation %.1fs, relaxing gate (accept artifacts until "
                    "next IDR, re-close in %.0fs)", s->id,
                    (double)(IDR_STARVE_RELAX_MS) / 1000.0,
                    (double)(RELAX_RESET_MS) / 1000.0);
            /* 不 return：这一帧照常往下推 */
        } else {
            if (!s->kf_starve_ms) {
                s->kf_starve_ms = now;
                CM_LOGI("#%d waiting for first IDR, dropping P frames", s->id);
                kf_request(s, "no keyframe yet");
            }
            s->stats.vid_gate_drop++;
            g_rtp.drop_streak++;
            rtp_reset_au();
            return;
        }
    }

    /* --- 2) 重传包补洞之后按 seq 重排 ---
     *
     * 正常情况（没有重传）分段表天然有序，直接用 frame，零开销。
     * 一旦有包是补洞来的（晚于后面分片到达），就必须按 seq 重排，
     * 否则解码器拿到乱序分片 = 花屏。
     *
     * 用插入排序而不是 qsort：段数 ≤512 且绝大多数 AU 根本进不来这里
     * （ordered=1 走快路径），插入排序对「几乎有序」的输入是最优的。
     *
     * ★ 顺序很重要：**必须先重排、后注入 SPS/PPS**。
     * 旧代码是先注入（写 au_out = sps+pps+frame）再重排（把 out_p 换成
     * au_sorted，而 au_sorted 只由 segs[] 拼成、不含注入的 SPS/PPS），
     * 于是「既补了洞又缺参数集」的 IDR 会**把刚注入的参数集丢掉**，
     * 裸 IDR 进解码器 —— 真机表现是 spsInject 计数在涨、画面却全黑。 */
    base_p = g_rtp.frame.p;
    base_n = (int)g_rtp.frame.len;
    if (!g_rtp.seg_ordered && g_rtp.seg_n > 1 && !g_rtp.seg_overflow) {
        int idx[AU_SEG_MAX];
        int i, j, k;
        for (i = 0; i < g_rtp.seg_n; i++) idx[i] = i;
        for (i = 1; i < g_rtp.seg_n; i++) {
            int cur = idx[i];
            j = i - 1;
            while (j >= 0 &&
                   (int16_t)(g_rtp.segs[idx[j]].seq - g_rtp.segs[cur].seq) > 0) {
                idx[j + 1] = idx[j];
                j--;
            }
            idx[j + 1] = cur;
        }
        cm_buf_reset(&g_rtp.au_sorted);
        for (k = 0; k < g_rtp.seg_n; k++) {
            int t = idx[k];
            /* ★ L6 修正（REVIEW-report L6）：旧代码不检查返回值。
             * cm_buf_put 失败（内存不足 / reserve 扩容失败）时 au_sorted 里
             * 只会留下**前面几段**，而旧代码的 `if (au_sorted.len > 0)`
             * 照样把它当成完整 AU 推给解码器 —— 推半截 AU 比整帧丢弃更糟：
             * 解码器会用这个截断帧污染参考帧，正是 bug3「马赛克残留」的
             * 成因形态。现在任一 put 失败就整帧走丢弃路径。 */
            if (cm_buf_put(&g_rtp.au_sorted, g_rtp.frame.p + g_rtp.segs[t].off,
                           (size_t)g_rtp.segs[t].len) != 0) {
                break;  /* k < seg_n 即为失败标志（正常结束是 k == seg_n） */
            }
        }
        if (k < g_rtp.seg_n) {
            /* 丢弃路径：不推、不计 frames，只记 au_drop 并复位 AU 状态。
             * 置 au_broken 只为让诊断看得出这帧是坏的（马上就 reset 了）。 */
            g_rtp.au_broken = 1;
            s->stats.vid_au_drop++;
            CM_LOGW("#%d AU resequence failed (buffer alloc), drop AU "
                    "(%llu bytes, segs=%d)", s->id,
                    (unsigned long long)g_rtp.frame.len, g_rtp.seg_n);
            rtp_reset_au();
            return;
        }
        if (g_rtp.au_sorted.len > 0) {
            base_p = g_rtp.au_sorted.p;
            base_n = (int)g_rtp.au_sorted.len;
            /* L1：升到 LOGI —— 说明 NACK 真的把乱序分片补回来了（重排生效）。 */
            CM_LOGI("#%d AU resequenced (%d segs, retransmits filled holes)",
                    s->id, g_rtp.seg_n);
        }
    }

    /* --- 3) IDR 缺参数集时补插（在重排之后做，否则会被重排覆盖）--- */
    out_p = base_p;
    out_n = base_n;
    if (key) {
        need_sps = !g_rtp.au_has_sps && g_rtp.sps_ok;
        need_pps = !g_rtp.au_has_pps && g_rtp.pps_ok;
        if (need_sps || need_pps) {
            cm_buf_reset(&g_rtp.au_out);
            if (need_sps) cm_buf_put(&g_rtp.au_out, g_rtp.sps.p, g_rtp.sps.len);
            if (need_pps) cm_buf_put(&g_rtp.au_out, g_rtp.pps.p, g_rtp.pps.len);
            if (cm_buf_put(&g_rtp.au_out, base_p, (size_t)base_n) < 0) {
                CM_LOGW("#%d failed to inject SPS/PPS, pushing raw AU", s->id);
                cm_buf_reset(&g_rtp.au_out);
            } else {
                out_p = g_rtp.au_out.p;
                out_n = (int)g_rtp.au_out.len;
                s->stats.vid_sps_inject++;
                CM_LOGI("#%d IDR without %s%s, injected cached parameter sets "
                        "(sps=%dB pps=%dB au=%dB)",
                        s->id, need_sps ? "SPS" : "", need_pps ? "PPS" : "",
                        (int)g_rtp.sps.len, (int)g_rtp.pps.len, base_n);
            }
        }
    }

    if (key) mark_keyframe(s);

    /* gst_mu 保护：JS 线程的 setVideoVisible(0/1) 会 set_rect 甚至销毁重建 vbox，
     * 收包线程这时正好在 push —— 不加锁就是 use-after-free。 */
    pthread_mutex_lock(&s->gst_mu);
    if (s->vbox) {
        if (cm_box_push(s->vbox, out_p, out_n,
                        (uint64_t)g_rtp.last_ts * 1000000ULL / 90, key) == 0) {
            s->stats.frames++;
            g_rtp.drop_streak = 0;  /* 成功推了一帧：连续丢计数归零 */
            s->kf_starve_ms = 0;
            if (!s->video_ok) {
                s->video_ok = 1;
                CM_LOGI("#%d first video frame pushed (%d bytes, %dx%d, key=%d)",
                        s->id, out_n, s->video_w, s->video_h, key);
            }
            /* ★ 诊断（1 秒限速）：把「实际推给 appsrc 的东西」记下来。
             * 终末地全黑时要分清「码流坏」还是「mppvideodec 坏」，靠的就是这行
             * + au.h264 dump。参数集 hex 只打前 8 字节，足够人工比对是不是
             * 67/68 开头的合法 SPS/PPS（67=nal 7+SPS，68=nal 8+PPS）。 */
            if (g_diag_ms == 0 || now > g_diag_ms) {
                g_diag_ms = now + 1000;
                CM_LOGI("#%d AU push key=%d bytes=%d sps=%d pps=%d inject=%llu "
                        "reseq=%d segs=%d broken=%d missing=%d "
                        "sps.len=%d sps[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x "
                        "pps.len=%d pps[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x",
                        s->id, key, out_n,
                        g_rtp.au_has_sps, g_rtp.au_has_pps,
                        (unsigned long long)s->stats.vid_sps_inject,
                        g_rtp.seg_ordered ? 0 : 1, g_rtp.seg_n,
                        g_rtp.au_broken, g_rtp.au_missing,
                        (int)g_rtp.sps.len,
                        g_rtp.sps.len > 0 ? g_rtp.sps.p[0] : 0,
                        g_rtp.sps.len > 1 ? g_rtp.sps.p[1] : 0,
                        g_rtp.sps.len > 2 ? g_rtp.sps.p[2] : 0,
                        g_rtp.sps.len > 3 ? g_rtp.sps.p[3] : 0,
                        g_rtp.sps.len > 4 ? g_rtp.sps.p[4] : 0,
                        g_rtp.sps.len > 5 ? g_rtp.sps.p[5] : 0,
                        g_rtp.sps.len > 6 ? g_rtp.sps.p[6] : 0,
                        g_rtp.sps.len > 7 ? g_rtp.sps.p[7] : 0,
                        (int)g_rtp.pps.len,
                        g_rtp.pps.len > 0 ? g_rtp.pps.p[0] : 0,
                        g_rtp.pps.len > 1 ? g_rtp.pps.p[1] : 0,
                        g_rtp.pps.len > 2 ? g_rtp.pps.p[2] : 0,
                        g_rtp.pps.len > 3 ? g_rtp.pps.p[3] : 0,
                        g_rtp.pps.len > 4 ? g_rtp.pps.p[4] : 0,
                        g_rtp.pps.len > 5 ? g_rtp.pps.p[5] : 0,
                        g_rtp.pps.len > 6 ? g_rtp.pps.p[6] : 0,
                        g_rtp.pps.len > 7 ? g_rtp.pps.p[7] : 0);
            }
            /* ★ 每帧 AU 指纹（开关 /userdisk/miniapp/data/auld.txt）。
             * 目的：区分两种"平面纹丝不动"——
             *   a) 码流每帧都不同，但视频平面 md5 一直不变 → 残差没被应用，
             *      问题在 mppvideodec（参考链坏了、错误掩盖吐同一张图）；
             *   b) 码流本身也每帧相同 → 源侧就是静止的（游戏卡在加载页）。
             * 用 FNV-1a 64 位当指纹：零依赖、够短、整数运算，3000 行也不拖速度。 */
            if (g_auld == 0) {
                FILE *sw = fopen("/userdisk/miniapp/data/auld.txt", "r");
                if (sw) { fclose(sw); g_auld = 2; }
                else g_auld = 1;
            }
            if (g_auld == 2) {
                uint64_t h = 1469598103934665603ULL;
                int i;
                for (i = 0; i < out_n; i++) { h ^= out_p[i]; h *= 1099511628211ULL; }
                CM_LOGI("#%d AULD t=%llu n=%d key=%d h=%016llx",
                        s->id, (unsigned long long)now, out_n, key,
                        (unsigned long long)h);
            }
            /* ★ AU dump：传的是刚推给 appsrc 的同一份字节（含注入的 SPS/PPS） */
            au_dump_hook(out_p, out_n, key);
        } else {
            CM_LOGW("#%d appsrc push failed", s->id);
        }
    }
    pthread_mutex_unlock(&s->gst_mu);
    s->stats.nal_out++;
    rtp_reset_au();
    emit_media_if_needed(s);
}

/* hold 窗口到期：把挂起的 IDR AU 放行（该坏就坏，但画面不能停在这儿）。
 * 由 cm_media.c 的主循环每轮调一次（20ms 粒度足够）。 */
void cm_rtp_flush_held(cm_sess *s) {
    uint64_t now;
    if (!s) return;
    now = cm_now_ms();
    pthread_mutex_lock(&g_rtp_mu);
    if (g_rtp.au_hold_state == 1 && now > g_rtp.au_hold_until_ms &&
        (now - g_rtp.au_hold_until_ms) >= 0) {
        g_rtp.au_hold_state = 2;  /* 已到期：flush_frame 不再重复挂起 */
        g_rtp.au_hold_until_ms = 0;
        flush_frame(s);
    }
    pthread_mutex_unlock(&g_rtp_mu);
}

/* 把一个分片登记进当前 AU 的分片表（NACK 补洞后按 seq 重排要用）。
 * off 是该分片在 frame 缓冲里的起始偏移，len 是它的字节数。 */
static void au_seg_add(uint16_t seq, uint32_t off, uint32_t len) {
    int n = g_rtp.seg_n;
    if (n >= AU_SEG_MAX) {
        /* 表满：这个 AU 的分片数超了 AU_SEG_MAX（512）。
         *
         * ★ 这里绝不能只置 seg_ordered=0 —— 那样 flush 会走重排路径，
         * 而重排只输出 segs[] 里登记过的段，**后面这几十个分片会被静默
         * 截断**，等于把一个缺小半个画面的 IDR 送进解码器（比整帧丢更糟：
         * 解码器会用坏帧污染参考帧，正是 bug3/bug4 的症状）。
         *
         * 正确做法：单独记 seg_overflow，flush 时**放弃重排**、直接用 frame
         * 原始内容。frame 里仍然包含全部分片（annexb_put 是独立于本表的），
         * 只是补洞包的插入顺序无法修正 —— 最坏是这一帧花一下，不会截断。
         * 未补洞时 frame 本来就是 seq 顺序，所以绝大多数情况下完全无损。 */
        g_rtp.seg_overflow = 1;
        return;
    }
    /* 正常前进时 seq 递增，seg_ordered 保持 1；一旦发现倒退就置 0。 */
    if (n > 0) {
        int16_t d = (int16_t)(seq - g_rtp.segs[n - 1].seq);
        if (d <= 0) g_rtp.seg_ordered = 0;
    }
    g_rtp.segs[n].seq = seq;
    g_rtp.segs[n].off = off;
    g_rtp.segs[n].len = len;
    g_rtp.seg_n = n + 1;
    if (!g_rtp.au_have_range) {
        g_rtp.au_have_range = 1;
        g_rtp.au_first_seq = seq;
        g_rtp.au_last_seq = seq;
    } else {
        if ((int16_t)(seq - g_rtp.au_first_seq) < 0) g_rtp.au_first_seq = seq;
        if ((int16_t)(seq - g_rtp.au_last_seq) > 0) g_rtp.au_last_seq = seq;
    }
}

/* 把「自 au_start_ms 起登记的缺口」升级成 IDR 优先级。
 *
 * 为什么需要这一步：缺口是在丢包那一刻登记的，那时我们往往还没看到
 * 这个 AU 的 IDR 标识（缺口可能就是 AU 的头几个包）。等 FU-A start 到了、
 * 发现 typ==5，才知道「刚才丢的是关键帧的分片」—— 这时必须把优先级提上来，
 * 否则重传预算会被一堆无关紧要的 P 帧缺口抢走。 */
static void gap_upgrade_since(uint64_t since_ms) {
    int i;
    for (i = 0; i < g_rtp.gap_n; i++) {
        if (g_rtp.gaps[i].t_ms >= since_ms) g_rtp.gaps[i].prio = NACK_PRIO_IDR;
    }
}

/* 查这个 seq 是不是我们在等重传的包；是就摘掉（缺口表 + AU 缺失表都摘）
 * 并返回 1。用它区分「重传包」和「真乱序」—— 前者要收下，后者必须丢。 */
static int gap_take(uint16_t seq) {
    int i, found = 0;
    for (i = 0; i < g_rtp.gap_n; i++) {
        if (g_rtp.gaps[i].seq == seq) { found = 1; break; }
    }
    if (!found) return 0;
    /* ★ L2 修正（REVIEW-report L2）：摘缺口 + 同步摘 au_miss 由 gap_drop() 统一负责。
     * 归零时 flush_frame 会把 au_broken 清掉。 */
    gap_drop(seq);
    return 1;
}

static void handle_h264(cm_sess *s, const unsigned char *p, int n, int marker, uint16_t seq) {
    int nal_type;
    if (n < 1) return;
    nal_type = p[0] & 0x1F;

    if (nal_type == 24) { /* STAP-A */
        int o = 1;
        /* ★ F1 附带修正：STAP-A 分支以前从不设置 au_start_ms，于是
         * gap_upgrade_since(au_start_ms) 用的是**别的 AU 残留的旧时刻**
         * （或者 0，导致 sha 全部通过、把无关缺口升成 IDR 优先级）。
         * AU 起点应该在第一个见到参数集/切片的包上就打戳。 */
        if (!g_rtp.au_start_ms) g_rtp.au_start_ms = cm_now_ms();
        while (o + 2 <= n) {
            int sz = (p[o] << 8) | p[o + 1];
            int t;
            o += 2;
            if (sz <= 0 || o + sz > n) {
                /* STAP-A 内部长度字段越界 = 这个聚合包本身坏了。
                 * ★ 这是**结构性损坏**，重传也补不回来：必须显式把
                 * au_broken_fixable 清 0，否则会继承上一帧丢包留下的 1，
                 * 让 flush_frame 的「补洞成功」判据（broken && fixable &&
                 * missing==0）误判成已修复，把截断的 AU 推给解码器。 */
                g_rtp.au_broken = 1;
                g_rtp.au_broken_fixable = 0;
                break;
            }
            t = p[o] & 0x1F;
            if (t == 7) {
                int w = 0, h = 0, prof = 0, lev = 0;
                if (sps_parse_size(p + o, sz, &w, &h, &prof, &lev) == 0) {
                    note_sps(s, w, h, prof, lev);
                }
            }
            if (t == 7 || t == 8) cache_param_set(p + o, sz);
            if (t == 1 || t == 5) g_rtp.au_has_slice = 1;
            if (t == 5) g_rtp.au_has_idr = 1;
            {
                uint32_t off = (uint32_t)g_rtp.frame.len;
                if (annexb_put(&g_rtp.frame, p + o, sz) == 0)
                    au_seg_add(seq, off, 4u + (uint32_t)sz);
            }
            g_rtp.have_data = 1;
            o += sz;
        }
        /* STAP-A 自带 SPS/PPS 时说明它是关键帧的开头，给它对应的缺口升级优先级 */
        if (g_rtp.au_has_idr || g_rtp.au_has_sps || g_rtp.au_has_pps) {
            gap_upgrade_since(g_rtp.au_start_ms);
        }
        /* ★ 这里原来是 `if (marker) flush_frame(s, 1);` —— 把**任何** STAP-A
         * 都当关键帧。于是只含 SPS/PPS/SEI 的 STAP-A 也会 mark_keyframe()
         * 刷新 last_kf_ms，cm_kf_tick 就以为「关键帧还新鲜」而不补 PLI，
         * 丢掉的 IDR 永远等不回来 —— 这正是「马赛克残留会一直持续」而不是
         * 「闪一帧就过」的原因。现在关键帧只由 au_has_idr（NAL type 5）决定。 */
        if (marker) flush_frame(s);
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
            /* ★ R4 修正（REVIEW-report R4）：hold 窗口里来了**下一个 AU** 的
             * start 时，必须先把挂起的 IDR 放行，再让新 AU 从干净的 start 起。
             *
             * 旧行为：hold 只把 au_hold_state 置 1 就 return，而此时的
             * in_fu 已经在 end 分支被清成 0（`if (end) { in_fu = 0; flush_frame(); }`），
             * 所以下面第一个分支（in_fu）永远拦不住它，必然落到
             * `else if (au_broken)` → rtp_reset_au() → au_hold_state=0，
             * **挂起的 IDR 连同它的 frame 缓冲一起被丢掉**，NACK 重传回来的
             * 包也无处可放。60fps 时间隔 16.7ms < IDR_HOLD_MS(30ms)，
             * hold 必然失效 —— 机制等于从未生效，还会误导后续调参。
             *
             * 现在：先置 state=2（flush_frame 不会重复挂起）并把 deadline 清零，
             * 然后直接 flush_frame —— 洞补上了就推、没补上就走「坏 IDR」丢弃
             * 路径并补 PLI，两种情况都比「默默消失」正确。 */
            if (g_rtp.au_hold_state == 1) {
                g_rtp.au_hold_state = 2;
                g_rtp.au_hold_until_ms = 0;
                CM_LOGD("#%d next AU start arrived during IDR hold, releasing held AU",
                        s->id);
                flush_frame(s);
            }
            if (g_rtp.in_fu) {
                /* 上一 AU 没等到 end 就来了新 start：整帧作废。
                 * 旧代码只是 cm_buf_reset 了事，既不计账也不补 PLI，
                 * 于是丢帧完全隐形。 */
                s->stats.vid_au_drop++;
                g_rtp.drop_streak++;
                kf_request(s, "truncated AU");
                rtp_reset_au();
            } else if (g_rtp.au_broken) {
                /* 之前的孤儿分片留下的破坏标记；新 AU 从干净的 start 起 */
                rtp_reset_au();
            }
            g_rtp.au_start_ms = cm_now_ms();
            g_rtp.seg_ordered = 1;
            {
                uint32_t off = (uint32_t)g_rtp.frame.len;
                cm_buf_put(&g_rtp.frame, sc, 4);
                cm_buf_put(&g_rtp.frame, &nh, 1);
                g_rtp.in_fu = 1;
                g_rtp.have_data = 1;
                g_rtp.au_has_slice = 1;   /* FU-A 承载的一定是切片 */
                if (typ == 5) {
                    g_rtp.au_has_idr = 1;
                    /* ★ 关键一步：IDR 的 start 到了，说明「刚才登记的缺口」
                     * 很可能就是这个 IDR 的头几个分片。把它们升到最高优先级，
                     * 并放宽重发轮数 —— 一个 IDR 丢一片要赔 39 个 P 帧。 */
                    gap_upgrade_since(g_rtp.au_start_ms);
                }
                au_seg_add(seq, off, 0); /* len 稍后补：本包正文还没 append */
            }
            /* 关键帧计数只在 flush_frame 里做，避免 start + flush 双计 */
        }
        if (!g_rtp.in_fu) {
            /* 丢了 start：后续分片无法归属，全部丢弃。整条 AU 作废，
             * 必须补 PLI —— 否则这一段码流永远缺一帧且无人知晓。
             *
             * 注意这里**还要登记缺口**：丢的 start 那一片是可以点名要回来的，
             * 如果网关重传了它，这个 AU 就能救回来（而不是整帧报废）。 */
            if (!g_rtp.au_broken) {
                g_rtp.au_broken = 1;
                g_rtp.au_broken_fixable = 1;
                s->stats.vid_au_drop++;
                gap_add(seq, NACK_PRIO_P);
                au_mark_missing(seq);
                kf_request(s, "FU-A start lost");
            }
            return;
        }
        if (n > 2) {
            uint32_t off = (uint32_t)g_rtp.frame.len;
            if (cm_buf_put(&g_rtp.frame, p + 2, (size_t)(n - 2)) == 0) {
                int last = g_rtp.seg_n - 1;
                /* start 那个槽位只是占位（len=0），这里把真正的长度补上；
                 * 后续分片各占一个槽位，并记下自己在 frame 里的偏移。 */
                if (last >= 0 && g_rtp.segs[last].seq == seq && g_rtp.segs[last].len == 0) {
                    g_rtp.segs[last].len = 5u + (uint32_t)(n - 2);
                } else {
                    au_seg_add(seq, off, (uint32_t)(n - 2));
                }
            }
        }
        if (end) {
            g_rtp.in_fu = 0;
            flush_frame(s);
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
    if (nal_type == 7 || nal_type == 8) cache_param_set(p, n);
    if (nal_type == 1 || nal_type == 5) g_rtp.au_has_slice = 1;
    if (nal_type == 5) g_rtp.au_has_idr = 1;

    /* 单 NAL（含 SPS=7 / PPS=8 / 普通切片 1、5） */
    {
        uint32_t off = (uint32_t)g_rtp.frame.len;
        if (annexb_put(&g_rtp.frame, p, n) == 0)
            au_seg_add(seq, off, 4u + (uint32_t)n);
    }
    if (nal_type == 5 || nal_type == 7 || nal_type == 8) {
        if (!g_rtp.au_start_ms) g_rtp.au_start_ms = cm_now_ms();
        gap_upgrade_since(g_rtp.au_start_ms);
    }
    g_rtp.have_data = 1;
    if (marker) flush_frame(s); /* flush_frame 自己按 au_has_idr 判关键帧 */
}

/* ---------------- 对外入口 ---------------- */

/* SRTP 解密失败（认证失败/重放）时由 cm_media.c 调用。
 *
 * ★ 这是 bug 3 的第二条上游通路，旧代码完全没有覆盖：
 *   cm_media.c 对 plen==-7 只打了 CM_LOGD("#%d srtp auth fail") 就 over
 *   （cm_media.c:142-143）——丢一个包就完事。若丢的是 FU-A 的中间片，
 *   剩下的片照样拼进同一个 AU 并被推给解码器（正是「上一帧马赛克残留」）。
 *   而且 SRTP 层看不出这是视频包，只有 RTP 层知道「当前 AU 已经不完整」。
 *
 * 所以这里做两件事：把当前 AU 标脏（flush 时整帧丢弃）+ 补一次 PLI。
 * 注意 pkt 的 RTP 头是明文（SRTP 只加密 payload），调用方可以据此判断 PT。 */
void cm_rtp_note_stale(cm_sess *s, const char *why) {
    if (!s) return;
    pthread_mutex_lock(&g_rtp_mu);
    if (g_rtp.have_data || g_rtp.in_fu) {
        if (!g_rtp.au_broken) {
            g_rtp.au_broken = 1;
            s->stats.vid_au_drop++;
        }
    }
    pthread_mutex_unlock(&g_rtp_mu);
    /* 关键帧请求放在锁外：cm_send_pli 会做 UDP send，不该持锁 */
    kf_request(s, why ? why : "srtp auth fail");
}

/* 视频管线被重建后调用（见 cm_media.c 的上屏看门狗）。
 *
 * 新解码器没有任何参考帧，所以必须：
 *   1. 丢掉正在拼的半截 AU（它属于旧解码器的时间线，拼完推过去只会花屏）；
 *   2. 清缺口表 —— 那些序号对应的是旧管线的数据，补回来也没人用；
 *   3. 关掉关键帧闸门（have_kf=0）并清掉饥饿/放行计时，让后续 P 帧被挡在
 *      外面，直到真的收到一个新 IDR；
 *   4. 立刻补一次 PLI，别等 cm_kf_tick 的 2 秒节拍。
 * **不清 SPS/PPS 缓存**：终末地的 IDR 不带参数集（每局 3~4 次
 * `IDR without SPSPPS, injected cached parameter sets`），新解码器要靠
 * 缓存里的 SPS/PPS 才能初始化，清了反而开不出画面。 */
void cm_rtp_after_rebuild(cm_sess *s) {
    if (!s) return;
    pthread_mutex_lock(&g_rtp_mu);
    if (!g_rtp_init) {
        cm_buf_init(&g_rtp.frame);
        cm_buf_init(&g_rtp.sps);
        cm_buf_init(&g_rtp.pps);
        cm_buf_init(&g_rtp.au_out);
        g_rtp_init = 1;
    }
    g_rtp.sess_id = s->id;
    rtp_reset_au();
    g_rtp.gap_n = 0;
    g_rtp.seg_n = 0;
    g_rtp.drop_streak = 0;
    g_rtp.have_seq = 0;
    g_rtp.last_seq = 0;
    s->have_kf = 0;
    s->kf_starve_ms = 0;
    s->relax_until_ms = 0;
    pthread_mutex_unlock(&g_rtp_mu);
    kf_request(s, "pipeline rebuild");
}

void cm_rtp_reset(cm_sess *s) {
    if (!s) return;
    pthread_mutex_lock(&g_rtp_mu);
    if (!g_rtp_init) {
        cm_buf_init(&g_rtp.frame);
        cm_buf_init(&g_rtp.sps);
        cm_buf_init(&g_rtp.pps);
        cm_buf_init(&g_rtp.au_out);
        g_rtp_init = 1;
    }
    rtp_reset_all();
    g_rtp.sess_id = s->id;
    /* P1-7：换局时饥饿/放行计时必须归零。否则上一局留下的
     * relax_until_ms 会在新局开头就把闸门当成「已放行」状态处理。 */
    s->kf_starve_ms = 0;
    s->relax_until_ms = 0;
    pthread_mutex_unlock(&g_rtp_mu);
}

/* ---- RTCP RR 的统计埋点（bug1）----
 *
 * 只在"确认是本路视频"之后累加三个量：received、扩展最高序号、抖动。
 * 刻意**不碰**任何判定逻辑：调用点之后的 vid_dup / vid_lost / vid_reorder
 * 与关键帧闸门一行都没改，本函数只是旁路观测。
 *
 * 为什么 received 含重复包/乱序包：RFC 3550 A.3 的口径是"网关发出来的包
 * 我们收到几个"，expected - received 才是网络丢包。若只数"干净前进的包"，
 * 重复包会被算成丢包，网关就会看到一个虚高的丢包率。
 */
/* 音视频两路流共用的统计内核：扩展最高序号 + 到达间隔抖动。
 *
 * 为什么做成"传一堆指针"的形态：视频和音频的状态分别放在 cm_stats 的两个
 * 字段组里（vid_* / aud_*），而算法一模一样，只有 RTP 时钟不同（视频 90000，
 * Opus 48000）。用指针传状态，比复制两份 20 行逻辑更不容易写歪一边。
 * 它只写统计字段，不读也不改任何判定状态（vid_dup / vid_lost / 闸门）。 */
static void rr_note_seq_jitter(uint16_t seq, uint32_t ts, uint32_t clock_hz,
                               uint32_t *ext_max, uint32_t *ext_base, int *have_ext,
                               uint64_t *jitter_q4, int32_t *last_transit, int *have_jit,
                               uint32_t *prior_exp, uint64_t *prior_rcv) {
    uint64_t now = cm_now_ms();
    /* 到达时刻换算到该路的 RTP 时钟（1ms = clock_hz/1000 单位）。乘完之后取
     * 32 位截断无所谓：后续全是模 2^32 的差值运算，和 RTP 时间戳本身一样回绕。 */
    uint32_t arrive = (uint32_t)(now * (uint64_t)clock_hz / 1000ULL);

    /* ---- 扩展最高序号（RFC 3550 A.1 的简化版）----
     * 用 int16 的符号差判前进/迟到：正数=前进（含跨 65536 回绕），
     * 非正=迟到或乱序，不推进最高序号。极大正向跳跃（> MAX_DROPOUT）
     * 按 RFC 也视为乱序而不是前进。 */
    if (!*have_ext) {
        *have_ext = 1;
        *ext_base = seq;
        *ext_max = seq;
        /* RR 的 fraction lost 需要和 ext_max 同基准的"上一次快照"。
         * 这里把基准放在包流起点前一个序号，语义上等价于"从第一个包开始
         * 统计"，否则第一次 RR 会拿 ext_max 去减 0，得到一个
         * 「丢了 99%」的假值（初始 seq 本身就有几十万）。 */
        *prior_exp = seq - 1u;
        *prior_rcv = 0;
    } else {
        int16_t d = (int16_t)(seq - (uint16_t)(*ext_max & 0xFFFFu));
        if (d > 0 && (uint32_t)d <= RR_MAX_DROPOUT) {
            *ext_max += (uint32_t)d;
        }
    }

    /* ---- 到达间隔抖动（RFC 3550 A.8）----
     * D = (到达时刻 - 时间戳) 与上一包的差；J += (|D| - J) / 16。
     * 内部按 Q4 定点保存，避免整数除法把很小的抖动直接抹成 0。 */
    if (*have_jit) {
        int32_t transit = (int32_t)(arrive - ts);
        int32_t d = transit - *last_transit;
        uint32_t ad = (d < 0) ? (uint32_t)(-d) : (uint32_t)d;
        int64_t q = (int64_t)*jitter_q4;
        q += (((int64_t)ad << 4) - q) / 16;
        if (q < 0) q = 0;
        *jitter_q4 = (uint64_t)q;
        *last_transit = transit;
    } else {
        *have_jit = 1;
        *last_transit = (int32_t)(arrive - ts);
        *jitter_q4 = 0;
    }
}

/* 视频侧入口：received 计数 + 上面那个内核。 */
static void rr_note_video(cm_sess *s, uint16_t seq, uint32_t ts) {
    s->stats.vid_rx++;
    rr_note_seq_jitter(seq, ts, VIDEO_CLOCK_HZ,
                       &s->stats.ext_seq_max, &s->stats.ext_seq_base,
                       &s->stats.have_ext_seq, &s->stats.jitter_q4,
                       &s->stats.last_transit, &s->stats.have_jitter,
                       &s->stats.rr_expected_prior, &s->stats.rr_received_prior);
}

/* 音频侧入口（Opus 走 48000Hz 时钟，不能套用视频的 90000）。
 *
 * 调用点在 SSRC 校验之后、推送 appsrc 之前：口径是「网关发过来的、属于这路
 * 音频的包收到了几个」，与"成功推进 GStreamer"无关 —— 和视频侧 received 含
 * 重复/乱序包是同一个道理（RFC 3550 A.3）。 */
static void rr_note_audio(cm_sess *s, uint16_t seq, uint32_t ts) {
    s->stats.aud_rx++;
    rr_note_seq_jitter(seq, ts, (uint32_t)AUDIO_CLOCK_HZ,
                       &s->stats.aud_ext_seq_max, &s->stats.aud_ext_seq_base,
                       &s->stats.have_aud_ext_seq, &s->stats.aud_jitter_q4,
                       &s->stats.aud_last_transit, &s->stats.have_aud_jitter,
                       &s->stats.aud_rr_expected_prior, &s->stats.aud_rr_received_prior);
}

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
        uint16_t seq;
        if (s->cfg.video_ssrc && ssrc != s->cfg.video_ssrc) return;
        /* 记下实际在收的视频 SSRC：SDP 没协商出 video_ssrc 时，RR 的
         * report block 要拿它填 SSRC_n（填 0 会被网关丢弃整条 RR）。 */
        s->stats.vid_ssrc_seen = ssrc;
        pthread_mutex_lock(&g_rtp_mu);
        if (!g_rtp_init) {
            cm_buf_init(&g_rtp.frame);
            cm_buf_init(&g_rtp.sps);
            cm_buf_init(&g_rtp.pps);
            cm_buf_init(&g_rtp.au_out);
            g_rtp_init = 1;
        }
        /* 换会话（换局/重连）：g_rtp 是进程级单例，上一局的 seq 基准、残缺 AU、
         * 旧 SPS/PPS 必须清掉，否则新一局第一帧会被误判成「乱序」而整帧丢弃，
         * 或者被旧 SPS 带偏 —— 这是「新开一局一上来就绿屏/花屏」的可疑成因。 */
        if (g_rtp.sess_id != s->id) {
            g_rtp.sess_id = s->id;
            rtp_reset_all();
            CM_LOGI("#%d video rtp ctx reset for session", s->id);
        }

        seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
        /* RR 统计埋点：放在 seq 判定**之前**，因为 received 要含重复包与
         * 乱序包（见 rr_note_video 的注释）。它不改变任何后续判定。 */
        rr_note_video(s, seq, cm_rd32(pkt + 4));
        /* ---- RTP 序号连续性校验（bug 3 的根治点）----
         *
         * 旧代码完全没读 seq。丢一个 FU-A 中间片，后面的片照样 append 进同一个
         * AU，end 时把**缺片的访问单元**推给解码器；mppvideodec 的 ignore-error
         * 出厂是 true，解坏了不报错、拿旧参考帧补齐错误宏块直接上屏，而且污染
         * 参考帧 —— 用户看到的就是持续存在的「上一帧马赛克残留」。
         *
         * 现在：任何缺口/乱序/重复都把当前 AU 标记损坏，flush 时整帧丢弃，
         * 并在缺口出现的那一刻补一发 PLI。残缺 AU 永不进解码器。 */
        if (!g_rtp.have_seq) {
            g_rtp.have_seq = 1;
            g_rtp.last_seq = seq;
        } else {
            uint16_t diff = (uint16_t)(seq - g_rtp.last_seq);
            if (diff == 0) {
                /* 重复包：直接丢，绝不能 append（会把分片内容拼两遍） */
                s->stats.vid_dup++;
                pthread_mutex_unlock(&g_rtp_mu);
                return;
            }
            if (diff == 1) {
                g_rtp.last_seq = seq;
            } else if (diff < 0x8000u) {
                /* 正向缺口 diff-1 个包丢了。注意 seq 是 16 位，diff 已经
                 * 处理了回绕，所以这里不能用 seq > last_seq 比较。 */
                uint16_t prev = g_rtp.last_seq;
                s->stats.vid_lost += (uint64_t)(diff - 1);
                g_rtp.last_seq = seq;
                /* ★ 登记缺口：这些 seq 就是我们希望网关重传的包。
                 * 真机 2026-11-13：401 个残缺 AU 赔掉 15511 个正常 P 帧，
                 * 原因就是「缺的那几个包永远回不来」。有了这张表，
                 * cm_nack_tick 才能点名要。逐个登记（缺口可能连着几十个）。 */
                {
                    int k;
                    unsigned char prio = (g_rtp.au_has_idr ? NACK_PRIO_IDR : NACK_PRIO_P);
                    for (k = 1; k < (int)diff && k <= 64; k++) {
                        uint16_t miss = (uint16_t)(prev + k);
                        gap_add(miss, prio);
                        au_mark_missing(miss);
                    }
                    if ((int)diff > 64) {
                        /* 超过 64 个连续丢包：这不是「零星丢包」，是整段断了
                         * （网关停推/换 SSRC/网络黑洞）。逐个登记没意义，
                         * 直接当结构性损坏，等 IDR。 */
                        g_rtp.au_broken = 1;
                        g_rtp.au_broken_fixable = 0;
                    }
                }
                if (!g_rtp.au_broken) {
                    g_rtp.au_broken = 1;
                    g_rtp.au_broken_fixable = 1;
                    if (s->stats.vid_lost <= LOSS_LOG_MAX) {
                        CM_LOGW("#%d rtp gap: lost %d pkt(s) (seq %u->%u), NACK queued",
                                s->id, (int)(diff - 1), (unsigned)prev, (unsigned)seq);
                    } else if (s->stats.vid_lost % 100 == 0) {
                        CM_LOGW("#%d rtp gap total lost=%llu au_drop=%llu nack=%llu",
                                s->id, (unsigned long long)s->stats.vid_lost,
                                (unsigned long long)s->stats.vid_au_drop,
                                (unsigned long long)s->stats.vid_nack_req);
                    }
                    kf_request(s, "rtp gap");
                }
            } else {
                /* 落后超过半个序号空间 = 旧包。两种情况要分开：
                 *
                 *   (a) 它在我们等重传的缺口表里 → 是 NACK 换回来的重传包，
                 *       必须收下（否则重传白做）。按 seq 定位放进去，
                 *       但不推进 last_seq（它本来就比 last_seq 旧）。
                 *   (b) 不在表里 → 真乱序（网络乱序，或网关重发旧包），
                 *       按到达顺序 append 会把分片插到错误位置，整帧必须丢。
                 *       也不要因此把 last_seq 往回拨 —— 那会让后面的正常包
                 *       全被当成「正向缺口」，形成雪崩。 */
                if (gap_take(seq)) {
                    /* ★ R2 修正（REVIEW-report R2）：重传包必须判归属。
                     * 缺口表按 TTL 存活，而 AU 边界会 reset —— 上一 AU 的
                     * 重传包回来时，当前正在拼的可能是**另一个** AU，直接
                     * append 会把别人的分片拼进来，污染整帧且不被发现
                     * （成功分支原本不置 au_broken）。
                     * 用 au_first_seq/au_last_seq 做范围校验：不属于当前 AU
                     * 的重传包丢掉（缺口已在 gap_take 里摘除，不会再要第二遍）。 */
                    if (g_rtp.au_have_range) {
                        int16_t d0 = (int16_t)(seq - g_rtp.au_first_seq);
                        int16_t d1 = (int16_t)(seq - g_rtp.au_last_seq);
                        if (d0 < 0 || d1 > 0) {
                            s->stats.vid_stale_rtx++;
                            if (s->stats.vid_stale_rtx <= LOSS_LOG_MAX) {
                                CM_LOGD("#%d stale retransmit seq=%u outside AU [%u,%u], dropped",
                                        s->id, (unsigned)seq,
                                        (unsigned)g_rtp.au_first_seq,
                                        (unsigned)g_rtp.au_last_seq);
                            }
                            pthread_mutex_unlock(&g_rtp_mu);
                            return;
                        }
                    }
                    s->stats.vid_nack_recovered++;
                    /* L1：升到 LOGI 并限速。这是「NACK 到底有没有用」的直接
                     * 证据（recovered/nack_req 的分子）。弱网下一旦生效会持续
                     * 触发，所以只详细打前 LOSS_LOG_MAX 条，之后每 100 条汇总。 */
                    if (s->stats.vid_nack_recovered <= LOSS_LOG_MAX ||
                        (s->stats.vid_nack_recovered % 100u) == 0) {
                        CM_LOGI("#%d retransmit recovered seq=%u (%llu/%llu)",
                                s->id, (unsigned)seq,
                                (unsigned long long)s->stats.vid_nack_recovered,
                                (unsigned long long)s->stats.vid_nack_req);
                    }
                    /* 落到下面正常处理：append 并按 seq 记录分片偏移。
                     * au_broken 保持原样，等 au_missing 归零时由 flush 清掉。 */
                } else {
                    s->stats.vid_reorder++;
                    g_rtp.au_broken = 1;
                    if (s->stats.vid_reorder <= LOSS_LOG_MAX) {
                        CM_LOGW("#%d rtp reorder: seq %u behind last %u, drop AU",
                                s->id, (unsigned)seq, (unsigned)g_rtp.last_seq);
                    }
                    pthread_mutex_unlock(&g_rtp_mu);
                    return;
                }
            }
        }

        g_rtp.last_ts = cm_rd32(pkt + 4);
        if (g_rtp.frame.len > FRAME_CAP) {
            /* 超长：整帧作废。旧代码只 reset 不记账，于是 SPS/PPS 丢在
             * 这个 AU 里也无人知道，后续 IDR 缺参数集直接出绿块。 */
            s->stats.vid_au_drop++;
            g_rtp.drop_streak++;
            if (!g_rtp.au_broken) {
                g_rtp.au_broken = 1;
                kf_request(s, "AU over frame cap");
            }
            rtp_reset_au();
        }
        handle_h264(s, payload, plen, marker, seq);
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
        /* RR 统计埋点（音频）：放在这之后 = 只统计"确实是这路音频的包"。
         * 与视频侧同理，它只写统计字段，音频推送与任何判定都不受影响。 */
        s->stats.aud_ssrc_seen = ssrc;
        rr_note_audio(s, (uint16_t)((pkt[2] << 8) | pkt[3]), cm_rd32(pkt + 4));
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
