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
 * 它刷新 last_poll_ms；心跳停了超过阈值就认定小程序已经不在前台，
 * 自己把 pipeline 拆掉、把 plane 还回去，然后退出媒体循环。
 *
 * 阈值历史：6 秒 → 60 秒 → 180 秒。别改小，也别只看 js_poll：
 * JS 定时器会被系统熄屏挂起（270s hal-screen down），把正常对局误判成
 * 「小程序走了」—— 详见下面 bug1 那段和 docs/BUGFIX-bug1-lifecycle.md。 */

/* ===== bug1 修正：看门狗不能只看 js_poll =====
 *
 * 上面这套「60 秒没 poll 就拆」在真机上有个致命误判：本固件
 * /etc/input-event-daemon.conf 的 [Idle] 段是
 *     270s = hal-screen down; 300s = len_onoff.sh off; hal-screen off
 * 熄屏之后 Falcon 会挂起 QuickJS 定时器，media.js 那 120ms 一次的 poll 停摆，
 * 于是 60 秒后这里就打 `JS heartbeat lost` + code=-6 把管线拆掉。
 * 用户看到的就是「打开约四分钟后画面停了 / 像自己退出了」——
 * 而进程、socket、网关推流全都还活着，是我们在自伤。
 *
 * 所以心跳改成 max(js_poll 时间, 最近一次收到字节的时间)：
 *   - 还有 RTP/RTCP 进来 → 内核 socket 还开着、网关还在推、进程还活着，
 *     这正是「这个会话该不该继续」的真实判据，比 JS 定时器可靠得多；
 *   - 反过来，小程序真被切走 / 进程真被杀时 socket 随之关闭、包自然停，
 *     一个阈值之后照样能拆，**拆管线的路径完整保留**（不能改成永不拆：
 *     KMS plane 会永久盖住系统 UI，见 HANDOFF.md 里 GST_STATE_NULL_ 那个坑）。
 *
 * 阈值 180 秒：270s 的 hal-screen down 会跨过旧的 60s，而 180s 能容忍
 * 熄屏 + 短暂挂起，又远小于 900s 的 system_sleep_wakeup。
 * 需要现场缩短观察窗口时用 -DPEN_CG_WATCHDOG_MS=60000 覆盖，不必改代码。
 *
 * 另外加一条**只告警不拆**的中间档：JS 静默超过 60s 但流还在正常收，
 * 打一次 `JS silent` 标记。这是「屏熄了/JS 被挂起，但流还活着」的现场指纹，
 * 真机排查时 grep 这一行就能把本条 bug 和"网关卡流"区分开。 */
#ifndef PEN_CG_WATCHDOG_MS
#define PEN_CG_WATCHDOG_MS 180000
#endif
#define WATCHDOG_MS PEN_CG_WATCHDOG_MS
#define WATCHDOG_WARN_MS 60000
#define WATCHDOG_CHECK_MS 500

/* ---- 视频上屏看门狗（终末地 mrfzzmd 全黑 / 冻结的正面修复）----
 *
 * 现象（真机 2026-11-14，同一个包、连续三次会话）：
 *   A 局：`AU push` 每秒 15~20 帧、`AULD` 每帧指纹都不同、`appsrc push
 *         failed` 为 0，但 `plane[76]` 三分钟里 `crtc=(null) fb=0`，
 *         kmsgrab 报 `Plane 76 does not have an attached framebuffer.`
 *         —— 视频区全黑，用户听到的却是正常游戏音。
 *   B 局：80 秒后平面才绑上，随后 70 秒里平面 md5 完全不变（画面冻结）。
 *   C 局：同样码流、同样构建，`displaying fb` 每秒 20 次，290 秒全程正常。
 *
 * 内核日志（A 局那 105 秒内）：
 *   mpp_rkvdec2 ff340100.rkvdec: session 32 task 1996xx
 *       irq_status 0x001123 timeout 0 abort 0 force_dequeue 1
 *   mpp_rkvdec2 ff340100.rkvdec: resetting... / reset done      （共 21 次）
 * 硬件解码器报错被复位，参考帧全部失效；而终末地一局只给 1 个 IDR
 * （后续 IDR 都被丢包打碎，网关又从不响应我们的 NACK，见
 *  cm_rtp.c 的 NACK 段注释），所以解码器再也回不到可解码状态。
 *
 * 判据必须是「pushed 在涨、shown 不动」：
 *   - 只看 pushed 不够 —— appsrc 是 leaky 的，队列满时它会默默丢旧 AU，
 *     pushed 照样上涨，把下游停摆掩盖掉；
 *   - 只看 shown 不动也不够 —— 起播初期本来就没帧。
 * 两个条件同时成立 VWD_STALL_MS，判定「下游停止消费」，先补 PLI，
 * 再宽限 VWD_PLI_GRACE_MS；还是不出画就报 -7 交 JS 换会话。 */
#define VWD_CHECK_MS        500    /* 检查粒度 */
#define VWD_STALL_MS        2500   /* pushed 涨而 shown 不动的容忍时长 */
#define VWD_PLI_GRACE_MS    1500   /* 先求 IDR 的宽限期，过了才交 JS */
#define VWD_BACKLOG_SLACK   60     /* 超过正常管线延迟多少帧才算下游停摆 */
/* ★ 2026-11-16 新增：解码器一级（decoded 不涨）的判据与恢复额度。
 *
 * 1.3.2 的三段计数把根因钉死在解码器这一级：pushed 913→959 时
 * decoded 与 shown 双双死钉 821，bus 无 ERROR、appsrc 无 flow error。
 * 也就是说「发 PLI 求 IDR」是治错的病 —— IDR 进得来，是解码器不消费。
 * 所以现在先按 decoded 判、先做 flush 恢复，恢复不动才退回原来的
 * 「求 IDR → 交 JS」两级。 */
#define VWD_DEC_STALL_MS    1000   /* decoded 停涨多久就认定解码器停摆 */
/* 单会话最多做几次递进式恢复：第 1 次 flush，第 2 次 READY 重置整条管线，
 * 第 3 次仍不动就放弃、交给下面的「求 IDR → 交 JS」两级。 */
#define VWD_MAX_RECOVERS    3

/* ★ 为什么不再自己重建管线（2026-11-15 真机定案）★
 *
 * 这一局原神的 IDR 是**周期性**的，间隔精确 604 秒（=10 分钟）：
 *   kf=1 08:02:49 / kf=2 08:13:00 / kf=3 08:23:04 / kf=4 08:33:08
 * 而 PLI 对网关完全无效（整局 506 条 PLI，零响应，recov=0）。
 *
 * 于是「拆管线重建」= 新建的 mppvideodec 拿不到 IDR，参考帧全空，
 * 只能等下一个周期 IDR —— 平均 5 分钟、最坏 10 分钟黑屏。
 * 真机 08:12:40 就是这样：2.5 秒的小抖动触发重建，画面直到 08:13:00
 * 的周期 IDR 才回来（黑 20 秒）；08:38 再触发一次就一直黑到被换会话。
 * 更糟的是旧代码的 s_vwd_rebuilds 只在换会话时清零，上午用掉的额度
 * 下午还在，一次小抖动就直接把 -7 抛给 JS（用户看到的「卡 2 秒→闪黑」）。
 *
 * 而「交回 JS 换会话（L2 复用网关）」只要约 5 秒，且**游戏进度不丢**
 * （服务端对局继续跑，只是换了一条媒体通道，实测 56 分钟的局一直在）。
 * 每局新会话起播时网关必定先发一个 IDR（`AU push key=1`），所以
 * 换会话 = 立刻拿到新 IDR，比等 604 秒快两个数量级。
 *
 * 结论：先发 PLI 求一次（万一以后网关开始响应），宽限 VWD_PLI_GRACE_MS
 * 之后直接 -7 交给 JS，绝不自己拆管线。 */

/* pushed - shown 的正常水位（滑动均值）。
 * 真机 2026-11-15：健康会话里这个差值长期稳定在 142~157 帧，
 * 那是 probe 采样时刻与 kmssink 实际取帧之间的管线延迟，**不是停摆**。
 * 旧代码的统计行判据是 pushed != shown 就打 "(DOWNSTREAM STALLED)"，
 * 于是整局每 10 秒都在喊停摆，把真正的冻结淹掉了。 */
static uint64_t s_sink_backlog_avg = 0;

static void emit_stat(cm_sess *s) {
    uint64_t now = cm_now_ms();
    uint64_t dt = now - s->stat_base_ms;
    uint64_t db = s->stats.bytes_rx - s->stat_base_bytes;
    uint64_t kbps = 0;
    uint64_t df = s->stats.frames - s->fps_base_frames;
    uint64_t dms = now - s->fps_base_ms;
    /* jitterMs：RR 上报的 jitter 是 90kHz RTP 单位（cm_rtp.c 的 jitter_q4>>4），
     * 前端要的是毫秒，除以 90 即可。之前这个字段是硬编码 0，等于把网关侧
     * 最关心的"链路抖不抖"藏了起来。 */
    uint64_t jitter_ms = (s->stats.jitter_q4 >> 4) / 90ULL;

    if (dt > 0) kbps = (db * 8ULL) / dt;
    if (dms >= 500) {
        s->fps = (int)((df * 1000ULL) / dms);
        s->fps_base_frames = s->stats.frames;
        s->fps_base_ms = now;
    }

    /* 健康检查：把「最可能卡住整个链路」的两种状态显式打出来。
     *
     * 这两个判断是 bug 3 / bug 4 真机定位的捷径，不靠肉眼猜：
     *   - 有包进来但一帧 IDR 都没收到 → 要么网关不理 PLI，要么 PLI 根本没
     *     送到（SRTCP 未就绪时 cm_cmd.c 会退回明文，明文在 SRTP 会话里必被丢）。
     *     这时画面只能全灰/全绿，任何上层重试都是无效功。
     *   - 收了不少包但一帧都没推给解码器 → 说明闸门和丢包防护把帧全丢了，
     *     需要看 auDrop/gateDrop 判断是「丢包太狠」还是「根本没关键帧」。 */
    if (s->stats.rtp_rx > 0) {
        static uint64_t last_health_ms = 0;
        if (now - last_health_ms >= 10000) {
            last_health_ms = now;
            if (!s->have_kf && s->stats.vid_gate_drop > 0) {
                CM_LOGW("#%d still no IDR (rx=%llu kfReq=%llu gateDrop=%llu "
                        "auDrop=%llu nack=%llu recov=%llu relax=%llu) — "
                        "gateway may be ignoring PLI / not retransmitting",
                        s->id, (unsigned long long)s->stats.rtp_rx,
                        (unsigned long long)s->stats.vid_kf_req,
                        (unsigned long long)s->stats.vid_gate_drop,
                        (unsigned long long)s->stats.vid_au_drop,
                        (unsigned long long)s->stats.vid_nack_sent,
                        (unsigned long long)s->stats.vid_nack_recovered,
                        (unsigned long long)s->stats.vid_relax);
            } else if (s->have_kf && s->stats.vid_au_drop > 0) {
                CM_LOGW("#%d packet loss seen (lost=%llu auDrop=%llu pDrop=%llu dup=%llu "
                        "reorder=%llu nack=%llu recov=%llu staleRtx=%llu relax=%llu gapForgot=%llu "
                        "sliceUnknown=%llu frameGap=%llu) "
                        "— broken AUs dropped, not shown",
                        s->id, (unsigned long long)s->stats.vid_lost,
                        (unsigned long long)s->stats.vid_au_drop,
                        (unsigned long long)s->stats.vid_p_drop,
                        (unsigned long long)s->stats.vid_dup,
                        (unsigned long long)s->stats.vid_reorder,
                        (unsigned long long)s->stats.vid_nack_sent,
                        (unsigned long long)s->stats.vid_nack_recovered,
                        (unsigned long long)s->stats.vid_stale_rtx,
                        (unsigned long long)s->stats.vid_relax,
                        (unsigned long long)cm_rtp_gap_forgot(),
                        (unsigned long long)cm_rtp_slice_unknown(),
                        (unsigned long long)s->stats.vid_frame_gap);
            }
        }
    }
    cm_emit(s,
            "{\"t\":\"stat\",\"rx\":%llu,\"lost\":%llu,\"jitterMs\":%llu,\"bitrateKbps\":%llu,"
            "\"decode\":%llu,\"frames\":%llu,\"authFail\":%llu,\"bad\":%llu,"
            "\"kf\":%llu,\"pli\":%llu,\"w\":%d,\"h\":%d,\"fps\":%d,"
            "\"rxPkts\":%llu,\"rxStun\":%llu,\"rxKept\":%llu,\"rxDtls\":%llu,"
            "\"consentSent\":%llu,\"consentResp\":%llu,"
            "\"rxOther\":%llu,\"aPkts\":%llu,\"aBytes\":%llu,"
            "\"auDrop\":%llu,\"gateDrop\":%llu,\"kfReq\":%llu,\"dup\":%llu,"
            "\"reorder\":%llu,\"spsInject\":%llu,\"rtcpIn\":%llu,\"rrSent\":%llu,"
            "\"nackSent\":%llu,\"nackReq\":%llu,\"nackRecovered\":%llu,"
            "\"relax\":%llu,\"relaxDrop\":%llu,\"staleRtx\":%llu,\"pDrop\":%llu,"
            /* join = 「一个残缺的切片组被接回同一帧」的次数（cm_rtp.c 的
             * au_joined）。终末地修复的判据：这个数持续增长说明 AU 边界
             * 确实被接回来了；恒为 0 说明这一局压根没有多 slice 帧。 */
            "\"join\":%llu,"
            /* holdRel = 「压住的 AU 被超时兜底放行」的累计次数（cm_rtp.c 的
             * au_hold_released）。与 join 配合看能区分三种情况：
             *   join 涨、holdRel 不涨 => 正常接回，没有走兜底（理想）
             *   join 不涨、holdRel 涨 => 一直在走兜底，说明边界判据没生效
             *   两个都不涨且 shown 也不涨 => AU 被永久压住（死锁，不该出现） */
            "\"holdRel\":%llu,"
            /* shown = 真正送进 kmssink 的帧数（sink pad 探针）。
             * 判「画面在不在动」只能看它：pushed/fps 只代表交给了 appsrc，
             * 而 appsrc 是 leaky 的，队列满就默默丢帧，会把下游停摆掩盖掉。
             * 真机指纹：shown 长时间不涨而 fps 一直有 → 全黑/冻结。 */
            "\"shown\":%llu,\"decoded\":%llu}",
            (unsigned long long)s->stats.rtp_rx,
            /* ★ 这里原来是硬编码 0，JS 侧看到的 lost 永远是 0，等于把
             * 「到底丢没丢包」这个最关键的信息扔掉了。现在用 RTP 层
             * 序号缺口估算的真实丢包数（cm_rtp.c 维护）。 */
            (unsigned long long)s->stats.vid_lost,
            (unsigned long long)jitter_ms,
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
            (unsigned long long)s->ice_consent_count,
            (unsigned long long)s->stats.rx_consent_resp,
            (unsigned long long)s->stats.rx_dtls,
            (unsigned long long)s->stats.rx_other,
            (unsigned long long)s->stats.audio_pkts,
            (unsigned long long)s->stats.audio_bytes,
            (unsigned long long)s->stats.vid_au_drop,
            (unsigned long long)s->stats.vid_gate_drop,
            (unsigned long long)s->stats.vid_kf_req,
            (unsigned long long)s->stats.vid_dup,
            (unsigned long long)s->stats.vid_reorder,
            (unsigned long long)s->stats.vid_sps_inject,
            (unsigned long long)s->stats.rx_rtcp,
            (unsigned long long)s->rr_count,
            /* NACK / 饥饿兜底的可观测通道（REVIEW-report L1）：
             * 之前这几个计数只在 CM_LOGD 里打，而默认 log_level=3 不打，
             * 真机上完全看不见 —— 无法判断 NACK 有没有生效。
             *   nackSent 涨 + nackRecovered=0 → 网关不重传（或 NACK 没送到）
             *   nackSent 涨 + nackRecovered 同步涨 → 重传在工作，重点看 auDrop 是否下降
             *   relax>0 → 已经靠「接受花屏」换画面在动，说明前两层没救回 */
            (unsigned long long)s->stats.vid_nack_sent,
            (unsigned long long)s->stats.vid_nack_req,
            (unsigned long long)s->stats.vid_nack_recovered,
            (unsigned long long)s->stats.vid_relax,
            /* relaxDrop = 画质优先策略下被闸门拒收的 P 帧数（见 cm_session.h）。
             * 与 relax 互斥，用来在真机上区分用户当前生效的是哪一档。 */
            (unsigned long long)s->stats.vid_relax_dropped,
            (unsigned long long)s->stats.vid_stale_rtx,
            /* pDrop = 因为丢包整帧丢掉的 P 帧个数（L1）。
             * 和 gateDrop 的区别：gateDrop 是「没关键帧所以不放行」，
             * pDrop 是「有关键帧、只丢拼不全的某一帧」—— 后者增长但 fps
             * 仍稳定，就是弱网下我们要的健康态。 */
            (unsigned long long)s->stats.vid_p_drop,
            (unsigned long long)cm_rtp_au_joined(),
            (unsigned long long)cm_rtp_au_hold_released(),
            (unsigned long long)cm_box_shown(s->vbox),
            (unsigned long long)cm_box_decoded(s->vbox));
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
            /* ★ 画面健康度三件套，10 秒一行（终末地全黑/冻结的判据）：
             *   pushed + shown 都在涨 → 正常
             *   pushed 涨、shown 不动 → 下游停止消费（看门狗会重建解码器）
             *   两个都不动        → 根本没数据到，问题在上游/网关
             * 把这行留在生产日志里，是因为「有声音没画面」这类报障只能靠它定位，
             * 而 cm_emit 的 stat JSON 不进日志文件。 */
            CM_LOGI("#%d video sink pushed=%llu parsed=%llu vdecin=%llu decoded=%llu shown=%llu queue=%llu recov=%llu join=%llu holdRel=%llu gapForgot=%llu gapLate=%llu auRep=%llu sliceUnknown=%llu frameGap=%llu%s", s->id,
                    (unsigned long long)cm_box_pushed(s->vbox),
                    (unsigned long long)cm_box_parsed(s->vbox),
                    (unsigned long long)cm_box_vdecin(s->vbox),
                    (unsigned long long)cm_box_decoded(s->vbox),
                    (unsigned long long)cm_box_shown(s->vbox),
                    (unsigned long long)cm_box_queue_depth(s->vbox),
                    (unsigned long long)cm_box_recoveries(s->vbox),
                    (unsigned long long)cm_rtp_au_joined(),
                    (unsigned long long)cm_rtp_au_hold_released(),
                    (unsigned long long)cm_rtp_gap_forgot(),
                    (unsigned long long)s->stats.vid_gap_late,
                    (unsigned long long)s->stats.vid_au_repaired,
                    (unsigned long long)cm_rtp_slice_unknown(),
                    (unsigned long long)s->stats.vid_frame_gap,
                    (s->vbox && s_sink_backlog_avg > 0 &&
                     cm_box_pushed(s->vbox) >
                         cm_box_shown(s->vbox) + s_sink_backlog_avg + VWD_BACKLOG_SLACK)
                        ? " (DOWNSTREAM STALLED)" : "");
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
    /* 看门狗状态（见循环体里的两级判定）。同一时刻只有一个媒体会话，
     * 放在循环内局部即可，退出循环自动清；s->id 变化时在循环里重置。 */
    uint64_t s_wd_last_check = 0;
    uint64_t s_wd_progress = 0;
    uint64_t s_wd_last_bytes = 0;
    uint64_t beat = 0;
    uint64_t silent = 0;
    int s_wd_sid = -1;
    int s_wd_warned = 0;
    /* 上屏看门狗状态（同一个会话内用，换局时重置） */
    uint64_t s_vwd_last_check = 0;
    uint64_t s_vwd_last_pushed = 0;
    uint64_t s_vwd_last_shown = 0;
    uint64_t s_vwd_frozen_since = 0;
    int s_vwd_sid = -1;
    /* 已经报过 -7（只报一次，别每 500ms 刷屏）。
     * 用完由会话重建时（s_vwd_sid 变化）清零。
     * 注意：这里刻意不再有 rebuilds 计数 —— 本文件已不自己重建管线，
     * 原因见 VWD_PLI_GRACE_MS 上方那段注释。 */
    uint64_t s_vwd_pli_at = 0;
    int s_vwd_gave_up = 0;
    /* 解码器一级的判定状态（见 VWD_DEC_STALL_MS 注释）：
     *   s_vwd_last_decoded  上一轮的 decoded，用来判断解码器是否还在出帧
     *   s_vwd_dec_since     decoded 开始不涨的时刻
     *   s_vwd_recovers      本会话已就地恢复（flush）几次 */
    uint64_t s_vwd_last_decoded = 0;
    uint64_t s_vwd_dec_since = 0;
    int s_vwd_recovers = 0;
    /* 短包日志限速（见 plen == -2/-5 分支）：上次打印时刻 + 这 1 秒内被抑制的条数 */
    uint64_t s_last_short_ms = 0;
    uint64_t s_short_suppressed = 0;
    /* 判定阈值的当前值：初值 VWD_STALL_MS（2.5s），发过 PLI 之后
     * 再加上 VWD_PLI_GRACE_MS（1.5s），即总共等 4 秒才报 -7。
     * 画面一恢复就回到 2.5s。
     *
     * 2026-11-16 收紧（原 6s + 4s = 10s）：`IDR_HOLD_MS` 修好后，重传
     * 窗口只需要 ~250ms，剩下的时间都在白等；而用户感知的「卡一下」正是
     * 这段白等 + 换会话建连。4 秒足以覆盖一次「PLI + 重传往返」。 */
    uint64_t s_vwd_wait_ms = VWD_STALL_MS;

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
    /* 换局/重连时把 RTP 拆包状态复位：g_rtp 是进程级单例（cm_rtp.c:35），
     * 上一局的 seq 基准、半截 AU、旧 SPS/PPS 会留到这一局。症状是
     * 「新开一局一上来就绿屏/花屏」——正是 bug 4 的形态之一：
     * 旧 SPS 先到、拿它初始化解码器，随后新码流的 IDR 尺寸/参数全对不上。 */
    cm_rtp_reset(s);
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
            /* RTCP（192..223）。旧代码在这里只打一行日志就完事，等于把
             * 「网关告诉我们丢了包 / 网关在等我们的接收报告」这两条信道
             * 全部扔了 —— 这是「丢包后恢复慢」和「网关一直不给新 IDR」
             * 最可能的服务端侧原因（很多 SFU 收不到 RR 就降级甚至停发）。
             *
             * 现在做三件确定的事：
             *   1. 统计 rx_rtcp（stat 事件里上报 rtcpIn），用来判断
             *      「网关到底会不会回 RTCP」。长期为 0 = 单向链路，
             *      说明 PLI 很可能根本没被对端看到，只能靠周期顿挫死磕。
             *   2. 认出 NACK（RTPFB, PT=205, fmt=1）**只打日志，绝不动 AU**。
             *      我们是纯接收端（cm_rtp_on_packet 只有收包路径，没有任何
             *      发送媒体的地方）。对端发 NACK 指的是「它没收到**我们发给
             *      它的**包」，可我们从没发过媒体 —— 这条 NACK 语义上不可能
             *      针对我们的视频流（可能属于对端上行，或干脆是乱发的）。
             *      旧代码在这里调 cm_rtp_note_stale(s,"peer NACK")，等于
             *      「对端一抱怨就丢我们自己的整帧」：note_stale 会置
             *      au_broken、vid_au_drop++ 并补发 PLI，对端若周期性发 NACK
             *      就按 NACK 频率丢帧，比原 bug 更糟。见 REVIEW-report R6。
             *   3. 加密的 SRTCP 解不开（cm_srtp.c 只解密 RTP），看不出
             *      类型时**不做任何破坏性动作**，只计数 —— 否则网关卡 1Hz
             *      的 RR 会让每循环丢一帧，比原 bug 更糟。 */
            if (n >= 2) {
                int rpt = buf[1] & 0xFF;
                if (rpt >= 192 && rpt <= 223) {
                    int fmt = buf[0] & 0x1F;
                    /* 205 = RTPFB，fmt=1 = Generic NACK。逐个打出来便于判断
                     * 「网关到底会不会发 NACK」，但**不做任何 AU 侧动作**：
                     * 纯接收端收到 NACK ≠ 我们的接收路径出了问题（见上方 2）。 */
                    if (rpt == 205 && fmt == 1) {
                        CM_LOGD("#%d RTCP peer NACK (RTPFB fmt=1, ignored: we are "
                                "receive-only)", s->id);
                    } else {
                        CM_LOGD("#%d RTCP %d bytes fmt=%d pt=%d", s->id, n, fmt, rpt);
                    }
                } else {
                    CM_LOGD("#%d RTCP %d bytes (srtcp, not parsed)", s->id, n);
                }
            }
        } else if (cls == 1 && n > 0) {
            int plen = cm_srtp_unprotect(s, buf, n);
            if (plen > 0) {
                cm_rtp_on_packet(s, buf, plen, ip, port);
            } else if (plen == -7) {
                /* ★ 认证失败：旧代码只打一行 CM_LOGD 就 over，丢一个包算完。
                 * 若丢的是 FU-A 中间片，后面的片照样拼进同一个 AU 并被推给
                 * 解码器 —— 这就是「上一帧马赛克残留」的第二条上游通路。
                 * 现在把当前 AU 标脏（flush 时整帧丢弃）+ 补一次 PLI。
                 *
                 * 注意：RTP 头在 SRTP 里是明文，所以能看出这是不是视频包。
                 * 音频包的认证失败不该触发视频 PLI，否则会白白打断画面。 */
                if (n >= 2) {
                    int ept = buf[1] & 0x7F;
                    if (ept == s->cfg.video_pt) {
                        CM_LOGW("#%d srtp auth fail on video pkt %d bytes", s->id, n);
                        cm_rtp_note_stale(s, "srtp auth fail");
                    } else {
                        CM_LOGD("#%d srtp auth fail (pt=%d)", s->id, ept);
                    }
                } else {
                    CM_LOGD("#%d srtp auth fail (%d bytes)", s->id, n);
                }
            } else if (plen == -2 || plen == -5) {
                /* ★ 2026-11-18：这里原来是一行无条件 CM_LOGD，打开 cdebug.txt
                 * 之后每 20 秒能刷出几百行 25 字节的短包（STUN/RTCP 走错 SRTP
                 * 分支的产物），把真正有用的诊断全淹掉了 —— 晚自习那份 11 万
                 * 行日志里九成是它。改成 1 秒最多一条 + 计数，噪声降三个量级
                 * 而信息量不减（短包与画面无关，只需知道有没有、有多少）。 */
                uint64_t sn = cm_now_ms();
                if (sn - s_last_short_ms >= 1000) {
                    s_last_short_ms = sn;
                    CM_LOGD("#%d short packet %d bytes (suppressed %llu in last 1s)",
                            s->id, n, (unsigned long long)s_short_suppressed);
                    s_short_suppressed = 0;
                } else {
                    s_short_suppressed++;
                }
            }
        }

        now = cm_now_ms();
        /* IDR hold 到期放行：flush_frame 会对残缺 IDR 挂起 IDR_HOLD_MS
         * 等重传补齐，到期必须有人把它放出来，否则画面停在这一帧。
         * 20ms 粒度足够（hold 窗口是几十 ms 量级）。 */
        cm_rtp_flush_held(s);
        /* 多 slice AU 的兜底放行（见 cm_rtp.c 文件开头「切片区」大注释）：
         * marker 之后我们先压住，等下一帧的第一个 slice 到来才算确认帧边界。
         * 码流一停就没人放行，这里到期放出去。 */
        cm_rtp_au_join_tick(s);
        /* 取证开关 noRtcp=1 时三种上行 RTCP 全部不发明文/密文，用来二分
         * 「网关是否因为我们的 PLI/NACK/RR 而停推」。正常路径恒为 0。 */
        if (!s->cfg.no_rtcp) cm_kf_tick(s);
        /* RTCP Generic NACK（RFC 4585 §6.2.1）：把收包侧登记的缺口
         * 点名要回来。内部自带 30ms 限速与"缺口表空就不发"的短路，
         * 每轮调一次没有代价。★ 这是「弱网丢一包就整帧报废」的正面修复：
         * 没有它，一个 IDR 丢一片就永远等不到完整 IDR（真机 2026-11-13
         * 高铁实测：画面冻结 12 分钟以上、PLI 发 468 次无效）。 */
        if (!s->cfg.no_rtcp) cm_nack_tick(s);
        /* RTCP Receiver Report（RFC 3550 §6.4.2）：每秒一个 RR，走 SRTCP。
         * 放在 cm_kf_tick 旁边是刻意的 —— 两者都是"我们主动对网关说的话"，
         * 一个说"缺关键帧"，一个说"我还在收、丢包率/抖动现在是这个数"。
         *
         * ★ 定位：这是补齐协议义务，**不是** bug1（约四分钟停推）的修复方案。
         * 现场证据（音视频同时停在 4m18s±1s、网关全程不回任何 RTCP、WS 会话
         * 健康）指向服务端挂机 kickout 定时器。理由与完整分析见
         * docs/BUGFIX-bug1-rtcp-rr.md 与 cm_cmd.c 的 RR 段注释。 */
        if (!s->cfg.no_rtcp) cm_rtcp_tick(s);
        /* ICE consent refresh（RFC 8445 §11）：每 2.5s 发一次带认证的
         * Binding Request，证明「我们还在」。这是与官方客户端唯一的
         * 周期性双向报文差异 —— 网关是 ice-lite，它自己会问我们，但
         * consent 是反方向义务。不加这个，约 4m18s 后网关单方面停推。
         * 内部按 next_ice_consent_ms 限速，每轮调无代价。 */
        cm_ice_consent_tick(s);
        /* ---- 视频上屏看门狗：pushed 在涨、shown 不动 ⇒ 下游停止消费 ----
         * 判据与取舍见文件头 VWD_* 的注释。分两级：
         *   1) VWD_STALL_MS（2.5 秒）没上屏 → 只发 PLI 求 IDR，**不拆管线**
         *   2) 再宽限 VWD_PLI_GRACE_MS（1.5 秒）仍没上屏 → 报 -7 交 JS 换会话，
         *      **仍不拆管线**。合计 4 秒，比 11-15 定案的 6+4=10 秒收紧，
         *      因为 IDR hold 修好后重传窗口只需 ~250ms，剩下都是白等，
         *      而用户感知的「卡一下」正是这段白等加上换会话建连的时间。
         * 为什么不拆：这一局的 IDR 间隔实测 604 秒
         * （kf 1→2→3→4 的时刻是 08:02:49 / 08:13:00 / 08:23:04 / 08:33:08），
         * 新解码器拿到 P 帧是解不出来的，一旦重建就必须再等一个 IDR ——
         * 运气不好就是黑屏十分钟；而换会话 5 秒就有新 IDR（新会话起播
         * 必然带 key=1 的 AU）。详见 VWD_PLI_GRACE_MS 上方那段注释。 */
        if (s->vbox && now - s_vwd_last_check >= VWD_CHECK_MS) {
            uint64_t vpushed = cm_box_pushed(s->vbox);
            uint64_t vshown = cm_box_shown(s->vbox);
            /* 解码器输出帧数：判定「pushed 涨而 shown 不动」到底卡在哪一级。
             * 这两个日志里带上它，现场就能分清「解码器停摆」与「下游不消费」。 */
            uint64_t vdecoded = cm_box_decoded(s->vbox);
            uint64_t vparsed = cm_box_parsed(s->vbox);
            uint64_t vvdecin = cm_box_vdecin(s->vbox);
            s_vwd_last_check = now;
            if (s_vwd_sid != s->id) {
                s_vwd_sid = s->id;
                s_vwd_last_pushed = vpushed;
                s_vwd_last_shown = vshown;
                s_vwd_last_decoded = vdecoded;
                s_vwd_dec_since = 0;
                s_vwd_recovers = 0;
                s_vwd_frozen_since = 0;
                s_vwd_pli_at = 0;
                s_vwd_gave_up = 0;
                s_vwd_wait_ms = VWD_STALL_MS;
            }
            if (vshown != s_vwd_last_shown) {
                /* 有帧真的上屏了：一切正常，清掉冻结计时。
                 * 顺手记下 pushed-shown 的水位（EMA），它是判断
                 * 「下游真停摆」还是「正常管线延迟」的唯一依据。 */
                uint64_t backlog = (vpushed > vshown) ? (vpushed - vshown) : 0;
                s_sink_backlog_avg = s_sink_backlog_avg
                    ? (s_sink_backlog_avg * 15 + backlog) / 16
                    : backlog;
                s_vwd_last_shown = vshown;
                s_vwd_last_pushed = vpushed;
                s_vwd_frozen_since = 0;
                s_vwd_pli_at = 0;
                s_vwd_gave_up = 0;
                s_vwd_wait_ms = VWD_STALL_MS;
            } else if (vpushed != s_vwd_last_pushed) {
                /* ---- 第 0 级：解码器这一级先判、先救（2026-11-16 新增）----
                 *
                 * 1.3.2 三段计数定案：pushed 913→959 时 decoded 与 shown
                 * 双双死钉 821，bus 无 ERROR、appsrc 无 flow error。
                 * 症结是 h264parse/mppvideodec 静默停止消费，而不是缺 IDR
                 * （缺 IDR 会让 decoded 继续跟着 pushed 走，只是画面是坏的）。
                 * 所以先看 decoded：它停涨就 flush 上游逼重新协商，
                 * 恢复不动再退回下面「求 IDR → 交 JS」那两级。
                 * 这一步的价值是把「换会话（闪黑 + 重连 5 秒）」换成
                 * 「就地恢复（不碰状态机、不解绑平面）」。 */
                if (vdecoded != s_vwd_last_decoded) {
                    s_vwd_last_decoded = vdecoded;
                    s_vwd_dec_since = 0;
                } else {
                    if (!s_vwd_dec_since) {
                        s_vwd_dec_since = now;
                    } else if (now - s_vwd_dec_since >= VWD_DEC_STALL_MS &&
                               s_vwd_recovers < VWD_MAX_RECOVERS) {
                        uint64_t qd = cm_box_queue_depth(s->vbox);
                        int attempt = s_vwd_recovers + 1;
                        int did = 0;
                        /* 递进式恢复（2026-11-16 23:0x 真机定案）：
                         *   第 1 次 → flush 上游（便宜，不碰状态机）
                         *   第 2 次 → READY 重置整条视频管线
                         * flush 救不回来的根因已经查明：dmesg 里
                         * `mpp_rkvdec2 ... irq_status 0x000017 / resetting /
                         * reset done` —— 硬件解码会话已被驱动自己重置，
                         * flush 事件到不了已死的会话，只有让 GStreamer
                         * 重新实例化 h264parse/mppvideodec（连带新的硬件
                         * 会话）才能恢复。 */
                        CM_LOGW("#%d decoder stalled %llu ms "
                                "(pushed=%llu parsed=%llu vdecin=%llu decoded=%llu "
                                "shown=%llu, queue=%llu) -> %s (recovery %d/%d)",
                                s->id,
                                (unsigned long long)(now - s_vwd_dec_since),
                                (unsigned long long)vpushed,
                                (unsigned long long)vparsed,
                                (unsigned long long)vvdecin,
                                (unsigned long long)vdecoded,
                                (unsigned long long)vshown,
                                (unsigned long long)qd,
                                attempt == 1 ? "flush upstream to renegotiate"
                                             : "READY reset to re-instantiate decoder",
                                attempt, VWD_MAX_RECOVERS);
                        if (attempt == 1) {
                            did = cm_box_recover_decoder(s->vbox, now);
                        } else {
                            did = cm_box_reset_decoder(s->vbox, now);
                            if (did) {
                                /* 新实例的 DPB 是空的，直接喂 P 帧只会再死
                                 * 一次。让 RTP 层回到「等 IDR」并重新要关键帧
                                 * —— cm_rtp_after_rebuild 会清 have_kf/缺口表
                                 * 并自己发一次 PLI。 */
                                cm_rtp_after_rebuild(s);
                            }
                        }
                        if (did) {
                            s_vwd_recovers++;
                            s_vwd_dec_since = 0;
                            /* 参考帧全没了，必须重新要 IDR 才能出画；顺带把
                             * 「求 IDR」这一级的计时也重置，否则会在同一轮里
                             * 立刻又发一次 -7。 */
                            if (attempt == 1) cm_send_pli(s);
                            s_vwd_frozen_since = 0;
                            s_vwd_pli_at = 0;
                            s_vwd_gave_up = 0;   /* 恢复成功一次，允许下一次再报 */
                            s_vwd_wait_ms = VWD_STALL_MS;
                        } else {
                            s_vwd_dec_since = now;   /* 没发起（限速/状态不对），下一轮再试 */
                        }
                    }
                }
                /* 我们还在往 appsrc 喂，但 kmssink 一帧都没新吃到 */
                if (!s_vwd_frozen_since) {
                    s_vwd_frozen_since = now;
                } else if (now - s_vwd_frozen_since >= s_vwd_wait_ms) {
                    /* 第一级：先求一个新 IDR 回来，别急着拆管线 */
                    if (!s_vwd_pli_at) {
                        s_vwd_pli_at = now;
                        CM_LOGW("#%d video sink stalled %llu ms "
                                "(pushed=%llu parsed=%llu vdecin=%llu decoded=%llu "
                                "shown=%llu, backlog avg=%llu) "
                                "-> request IDR (PLI), give it %d ms before "
                                "handing over to JS",
                                s->id,
                                (unsigned long long)(now - s_vwd_frozen_since),
                                (unsigned long long)vpushed,
                                (unsigned long long)vparsed,
                                (unsigned long long)vvdecin,
                                (unsigned long long)vdecoded,
                                (unsigned long long)vshown,
                                (unsigned long long)s_sink_backlog_avg,
                                VWD_PLI_GRACE_MS);
                        cm_send_pli(s);
                        /* 宽限期同样是 wms，但至少 VWD_PLI_GRACE_MS */
                        s_vwd_wait_ms += VWD_PLI_GRACE_MS;
                    } else if (!s_vwd_gave_up) {
                        /* 求 IDR 没用（网关不看 PLI）：交回 JS 换会话。
                         * 这里只报错，绝不自己拆管线 —— 拆了就要等 604 秒
                         * 的周期 IDR，而换会话 5 秒就有新 IDR。
                         * 也不自己拆会话：拆会话会让音频也断，
                         * 而用户此刻听得到的音频是「还能不能继续玩」的判据。 */
                        s_vwd_gave_up = 1;
                        CM_LOGE("#%d video still frozen %llu ms after PLI "
                                "(pushed=%llu parsed=%llu vdecin=%llu decoded=%llu "
                                "shown=%llu, backlog avg=%llu), "
                                "asking JS to reconnect instead of rebuilding",
                                s->id,
                                (unsigned long long)(now - s_vwd_frozen_since),
                                (unsigned long long)vpushed,
                                (unsigned long long)vparsed,
                                (unsigned long long)vvdecin,
                                (unsigned long long)vdecoded,
                                (unsigned long long)vshown,
                                (unsigned long long)s_sink_backlog_avg);
                        cm_emit(s, "{\"t\":\"error\",\"msg\":\"video sink frozen\","
                                   "\"code\":-7}");
                    }
                }
            } else {
                /* pushed 也没动：没有新数据，不是下游的问题，不计冻结。
                 * decoded 也一起记住，免得「上游停发」被误判成解码器停摆。 */
                s_vwd_last_pushed = vpushed;
                s_vwd_last_decoded = vdecoded;
                s_vwd_dec_since = 0;
            }
        }

        if (now - s->last_stat_ms >= STAT_INTERVAL_MS) {
            /* 每个统计周期顺手抽一次 bus：opusdec/alsasink/kmssink 的致命错误
             * 只会出现在这里，不打出来就是「有包没声/没画」排查黑洞。 */
            if (s->abox) (void)cm_box_drain_bus(s->abox);
            if (s->vbox) (void)cm_box_drain_bus(s->vbox);
            emit_stat(s);
        }

        /* 看门狗：小程序被切走 / 被冻结后 QuickJS 定时器停摆，poll 断了。
         * 这时必须自己拆管线，否则 KMS 平面永远盖着系统 UI（用户报障的根因）。
         *
         * bug1 之后是两级判定，心跳取 max(JS poll, 最近一次收到字节)：
         *   静默 > 60s 但流还在 → 只打一次 `JS silent`（屏熄/JS 挂起的指纹，不拆）
         *   静默 > 180s        → 拆管线退出（不再因为"屏黑了"自伤）
         * 每秒最多算一次，别每轮 20ms 都算。
         * PenCG 同时只有一个媒体会话（media.js 的 S.mediaId 单例），
         * 所以这些状态用循环内局部变量就够了，退出循环自然清掉；s->id 变了就重置。 */
        if (now - s_wd_last_check >= WATCHDOG_CHECK_MS) {
            s_wd_last_check = now;
            if (s_wd_sid != s->id) {
                s_wd_sid = s->id;
                s_wd_progress = now;
                s_wd_last_bytes = s->stats.bytes_rx;
                s_wd_warned = 0;
            }
            /* 字节数变了 = 真的还有数据流。这是比 JS 定时器可靠得多的存活证据：
             * socket 还开着、网关还在推、进程还在跑，就是"这个会话该继续"。 */
            if (s->stats.bytes_rx != s_wd_last_bytes) {
                s_wd_last_bytes = s->stats.bytes_rx;
                s_wd_progress = now;
            }
            beat = s->last_poll_ms > s_wd_progress ? s->last_poll_ms : s_wd_progress;
            /* ★ 必须写成「先比大小再相减」这种对无符号下溢安全的比较。
             * 踩过的坑：js_poll() 在 JS 线程里刷新 last_poll_ms，媒体线程刚取完 now
             * 之后 JS 线程正好又写了一个更大的值（同一毫秒内相差 1ms），
             * `now - last_poll_ms` 就下溢成 2^64-1，日志打印
             * `JS heartbeat lost (18446744073709551615 ms)` 并立刻拆掉正在玩的对局。
             * 这是 volatile 不够用的典型竞态——跨线程读一个 64 位值必须防下溢。 */
            if (beat && now > beat) {
                silent = now - beat;
                if (silent > WATCHDOG_MS) {
                    CM_LOGW("#%d JS heartbeat lost (%llu ms silent, rx=%llu bytes), "
                            "tearing down pipeline + plane",
                            s->id, (unsigned long long)silent,
                            (unsigned long long)s->stats.bytes_rx);
                    cm_emit(s, "{\"t\":\"error\",\"msg\":\"JS heartbeat lost, session torn down\",\"code\":-6}");
                    break;
                }
                if (silent > WATCHDOG_WARN_MS && !s_wd_warned) {
                    s_wd_warned = 1;
                    CM_LOGW("#%d JS silent %llu ms but stream alive (rx=%llu bytes) "
                            "— screen off / JS suspended, keeping pipeline",
                            s->id, (unsigned long long)silent,
                            (unsigned long long)s->stats.bytes_rx);
                }
            }
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
