/*
 * cm_gst.c —— GStreamer 出画/出声
 *
 * 设备上没有 GStreamer 开发头文件，也没有 webrtcbin，所以这里：
 *   - 手写最小的 GStreamer/GLib 函数声明（同 httpjson.c 对 libcurl 的做法）
 *   - 每条 m-line 一个 "盒子"：appsrc → 解析 → 硬解 → 显示/声卡
 *   - 推流直接调用 gst_app_src_push_buffer（appsrc 内部线程安全，且 block=false
 *     时不会阻塞收包线程；配合 leaky-type=downstream 丢旧数据）
 *
 * 拆除顺序很关键：GST_STATE_NULL → unref，否则 KMS 平面不释放，UI 永远被盖住。
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#include "cm_session.h"

/* ==================== 最小 GStreamer 声明 ==================== */

typedef struct _GstElement GstElement;
typedef struct _GstPipeline GstPipeline;
typedef struct _GstBin GstBin;
typedef struct _GstAppSrc GstAppSrc;
typedef struct _GstBuffer GstBuffer;
typedef struct _GstCaps GstCaps;
typedef struct _GstPad GstPad;
typedef struct _GError GError;
typedef int GstState;
typedef int GstFlowReturn;
typedef unsigned long long GstClockTime;

/* GstAppLeakyType */
#define GST_APP_LEAKY_NO 0
#define GST_APP_LEAKY_DOWNSTREAM 2

/* GstState 枚举（GStreamer 1.x 实测值，顺序极易搞错）：
 *   GST_STATE_VOID_PENDING = 0   <- 不是 NULL！
 *   GST_STATE_NULL         = 1
 *   GST_STATE_READY        = 2
 *   GST_STATE_PAUSED       = 3
 *   GST_STATE_PLAYING      = 4
 * 之前这里把 NULL 写成 0，等于传 VOID_PENDING（非法目标态），
 * gst_element_set_state 直接失败返回，pipeline 从没进 NULL，
 * kmssink 一直占着 KMS 平面 -> 退出游戏/退出软件后最后一帧永远留在屏上。
 * 这是「画面不消失」的真正根因，不要改回去。 */
#define GST_STATE_VOID_PENDING_ 0
#define GST_STATE_NULL_  1
#define GST_STATE_READY_ 2
#define GST_STATE_PAUSED_ 3
#define GST_STATE_PLAYING_ 4

#define GST_APP_STREAM_TYPE_STREAM 0

extern void gst_init(int *argc, char **argv[]);
extern GstElement *gst_parse_launch(const char *pipeline_description, GError **error);
extern GstElement *gst_bin_get_by_name(GstBin *bin, const char *name);
extern int gst_element_set_state(GstElement *element, GstState state);
/* 等异步状态切换真正落地；不等待就 unref，KMS 平面可能来不及解绑 */
extern int gst_element_get_state(GstElement *element, GstState *state,
                                 GstState *pending, GstClockTime timeout);
extern void gst_object_unref(void *object);
/* 进程内开 GST_DEBUG：设备 shell 里跑的 gst-launch 和 miniapp 进程内的
 * GStreamer 环境不同（元素可用性/插件路径/时钟都可能差），只能在进程内取证。
 * 参数形如 "appsrc:5,opusdec:5,alsa*:5"；空串 = 关闭。 */
extern void gst_debug_set_threshold_from_string(const char *str, int reset);
extern void gst_debug_add_log_function(void *func, void *user_data, void *notify);
extern const char *gst_debug_message_get(void *message);
extern GstCaps *gst_caps_from_string(const char *string);
extern void gst_caps_unref(GstCaps *caps);
extern int gst_util_set_object_arg(void *object, const char *name, const char *value);
extern GstBuffer *gst_buffer_new_allocate(void *allocator, size_t size, void *params);
extern size_t gst_buffer_fill(GstBuffer *buffer, size_t offset, const void *src, size_t size);
extern void gst_buffer_unref(GstBuffer *buf);
/* ---- buffer 标志位（bug3/bug4：给非关键帧打 DELTA_UNIT）----
 * 为什么用函数而不是 GST_BUFFER_FLAGS()/GST_BUFFER_PTS() 宏：那两个宏是
 * gstbuffer.h 里的**结构体字段**宏，本进程没有 GStreamer 头文件（本文件全部
 * 手写声明，见文件头注释），宏无法展开。而 `gst_buffer_get_pts/set_pts` 在
 * 真机 libgstreamer-1.0.so.0 (1.22.0) 与本地 stub 里**都没有导出符号**
 * （逐个 parse .dynsym 核实过），一经调用就是 dlopen 期未解析符号、整个 .so
 * 加载失败。所以时间戳（PTS/DTS）这一半**不做**，只做可靠的标志位这一半，
 * 详见 docs/BUGFIX-bug3-bug4-gst.md「hunk 2 的降级」。 */
extern unsigned int gst_buffer_get_flags(GstBuffer *buf);
extern int gst_buffer_set_flags(GstBuffer *buf, unsigned int flags);
/* ⚠ GST_BUFFER_FLAG_DELTA_UNIT 的**真实数值**（别照抄 (1u<<4)，那是 LIVE）：
 *
 * GStreamer 1.22.0 的 gstbuffer.h 原文：
 *     GST_BUFFER_FLAG_LIVE       = (GST_MINI_OBJECT_FLAG_LAST << 0)
 *     GST_BUFFER_FLAG_DECODE_ONLY= (GST_MINI_OBJECT_FLAG_LAST << 1)
 *     GST_BUFFER_FLAG_DISCONT    = (GST_MINI_OBJECT_FLAG_LAST << 2)
 *     ... (RESYNC<<3, CORRUPTED<<4, MARKER<<5, HEADER<<6, GAP<<7,
 *          DROPPABLE<<8)
 *     GST_BUFFER_FLAG_DELTA_UNIT = (GST_MINI_OBJECT_FLAG_LAST << 9)   <-- 这个
 * 而 GST_MINI_OBJECT_FLAG_LAST = (1 << 4) = 16，所以
 *     DELTA_UNIT = 16 << 9 = 8192 = (1u << 13)
 *     LIVE       = 16 << 0 =   16 = (1u << 4)     <-- 写错一位就变成这个
 * 曾有交接文档误写 (1u<<4)，那会把每个 P 帧标成 LIVE（"PAUSED 时应丢弃的
 * 直播数据"），语义完全相反：画面在 PAUSED 下被丢、QoS 行为也会变。 */
#define GST_BUFFER_FLAG_DELTA_UNIT (1u << 13)

/* ---- flush 事件（bug3/bug4：SPS/分辨率变化时逼下游重新协商）----
 * 真机与 stub 均确认导出：gst_event_new_flush_start / _flush_stop /
 * gst_event_unref / gst_pad_push_event / gst_element_get_static_pad。
 * 只用 gst_pad_push_event 发事件，**不用 gst_element_send_event**：
 * 后者对 event 的所有权在文档里是 transfer-none（调用方要自己 unref），
 * 但元素实现里又可能把 event 转交 sink pad 消费，语义容易踩双 free 或泄漏；
 * gst_pad_push_event 明确是 transfer-full（吃掉引用），边界干净。 */
typedef struct _GstEvent GstEvent;
extern GstEvent *gst_event_new_flush_start(void);
extern GstEvent *gst_event_new_flush_stop(int reset_time);
extern void gst_event_unref(GstEvent *event);
extern int gst_pad_push_event(GstPad *pad, GstEvent *event);
extern GstPad *gst_element_get_static_pad(GstElement *element, const char *name);

/* ---- pad probe（只用来数「真正流到 sink 的帧」）----
 * 设备上没有开发头文件，这里按 GStreamer 1.x 的 ABI 手写声明：
 *   GstPadProbeReturn 是 enum（int），GstPadProbeType 是 guint，
 *   GST_PAD_PROBE_TYPE_BUFFER = 1<<4，GST_PAD_PROBE_OK 在枚举里是 1。
 * 设备符号表已确认 `gst_pad_add_probe` 存在（libgstreamer 1.22.0），
 * 返回的 id 我们不用，所以不声明 remove_probe。 */
typedef struct _GstPadProbeInfo GstPadProbeInfo;
typedef int GstPadProbeReturn;
typedef unsigned int GstPadProbeType;
#define GST_PAD_PROBE_TYPE_BUFFER (1u << 4)
#define GST_PAD_PROBE_OK 1
extern unsigned long gst_pad_add_probe(GstPad *pad, GstPadProbeType mask,
                                       GstPadProbeReturn (*cb)(GstPad *, GstPadProbeInfo *, void *),
                                       void *user_data, void (*destroy)(void *));

extern void gst_app_src_set_caps(GstAppSrc *appsrc, GstCaps *caps);
extern GstFlowReturn gst_app_src_push_buffer(GstAppSrc *appsrc, GstBuffer *buffer);
extern void gst_app_src_set_max_bytes(GstAppSrc *appsrc, unsigned long long max);
extern void gst_app_src_set_leaky_type(GstAppSrc *appsrc, int leaky);
extern void gst_app_src_set_stream_type(GstAppSrc *appsrc, int type);
extern GstFlowReturn gst_app_src_end_of_stream(GstAppSrc *appsrc);
extern int gst_app_src_get_type(void);
extern void g_object_set(void *object, const char *first_name, ...);

/* ---- GstBus（用来抓解码/声卡的真实错误）----
 * 之前我们从不碰 bus，导致 opusdec 出错、alsasink 打不开 PCM 这类致命问题
 * 全被静默吞掉：appsrc push 一直返回 OK，而声卡 hw_ptr 永远是 0。
 * 有了它，`cm_box_drain_bus()` 一调就能把 GStreamer 的原文错误打进日志。 */
typedef struct _GstBus GstBus;
typedef struct _GstMessage GstMessage;
typedef struct _GErrorDetail {
    unsigned int domain;
    int          code;
    char        *message;
} GErrorDetail;
/* GstMessageType 位掩码（GStreamer 1.x）：EOS=1 ERROR=2 WARNING=4 INFO=8 */
#define GST_MSG_EOS      1
#define GST_MSG_ERROR    2
#define GST_MSG_WARNING  4
extern GstBus *gst_element_get_bus(GstElement *element);
extern GstMessage *gst_bus_timed_pop_filtered(GstBus *bus, GstClockTime timeout,
                                              int types);
extern void gst_message_parse_error(GstMessage *msg, GErrorDetail **err,
                                    char **debug);
extern void gst_message_parse_warning(GstMessage *msg, GErrorDetail **err,
                                      char **debug);
extern void gst_message_unref(GstMessage *msg);


/* g_object_set 的 gobject 符号在 libgobject；gst_util_set_object_arg 在 libgstreamer */

/* ==================== 盒子 ==================== */

struct cm_box {
    int is_video;
    GstElement *pipeline;
    GstAppSrc *src;
    GstElement *sink; /* kmssink，改 rect 用 */
    int width, height;
    int rect_x, rect_y, rect_w, rect_h;
    /* 面板换算参数，建盒时从 cfqg 拷进来（见 cm_session.h 的字段注释）。
     * 存一份而不是每次 apply_rect 时读 s->cfg，是因为 apply_rect 可能被
     * cm_box_set_rect 在会话已经改过 cfg 之后调用，拷贝快照语义更清楚。 */
    int panel_x_offset;
    int panel_h;
    int panel_w;
    uint64_t pushed;
    uint64_t pushed_bytes;
    /* ★ 真·上屏帧计数：kmssink 的 sink pad 上挂 BUFFER 探针数出来的。
     *
     * pushed 只说明「我们把 AU 交给了 appsrc」，完全不代表解码器出帧 ——
     * 终末地（mrfzzmd）真机实测过：AU push 每秒 15~20 帧、AULD 每帧指纹都
     * 不同、appsrc push failed 为 0，但 kmssink 一条新 buffer 都没收到，
     * 用户看到全黑或冻结。原因是 appsrc 是 leaky（见 cm_box_create 末尾的
     * set_max_bytes/set_leaky_type），队列塞满时它默默丢掉旧 AU，于是
     * pushed 照样在涨，把「下游已经停摆」这件事完全掩盖掉。
     *
     * 所以「画面到底有没有在动」必须以这个计数为准，看门狗在 cm_media.c。
     * 跨线程读写（探针跑在 kmssink 的流任务里），用 volatile 即可 ——
     * 64 位对齐的单调计数器，读者只做「与上次是否相等」的比较，不需要原子性。 */
    volatile uint64_t shown;
    int failed;
    char *caps_str;

    /* ---- 音频专用：外部 aplay 子进程 ----
     *
     * 为什么不用 alsasink：真机实测在 miniapp 进程内 alsasink 必报
     *   `The stream is in the wrong format (gst_audio_base_sink_wait_event)`
     * 而**完全相同的 pipeline 在 adb shell 里跑 gst-launch 却正常**
     * （state:RUNNING、hw_ptr 递增、能出声）。同一套 libgstalsa.so、同一份
     * /etc/asound.conf、同一个 caps，差别只在进程环境（miniapp 导出了
     * ALSA_LIB_ADD_GAIN=1 / PLAYBIN2_PREFERRED_AUDIOSINK=pulsesink 等），
     * 查不到可改的开关，于是改成 PenBili/PenDouyu 已经在用的方案：
     * GStreamer 只解码，PCM 从 fdsink 写进 aplay 的 stdin，由 aplay 独占声卡。
     * 这条路径在 PenDouyu 上长期可用。 */
    pid_t aplay_pid;
    int   aplay_fd;     /* 管道写端，交给 fdsink 的 fd 属性 */
    /* 实际选中的 aplay 输出设备（"" = 不带 -D，走 /etc/asound.conf 默认 speaker）。
     * 真机排查就靠它 + cg_aplay.log 的头几行。 */
    char  audio_dev[128];
    /* 归属会话：销毁音频盒时要按会话上的快照还原全局 mixer。
     * cm_box_destroy 的签名被 cm_media.c / cgmedia.c 三处调用（那些文件不归
     * 本单元改），所以只能把会话指针存进盒子，而不是改签名。 */
    cm_sess *sess;
};

/* gst_init 只做一次 */
static pthread_once_t g_gst_once = PTHREAD_ONCE_INIT;
static int g_gst_ok = 0;

static void gst_boot(void) {
    gst_init(NULL, NULL);
    g_gst_ok = 1;
    CM_LOGI("gst_init done");
}

static int gst_ensure(void) {
    pthread_once(&g_gst_once, gst_boot);
    return g_gst_ok ? 0 : -1;
}

/* 设备上 mp4/h264 解码走 mppvideodec；拿不到就逐级降级 */
/* kmssink 的四个属性缺一不可（PenBili/native/csrc/player.c:183-195 真机实测）：
 *
 *  plane-id=76        —— 不给就走默认选面，会去抢 weston 的 Esmart0-win0(54)。
 *                        抢不干脆的表现是 summary 里 plane ACTIVE 但
 *                        buf[0].addr == 0，屏幕上什么都不显示（我们踩过：
 *                        PLAYING + pushed=289 但整屏是 UI）。76 = Esmart1-win0，
 *                        zpos 高于 UI，硬件叠加。
 *  driver-name=rockchip —— 配合 plane-id 定位设备。
 *  force-aspect-ratio=false —— 否则 kmssink 在矩形内做信箱缩放，画面缩成一条。
 *  can-scale=true     —— 允许 RGA 硬件缩放，1280x720 直接缩到目标矩形。
 *
 * ★ sync=false 是 bug3/bug4「终末地黑屏 / 画面卡住不动」的真正修复，别改回 true ★
 *
 * 症状（用户报的 bug4）：明日方舟：终末地在 App 里建连成功、`first video frame
 * pushed` 有输出、probe 的 rx/fps 都正常，但屏幕上整块视频区是深灰（透出 Play 页
 * 底色 `#1E2831`），或者只显示一帧就永久静止。
 *
 * 定位过程（每一层都被独立验证过，所以结论是收敛的）：
 *   1) 码流没问题 —— 把 App 实际推给 appsrc 的字节 dump 成 au.h264，
 *      本地 ffmpeg 解出 233 帧，后 110 帧是完整清晰的游戏画面（不花不绿）。
 *   2) 硬解没问题 —— `gst-launch-1.0 filesrc ! h264parse ! mppvideodec` 的输出
 *      帧数与软解完全一致（954368 = 233*4096）。
 *   3) plane76 硬件通道没问题 —— `videotestsrc ! kmssink plane-id=76` 全屏彩条
 *      用户肉眼确认能看到。
 *   4) 同一条管线换数据源就好了 ——
 *      `multifilesrc ... loop=true ! h264parse ! mppvideodec rotation=270 !
 *       videoconvert ! kmssink plane-id=76 ... sync=true render-rectangle=<小窗>`
 *      用户肉眼确认「能看到终末地，一开始全绿过一会正常」。
 *   也就是说 **管线、旋转、缩放、plane 绑定全都对**，唯一变量是喂数据的元件。
 *
 * 根因：`sync=true` 让 kmssink 按 **buffer 的 PTS** 决定何时显示。我们的 appsrc 是
 * `format=time do-timestamp=true`，由 appsrc 在 **到达时刻** 给每帧打一个绝对
 * 单调时钟戳。这套时间戳与 kmssink 的 running-time 基准之间一旦错位（首次缓冲、
 * 重连、或片头 I 帧被丢弃后 PTS 出现跳变），kmssink 就会判定后续 buffer「还没到
 * 显示时刻」或「已过期」而整体丢弃 —— 表现正是**平面绑着却一口不更新**
 * （真机实测：`plane[76] crtc=video_port0 fb=130`，20 秒采样 fb 编号一次不变，
 * 而 `AU push` 涨了 77 帧；用户同时确认「有画面但完全静止」）。
 * 改成 `sync=false` 后同样的采样里 fb 在 127/130 之间持续交替，用户确认
 * 「画面在动了，能正常玩」，原神同时回归正常。
 *
 * 代价：kmssink 不再按 PTS 节流，而是「有空就显示最新一帧」。对本机场景（云游戏
 * 实时流、拒绝累积延迟、720p 单路）这是想要的语义；丢帧由 RTP 层的 IDR/NACK 逻辑
 * 负责，不靠 sink 的时钟。若将来要接本地文件播放（需要严格按时间轴出帧），应当
 * 另建一条 `sync=true` 的管线，而不是把这里改回去。
 *
 * ★ restore-crtc=false 是 bug6「整屏黑、左右两栏消失、触摸失效」的修复，别删 ★
 *
 * 症状：play 页被销毁（重复 `miniapp_cli start`、或宿主重载页面）之后，
 * `crtc[68] plane_mask` 从 3 掉到 0 —— 不只我们的视频面（76），连 weston 的 UI 面
 * （54）也被解绑，整屏黑、触摸无响应，且只能重启设备才恢复。
 *
 * 根因：kmssink 的 `restore-crtc` 默认 true，语义是「CRTC 曾被设过新 mode，
 * 就在 NULL 时把 CRTC 恢复成旧 mode」。它在收尾时会 `drmModeSetCrtc(crtc, 0, ...)`
 * 这类「不带 fb 的 CRTC 恢复」，内核对一个 crtc 做 disable/restore 时会把挂在上面的
 * **所有** plane 的 mask 清掉 —— 于是 weston 辛苦挂的 54 也被一起清掉，而 weston
 * 不知道发生了什么、不会主动重挂（除非被唤醒重新 commit）。
 * 我们只申请了 plane 76，没有资格去动 CRTC 的 mode。`restore-crtc=false` 让
 * kmssink 收尾时只 disable 自己那条 plane，不碰 CRTC → weston 的 54 保持绑定
 * （真机验证：拆除后 `plane_mask` 稳定为 1，即只剩 weston）。
 *
 * 注：`gst-inspect-1.0 kmssink` 真机核实存在该属性，Default: true。 */
#define KMS_SINK_PROPS \
    " ! kmssink name=vsink plane-id=76 driver-name=rockchip " \
    "force-aspect-ratio=false can-scale=true sync=false restore-crtc=false"

static const char *video_pipeline_fmt(void) {
    /* rect 不在这里拼；由 apply_rect() 用 gst_util_set_object_arg 单独设 */
    /* 【旋转必须在解码器里做】面板 480x960 竖屏、逻辑画布 960x266 横屏，
     * weston 合成 UI 时替我们转了 90°，但 KMS 视频平面**绕过了 weston**，
     * 所以必须自己转。硬旋转走不通：modetest 实测 Esmart1 的 rotation 属性
     * 只有 rotate-0/reflect-y，**没有 rotate-90/270**，所以交给解码器：
     * mppvideodec 的 rotation 属性会在 VPU 内完成旋转，输出直接是
     * 竖的（1600x900 -> 900x1600），下游 render-rectangle 的 266x473 才是对的。
     * 270 这个值是与本机同面板、同 PANEL_X_OFFSET 的 PenBili 真机 A/B 结论
     * （PenBili/native/csrc/player.c:539-548：rotation=90 会整体倒置，270 正确）。 */
    /* 【bug3/bug4 修复：解码器错误策略】真机 gst-inspect-1.0 mppvideodec 核实：
     *   ignore-error  布尔, Default: true   —— 解码出错时是「只 warn 继续」还是「上报致命错误」
     *   max-errors    整数, Range -1..2^31-1, Default: -1 —— -1 = 不限次数
     * 两个属性都存在且可设。这里显式写死，不依赖插件默认值。
     * 为什么要 ignore-error=false（视频 teammate 的真机结论，见
     * docs/BUGFIX-bug3-bug4-video.md §hunk 1）：默认 true 是「解坏了也不报错、
     * 把坏帧直接吐出去上屏」，现象就是花屏且日志里一条都没有。RTP 层已经拦住
     * 了残帧（bug3/bug4 的第一道防线），这里是**第二道**：让网关侧编码器自身
     * 出错这类我们识别不出的坏帧变成 GST_MSG_ERROR，被 cm_box_drain_bus 抓进
     * 日志，而不是悄悄糊在屏幕上。max-errors=16 是连续错误上限 —— 连续错 16 次
     * 才让解码器吐 flow error，偶发单帧坏不会立刻停摆。
     * ⚠ 真机若看到「画面卡住 + 日志有 mpp 解码错误」，就是这道防线在生效
     *   （16 次连续错误后 flow error）；回退只需去掉这两个属性恢复插件默认值，
     *   详见 docs/BUGFIX-bug3-bug4-gst.md「hunk 1」。 */
    return "appsrc name=vsrc is-live=true format=time do-timestamp=true "
           "caps=video/x-h264,stream-format=byte-stream,alignment=au "
           "! h264parse ! mppvideodec rotation=270 ignore-error=false max-errors=16 "
           "! videoconvert" KMS_SINK_PROPS;
}

static const char *video_pipeline_soft(void) {
    /* 软解兜底分支：avdec_h264 没有 rotation 属性，用 videoflip 做同样的旋转。
     * videoflip 的 method 枚举：1=clockwise(顺90) 3=counterclockwise(逆90)。
     * 与 mppvideodec rotation=270（逆时针 90°）等价 -> method=counterclockwise。
     * 注意显式写 method 而不是 video-direction，后者在本机型 1.22 上部分枚举不支持。 */
    return "appsrc name=vsrc is-live=true format=time do-timestamp=true "
           "caps=video/x-h264,stream-format=byte-stream,alignment=au "
           "! h264parse ! avdec_h264 ! videoflip method=counterclockwise ! videoconvert" KMS_SINK_PROPS;
}

/* ---------------- X6 Pro 音频路由：尊重系统音量 + 完整还原 ----------------
 *
 * 【这条单元的来历，别删注释就改代码】
 * 早期症状：alsasink 正常 PLAYING、`/proc/asound/card1/.../status = RUNNING`，
 * 但扬声器完全无声。当时的结论是 card0（rk817）的 `Playback Path`（numid=45）
 * 开机默认 OFF —— 功放通路没接上，于是每次开播前把 OFF 拨成 SPK，并且
 * 「card1 功放音量如果是 0,0 就抬到 200,200」。X5 上有系统音频守护进程做这件事，
 * X6 Pro 没人做。
 *
 * 【Bug5 真机重测：那个 200,200 是错的方向，必须去掉】
 * 用户反馈「系统音量拖到 0，进游戏还是有声；退出后连有道查词都有声」。真机取证：
 *
 * ① 系统音量 0 落在哪：**card1（aw883xx smart PA）numid=54
 *    `Master Playback Volume` = 0,0**，和 card0 的任何控件都无关。
 *    证据：`hal-audio -s 0|30|50|100` 前后的 `amixer -c 0 contents` +
 *    `amixer -c 1 contents` 全量 diff，**只有 numid=54 变化**
 *    （0,0 / 128,128 / 176,176 / 255,255），card0 一个字节没动；
 *    `/etc/asound.conf` 的 `spk_softvol` 也把 softvol 绑在
 *    `control { name "Master Playback Volume" card 1 device 0 }`。
 *    所以 card1 numid=54 **就是系统音量本体**，读到 0,0 的正确语义是
 *    「用户把音量调到了 0」，而不是「寄存器没初始化」。
 *
 * ② 声学矩阵（`aplay 播 /dev/urandom` + `arecord -D hw:0,0` 同时录音，
 *    数值是 16bit 采样绝对值均值/峰值）：
 *      quiet                      avg=83   max=240
 *      Playback Path=OFF amp=0,0  avg=90   max=553    <- 静音
 *      Playback Path=OFF amp=200  avg=9339 max=32768  <- 大声（旧代码干的事）
 *      Playback Path=SPK amp=0,0  avg=102  max=605    <- 静音
 *      Playback Path=SPK amp=200  avg=9366 max=32768  <- 大声
 *    两条结论：(a) 响度**只由 card1 amp 决定**，OFF/SPK 对是否出声没有区别
 *    —— 因为 aplay 走 /etc/asound.conf 的默认 `speaker`，最终 slave 是
 *    `hw:1,0`，声音根本不经过 rk817 的模拟输出级；
 *    (b) 把 0,0 抬到 200,200 正是用户听到的那声「不该有的声音」。
 *
 * ③ 由此，旧代码同时制造了两条用户可见故障：
 *    起播时把系统音量 0 抬成 200（音量条是 0，却有声）；
 *    退出时既不还原 amp，也不还原 Playback Path（改成 SPK 后再没人改回去），
 *    于是退出游戏后系统音量被永久留在 200/255，有道查词随口一播就出声。
 *
 * 【现行策略】
 *   - card1 numid=54 **一个字节都不写**。它就是系统音量，写它等于替用户调音量。
 *     只读，用于快照和退出时的「我们有没有动过」自证。
 *   - card0 Playback Path 仍按历史语义「仅 OFF 时拨到 SPK」（这条路径在本机
 *     声学上无差别，但保留它不影响任何机型，且它确实是历史无声问题的修复），
 *     但**必须先快照、事后无条件还原**，且只还原「确实是我们写下去的那个值」。
 *   - 快照/还原放在 cm_sess（不是 cm_box）：盒子创建可能失败、销毁可能先于
 *     会话结束，还原必须能在任何一条拆管线路径上幂等地跑一次。
 *
 * 解析必须两步（先建 Item #n -> 名字表，再读 `: values=n` 定位）：
 * 首行 `name='Playback Path'` 也是单引号包的，取「第一对单引号」会拿到控件名本身
 * （PenBili v2.3.2 的教训）；而取「第一个 Item 行」会永远读到 OFF（我第一版踩的坑）。 */
#define AUDIO_AMIXER_PATH "/bin/amixer"
#define AUDIO_AMIXER_CTL  "Playback Path"
#define AUDIO_SPK_VALUE   "SPK"
/* card1 = aw883xx smart PA；numid=54 = Master Playback Volume = 系统音量本体。
 * 只读。永远不要对这里 cset（见上面的声学矩阵）。 */
#define AUDIO_AMP_CARD    "-c 1"
#define AUDIO_AMP_CTL     "numid=54"

/* 读当前路由枚举名（"OFF"/"SPK"/"HP"/"BT"...）。0 = 读到了。
 *
 * ★ 必须两步解析，不能取「第一个 Item # 行的枚举名」：
 *   amixer -c 0 cget name='Playback Path' 的真实输出是
 *     numid=45,...,name='Playback Path'
 *       ; type=ENUMERATED,access=rw------,values=1,items=11
 *       ; Item #0 'OFF'        <- 这是「可选项 0」，不是当前值
 *       ; Item #1 'RCV'
 *       ...
 *       ; Item #10 'RING_SPK_HP'
 *       : values=2             <- 这一行才指出当前选中 Item #2
 *   PenBili v2.3.2 踩过同一类坑（当时是取了控件名）。我第一版又踩了一次：
 *   取第一个 Item 行 → 永远读到 "OFF" → 回读校验恒为 OFF，误导排查。
 *   正确做法：先收 Item #n '名字' 映射表，再读 `: values=n` 取对应名字。 */
static int audio_mixer_get_path(char *out, int n) {
    char cmd[192];
    FILE *fp;
    int rc = -1;
    char items[24][32];
    int nitems = 0;
    int cur = -1;
    int i;
    if (!out || n <= 0) return -1;
    out[0] = 0;
    memset(items, 0, sizeof(items));
    snprintf(cmd, sizeof(cmd), "%s -c 0 cget name='%s' 2>/dev/null",
             AUDIO_AMIXER_PATH, AUDIO_AMIXER_CTL);
    fp = popen(cmd, "r");
    if (!fp) return -1;
    {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char *item = strstr(line, "Item #");
            if (item) {
                int idx = atoi(item + 6);
                char *q1 = strchr(item, 0x27);
                char *q2 = q1 ? strchr(q1 + 1, 0x27) : NULL;
                if (q1 && q2 && q2 > q1 + 1 && idx >= 0 && idx < 24) {
                    int ln = (int)(q2 - q1 - 1);
                    if (ln > 31) ln = 31;
                    memcpy(items[idx], q1 + 1, (size_t)ln);
                    items[idx][ln] = 0;
                    if (idx + 1 > nitems) nitems = idx + 1;
                }
                continue;
            }
            {
                char *v = strstr(line, ": values=");
                if (v) { cur = atoi(v + 9); break; }
            }
        }
    }
    pclose(fp);
    if (cur >= 0 && cur < nitems && items[cur][0]) {
        strncpy(out, items[cur], (size_t)(n - 1));
        out[n - 1] = 0;
        rc = 0;
    }
    return rc;
}

static int audio_mixer_set_path(const char *value) {
    char cmd[256];
    if (!value || !value[0]) return -1;
    snprintf(cmd, sizeof(cmd), "%s -c 0 cset name='%s' '%s' >/dev/null 2>&1",
             AUDIO_AMIXER_PATH, AUDIO_AMIXER_CTL, value);
    return (system(cmd) == 0) ? 0 : -1;
}

/* 读 card1 功放（= 系统音量本体）当前值，规范成 "0,0" / "176,176" 这种原文。
 * ★ 只读。这是**系统音量**，不是我们的音量：写它等于替用户拖音量条
 *   （Bug5 的根因就是把 0,0 写成 200,200）。
 * 解析：`amixer -c 1 cget numid=54` 输出里有两行带 values ——
 *   `  ; type=INTEGER,access=rw------,values=2,min=0,max=255,step=1`
 *   `  : values=0,0`
 * 只有后者是当前值。区别在行首字符（';' vs ':'），所以必须匹配 ": values="，
 * 匹配 "values=" 会把 `values=2`（声道数）当成音量读成 2（本轮实测踩过）。 */
static int audio_amp_read(char *out, int n) {
    FILE *fp;
    int rc = -1;
    if (!out || n <= 0) return -1;
    out[0] = 0;
    fp = popen(AUDIO_AMIXER_PATH " " AUDIO_AMP_CARD " cget " AUDIO_AMP_CTL
               " 2>/dev/null", "r");
    if (!fp) return -1;
    {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char *v = strstr(line, ": values=");
            if (v) {
                int i = 0, ln;
                v += 9;
                ln = (int)strlen(v);
                while (ln > 0 && (v[ln - 1] == '\n' || v[ln - 1] == '\r' ||
                                  v[ln - 1] == ' '))
                    ln--;
                if (ln > n - 1) ln = n - 1;
                for (i = 0; i < ln; i++) out[i] = v[i];
                out[ln] = 0;
                rc = ln > 0 ? 0 : -1;
                break;
            }
        }
    }
    pclose(fp);   /* SIGCHLD=SIG_IGN 下返回值不可信，但子进程确实执行了 */
    return rc;
}

/* 开播前快照我们**将要动**的全局 mixer 状态。只做一次，幂等。
 * 快照放在 cm_sess 而不是 cm_box：盒子可能创建失败、也可能先于会话结束被销毁，
 * 而还原必须在任何一条拆管线路径上都能跑（见 cm_audio_route_restore）。 */
static void audio_snapshot(cm_sess *s) {
    char cur[64];
    if (!s || s->audio_snap_done) return;
    s->audio_snap_done = 1;
    s->audio_path_wrote[0] = 0;
    s->audio_path_snap[0] = 0;
    s->audio_amp_snap[0] = 0;
    s->audio_amp_after[0] = 0;
    if (audio_mixer_get_path(cur, sizeof(cur)) == 0) {
        strncpy(s->audio_path_snap, cur, sizeof(s->audio_path_snap) - 1);
        s->audio_path_snap[sizeof(s->audio_path_snap) - 1] = 0;
    }
    if (audio_amp_read(s->audio_amp_snap, (int)sizeof(s->audio_amp_snap)) != 0)
        s->audio_amp_snap[0] = 0;
    CM_LOGI("#%d audio snapshot: Playback Path=%s amp(card1#54)=%s",
            s->id,
            s->audio_path_snap[0] ? s->audio_path_snap : "?",
            s->audio_amp_snap[0] ? s->audio_amp_snap : "?");
}

/* 把我们在 audio_route_ensure 里改过的全局 mixer 状态还原。**幂等**。
 *
 * 三条路径都会走到这里（cm_media 看门狗退出 / cgmedia sess_free / cgmedia
 * js_stop / cm_cmd setAudio 0），实际挂在 cm_box_destroy 的音频分支上 ——
 * 上述四条路径最终都调它，所以只需要一处实现。
 *
 * 还原规则（只还原「确实是我们改的」）：
 *   1. Playback Path：只有 audio_path_wrote 非空（我们真的写过）才还原；
 *      并且**只在当前值仍等于我们写下去的那个值**时才写回 ——
 *      用户在我们运行期间插了耳机 / 连了蓝牙，属于他的选择，不许覆盖。
 *   2. card1 numid=54：我们从头到尾没写过，只回读一次打进日志当自证
 *      （退出后它应该和快照一模一样；不一样说明是系统音量服务改的）。
 */
void cm_audio_route_restore(cm_sess *s) {
    char cur[64];
    char after[64];
    if (!s || !s->audio_snap_done) return;   /* 没快照过 = 没改过 = 无事可做 */
    s->audio_snap_done = 0;                  /* 先置位，保证重入时直接返回 */

    if (s->audio_path_wrote[0]) {
        if (audio_mixer_get_path(cur, sizeof(cur)) == 0 &&
            strcmp(cur, s->audio_path_wrote) == 0) {
            (void)audio_mixer_set_path(s->audio_path_snap);
            if (audio_mixer_get_path(after, sizeof(after)) == 0)
                CM_LOGI("#%d audio restore: Playback Path %s -> %s (back to %s)",
                        s->id, s->audio_path_wrote, after, s->audio_path_snap);
            else
                CM_LOGI("#%d audio restore: Playback Path %s -> %s (readback failed)",
                        s->id, s->audio_path_wrote, s->audio_path_snap);
        } else {
            /* 当前值已经不是我们写的那个：用户自己改过路由，尊重他。 */
            CM_LOGI("#%d audio restore: Playback Path now %s (not ours %s), leave it",
                    s->id, cur[0] ? cur : "?", s->audio_path_wrote);
        }
        s->audio_path_wrote[0] = 0;
    }

    /* 自证：把退出时的系统音量打进日志，和开播前快照对照。 */
    if (s->audio_amp_snap[0] &&
        audio_amp_read(s->audio_amp_after, (int)sizeof(s->audio_amp_after)) == 0) {
        if (strcmp(s->audio_amp_snap, s->audio_amp_after) == 0)
            CM_LOGI("#%d audio restore: amp(card1#54) %s unchanged (we never write it)",
                    s->id, s->audio_amp_after);
        else
            CM_LOGI("#%d audio restore: amp(card1#54) %s -> %s (changed by system, not us)",
                    s->id, s->audio_amp_snap, s->audio_amp_after);
    }
}

/* 建音频盒前调用：快照 →（仅当 Playback Path 为 OFF 时）拨到 SPK。
 * ★ 绝不再碰 card1 音量。失败全部静音处理（别的机型没有该控件）。 */
static void audio_route_ensure(cm_sess *s) {
    char cur[64];
    char after[64];
    audio_snapshot(s);   /* 无论后面改不改，先把「改之前」记下来 */
    if (audio_mixer_get_path(cur, sizeof(cur)) != 0) {
        /* 读不到就静默返回（别的机型没有该控件，或 amixer 不在）。
         * 注意：宿主进程把 SIGCHLD 设成 SIG_IGN，popen/pclose 会返回 -1，
         * 但子进程其实照常执行了 —— 所以不能拿返回值当「命令没成功」用。 */
        return;
    }
    if (strcmp(cur, "OFF") != 0) {
        CM_LOGI("Playback Path already %s, leave it", cur);
        return;                            /* 已路由（HP/BT/SPK）-> 尊重现状 */
    }
    (void)audio_mixer_set_path(AUDIO_SPK_VALUE);   /* 返回值不可信，见上 */
    /* 回读确认：这是唯一可信的成功判据（和 imgConv 用产物文件自证同理）。
     * 只有回读确实变成 SPK 才记 audio_path_wrote —— 还原时据此判断「是我们改的」。 */
    if (audio_mixer_get_path(after, sizeof(after)) == 0) {
        CM_LOGI("Playback Path OFF -> %s (now=%s)", AUDIO_SPK_VALUE, after);
        if (strcmp(after, AUDIO_SPK_VALUE) == 0 && s) {
            strncpy(s->audio_path_wrote, after, sizeof(s->audio_path_wrote) - 1);
            s->audio_path_wrote[sizeof(s->audio_path_wrote) - 1] = 0;
        }
    } else {
        CM_LOGI("Playback Path OFF -> %s (readback failed)", AUDIO_SPK_VALUE);
    }
}

static const char *audio_pipeline_desc(void) {
    /* 三个坑，全部由 bus 上的原文错误确认过（`cm_box_drain_bus`）：
     *
     * ① appsrc caps 只能写 `audio/x-opus`，**不能带 rate/channels**。
     *    本机 gst-inspect-1.0 opusdec 的 sink 模板是
     *      audio/x-opus, channel-mapping-family: 0
     *      audio/x-opus, channel-mapping-family: [1,255], channels, stream-count, coupled-count
     *    —— 根本没有 rate 字段。写 `rate=48000` 会导致 caps 相交为空，
     *    bus 报 `Internal data stream error (gst_base_src_loop / GstAppSrc:asrc)`
     *    并且 appsrc push 依然返回 OK，于是「有包但没声」完全静默。
     * ② 解码后必须显式钉死 raw 格式再进 alsasink，否则 bus 报
     *    `The stream is in the wrong format (gst_audio_base_sink_wait_event)`。
     *    opusdec 输出是 F32LE，而 /etc/asound.conf 的 speaker 链路要 S16LE/48k/2ch。
     * ③ appsrc 是 live source、alsasink 是 sync sink，中间必须有 queue，
     *    否则 bus 报 `Pipeline construction is invalid, please add queues.`
     *    （视频那条也一样，是 gst_base_sink_query_latency 的告警）。 */
    return "appsrc name=asrc is-live=true format=time do-timestamp=true "
           "caps=audio/x-opus ! queue ! opusdec ! audioconvert ! audioresample "
           "! audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2 "
           "! alsasink name=asink sync=true";
}

/* GStreamer 的 GST_DEBUG 默认写 stderr，而小程序进程的 stderr 不会被
 * /userdata/applog 捕获（真机实测：打开 gstdebug.txt 后日志里一条 GST_ 行都没有）。
 * 所以挂一个自己的 log handler，把 GStreamer 的调试输出转进 CM_LOGW，
 * 才能落进设备日志。只看 WARNING 及以上，避免刷爆。 */
static void cm_gst_log_handler(void *category, int level, const char *file,
                               int line, const char *func, void *object,
                               void *msg, void *user_data) {
    const char *text;
    char cat[32];
    int i;
    (void)object; (void)user_data; (void)line; (void)func;
    /* GstDebugLevel: 0=NONE 1=ERROR 2=WARNING 3=FIXME 4=INFO 5=DEBUG 6=LOG。
     * 这里不按等级过滤 —— 阈值已经由 gstdebug.txt 里的字符串控制，
     * 再过滤一次会把 alsasink 协商细节（DEBUG 级）全吃掉。改为按条数限流。 */
    static int budget = 120;
    if (budget <= 0) return;
    budget--;
    text = gst_debug_message_get ? gst_debug_message_get(msg) : NULL;
    if (!text) return;
    cat[0] = 0;
    if (category) {
        for (i = 0; i < 31 && ((const char *)category)[i]; i++)
            cat[i] = ((const char *)category)[i];
        cat[i] = 0;
    }
    CM_LOGW("GST[%s] %s: %s", cat, file ? file : "?", text);
}

static void cm_gst_enable_debug(void) {
    /* ★ 防爆盘：gstdebug.txt 开关（内容如 kmssink:7）会让 kmssink 的每帧
     * TRACE/LOG 行全部落进 DictPen_*.log —— 真机实测一局把日志灌到 158MB
     * （曾把 /userdata 的 56M 可用空间写满）。所以开关文件必须在 30 分钟后
     * 自动失效：取证时先 touch gstdebug.txt 再起播，30 分钟后即使忘删也只会
     * 灌半小时，不会刷一整夜。 */
    static int dbg_once = 0;
    if (dbg_once) return;
    dbg_once = 1;
    {
        FILE *df = fopen("/userdisk/miniapp/data/gstdebug.txt", "r");
        if (df) {
            char lv[256];
            int fresh = 0;
            /* freshness check：文件 mtime 距今超过 30 分钟就当它不存在 */
            struct stat st;
            if (fstat(fileno(df), &st) == 0) {
                long age_s = (long)(time(NULL) - st.st_mtime);
                fresh = (age_s >= 0 && age_s <= 1800);
                if (!fresh)
                    CM_LOGW("gstdebug.txt is %ld min old, ignore (touch it to enable GST_DEBUG)", age_s / 60);
            } else {
                fresh = 1; /* 拿不到 mtime 就保持旧行为 */
            }
            if (fresh && fgets(lv, sizeof(lv), df)) {
                size_t n = strlen(lv);
                while (n && (lv[n - 1] == '\n' || lv[n - 1] == '\r')) lv[--n] = 0;
                if (n) {
                    gst_debug_set_threshold_from_string(lv, 1);
                    gst_debug_add_log_function((void *)cm_gst_log_handler, NULL, NULL);
                    CM_LOGW("GST_DEBUG enabled: %s (auto-expires 30min after file mtime)", lv);
                }
            }
            fclose(df);
        }
    }
}

/* ==================== 音频：外部 aplay 播放 ==================== */

/* ---------------- 蓝牙音频输出设备探测（照抄 PenBili player.c v2.3.1 语义） ----------------
 * 本机音频走 ALSA：内置扬声器是默认 PCM，蓝牙耳机则必须显式给 aplay 一个
 * bluealsa PCM。`bluealsa-aplay -L` 的输出（真机实测）形如：
 *
 *     bluealsa:SRV=org.bluealsa,DEV=<BT_MAC>,PROFILE=a2dp
 *         Redmi Buds 6, trusted audio-card, playback
 *
 * DEV 前面还有 SRV=...，所以必须匹配 "bluealsa:" 而**不是** "bluealsa:DEV="，
 * 否则永远匹配不到、声音一直留在内置扬声器上（PenBili 已踩过这坑）。
 *
 * ★ X6 Pro 真机：/usr/bin/bluealsa-aplay -L **直接崩溃**——
 *       $ /usr/bin/bluealsa-aplay -L
 *       *** stack smashing detected ***: terminated
 *       Aborted
 *   （v3.0.0, 43344 B, Mar 18 2024；耳机已 Connected 时同样崩。）
 *   abort 前 stdout 未 flush ⇒ popen 永远读到空 ⇒ 蓝牙永远选不中。
 *   故必须加 BlueZ 回退：bluetoothctl devices → 逐台 info 判 Connected / Audio Sink。
 *   这就是「连了蓝牙耳机声音还是外放」的真正根因链。 */
#define BTCTL_PATH      "/usr/bin/bluetoothctl"
#define BT_LIST_CMD     "/usr/bin/bluealsa-aplay -L 2>/dev/null"
#define BT_MAX_DEVS     8
#define BT_MAX_PROBE    4   /* 最多对几台设备做 bluetoothctl info */

/* 强制设备选择（供真机 A/B）。内容：
 *     speaker -> 强制内置扬声器（完全跳过探测）
 *     bt      -> 强制蓝牙（探不到时按 auto 继续，不会变成没声音）
 *     其它 / 文件不存在 -> auto（先蓝牙，后扬声器）
 * 每次建音频盒读一次（不是每包、每秒读），改完标记文件重开一局即生效。 */
#define AUDIO_DEV_FORCE_FILE "/userdisk/miniapp/data/audiodev.txt"
/* 视频软解强制开关（纯诊断，默认不存在 = 正常走 mppvideodec）。
 *
 * 存在的意义：终末地「平面绑上 + 我们稳定推 AU + 解码器输出过一帧」却全黑时，
 * 必须把「我们喂的码流坏」和「mppvideodec 对这条流的处理不对」切开。
 * touch 这个文件 → 走 avdec_h264 软解：
 *   软解有画面 ⇒ 码流是好的，锅在硬解这一层；
 *   软解也全黑 ⇒ 锅在我们的 AU 组装 / SPS-PPS 注入（见 cm_rtp.c 的 au_dump_hook，
 *                那里能 dump 出实际喂给解码器的字节，用设备自带 ffmpeg 验证）。
 * 不改管线字符串：两条管线字符串都是现成的，这里只是选哪一条。 */
#define VIDEO_SOFTDEC_SWITCH  "/userdisk/miniapp/data/softdec.txt"
#define AUDIO_FORCE_AUTO     0
#define AUDIO_FORCE_SPEAKER  1
#define AUDIO_FORCE_BT       2

static int audio_force_mode(void) {
    FILE *f;
    char line[64];
    int mode = AUDIO_FORCE_AUTO;
    f = fopen(AUDIO_DEV_FORCE_FILE, "r");
    if (!f) return AUDIO_FORCE_AUTO;
    if (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        if (strcmp(line, "speaker") == 0) mode = AUDIO_FORCE_SPEAKER;
        else if (strcmp(line, "bt") == 0) mode = AUDIO_FORCE_BT;
        else if (line[0]) CM_LOGW("audiodev.txt unknown value '%s', using auto", line);
    }
    fclose(f);
    CM_LOGI("audio force mode = %d (%s)", mode,
            mode == AUDIO_FORCE_SPEAKER ? "speaker" :
            mode == AUDIO_FORCE_BT ? "bt" : "auto");
    return mode;
}

/* 单台设备是否「已连接 + 有 Audio Sink」→ 1/0。一次 popen 一次 info。
 * 只有 Connected: yes 但没有 Audio Sink 的是 AVRCP-only 设备（遥控角色），
 * 能连上但放不出声，必须排除，否则会出现「选了它但静默无声」。 */
static int bt_dev_is_connected_sink(const char *mac) {
    char cmd[192];
    char line[256];
    FILE *fp;
    int connected = 0;
    int sink = 0;
    if (!mac || !mac[0]) return 0;
    snprintf(cmd, sizeof(cmd), "%s info '%s' 2>/dev/null", BTCTL_PATH, mac);
    fp = popen(cmd, "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Connected: yes")) connected = 1;
        /* A2DP Sink = 0000110b（耳机作为我们推流的接收端）。
         * 用独立 if 而不是 else-if：某些固件把两段信息打在同一行附近，
         * 用 else-if 会漏掉同一行上的 Audio Sink。 */
        if (strstr(line, "Audio Sink")) sink = 1;
    }
    pclose(fp);
    if (connected && !sink)
        CM_LOGI("bt: %s connected but no Audio Sink (AVRCP only), skip", mac);
    return (connected && sink) ? 1 : 0;
}

/* 回退路径：扫已配对设备，挑第一台已连接的 A2DP Sink。0 = 找到。 */
static int bt_detect_pcm_bluez(char *out, int n) {
    char macs[BT_MAX_DEVS][24];
    int n_mac = 0;
    int i;
    char cmd[192];
    char line[256];
    FILE *fp;

    if (!out || n <= 0) return -1;
    snprintf(cmd, sizeof(cmd), "%s devices 2>/dev/null", BTCTL_PATH);
    fp = popen(cmd, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp) && n_mac < BT_MAX_DEVS) {
        /* "Device B0:A3:F2:**:**:** <耳机名>" */
        char mac[24];
        if (strncmp(line, "Device ", 7) != 0) continue;
        if (sscanf(line + 7, "%23s", mac) != 1) continue;
        if (strlen(mac) != 17 || mac[2] != ':' || mac[5] != ':') continue;
        snprintf(macs[n_mac], sizeof(macs[n_mac]), "%s", mac);
        n_mac++;
    }
    pclose(fp); /* 先关掉外层 pipe 再逐台 info，避免管道缠绕 */

    /* 每个 info 都是一次 D-Bus 往返（几十~几百 ms），而这条路径在收包线程
     * 建音频盒时同步执行，会推迟第一帧。已配对设备很多时只探前 BT_MAX_PROBE 台
     * （足够：真正在用的耳机通常排在 bluetoothctl devices 的最新/靠前位置）。 */
    if (n_mac > BT_MAX_PROBE) {
        CM_LOGW("bt: %d paired devices, only probing first %d", n_mac, BT_MAX_PROBE);
        n_mac = BT_MAX_PROBE;
    }
    for (i = 0; i < n_mac; i++) {
        if (!bt_dev_is_connected_sink(macs[i])) continue;
        snprintf(out, (size_t)n, "bluealsa:DEV=%s,PROFILE=a2dp", macs[i]);
        return 0;
    }
    return -1;
}

/* 自动挑选音频输出设备。返回 0 = 找到了 bluealsa PCM（写进 out）；
 * -1 = 没有蓝牙设备，调用方应不带 -D（走 /etc/asound.conf 的默认 speaker）。
 *
 * ★ 必须在 fork 之前（父进程里）调用：popen 在 fork 出来的子进程里用会复制
 *   父进程的 stdio 锁，且子进程的 stdout 已被重定向到 aplay 日志，行为不可控。 */
static int bt_detect_pcm(char *out, int n) {
    FILE *fp;
    char line[256];
    int forced;
    uint64_t t0;

    if (!out || n <= 0) return -1;
    out[0] = '\0';
    t0 = cm_now_ms();

    forced = audio_force_mode();
    if (forced == AUDIO_FORCE_SPEAKER) {
        CM_LOGI("bt: none (use speaker) [forced by audiodev.txt]");
        return -1;
    }

    /* ① 首选 bluealsa-aplay -L（真机上多半崩溃/空输出，但成本极低） */
    fp = popen(BT_LIST_CMD, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char *p = strstr(line, "bluealsa:");
            if (p) {
                char *end = p;
                while (*end && *end != ' ' && *end != '\n' && *end != '\r' && *end != '\t')
                    end++;
                if (end > p && (int)(end - p) < n) {
                    memcpy(out, p, (size_t)(end - p));
                    out[end - p] = '\0';
                    pclose(fp);
                    CM_LOGI("bt: bluealsa-aplay -L ok -> %s (%llu ms)",
                            out, (unsigned long long)(cm_now_ms() - t0));
                    return 0;
                }
            }
        }
        pclose(fp);
    }
    CM_LOGI("bt: bluealsa-aplay -L gave nothing (known to crash on X6 Pro), falling back");

    /* ② BlueZ 回退 */
    if (bt_detect_pcm_bluez(out, n) == 0) {
        CM_LOGI("bt: bluetoothctl fallback ok -> %s (%llu ms)",
                out, (unsigned long long)(cm_now_ms() - t0));
        return 0;
    }
    out[0] = '\0';
    if (forced == AUDIO_FORCE_BT)
        CM_LOGW("bt: forced but no connected A2DP sink found, using speaker");
    CM_LOGI("bt: none (use speaker) (%llu ms)",
            (unsigned long long)(cm_now_ms() - t0));
    return -1;
}

#define APLAY_PATH "/bin/aplay"
#define APLAY_LOG  "/userdisk/miniapp/data/cg_aplay.log"

/* aplay 的标准参数：裸 PCM、S16LE、48k、双声道、从 stdin 读。
 * 输出重定向到日志文件，方便回读 ALSA 的 underrun/格式错误。
 *
 * 输出设备：先 bt_detect_pcm() 找蓝牙（bluealsa PCM），找到就带 `-D <pcm>`；
 * 找不到就不带 -D，走 /etc/asound.conf 的默认 `speaker`（内置扬声器）。
 * ★ 探测必须在 fork **之前**在父进程做完（见 bt_detect_pcm 的注释），
 *   结果放在 b->audio_dev 里让子进程继承。
 * ★ 采样率固定 48000，和上面音频管线的 `rate=48000` caps 必须一致；
 *   蓝牙走 bluealsa 时同样接受 48k A2DP，不要引入 44100 分支（会和要求
 *   S16LE/48k/2ch 的 caps 打架，必须先改 resample caps）。 */
static int aplay_spawn(cm_box *b, int rate, int channels) {
    int pp[2];
    pid_t pid;
    static int sigpipe_armed = 0;
    if (!b) return -1;

    /* ★ 忽略 SIGPIPE（只需一次，进程级）。
     * 走蓝牙时 `-D bluealsa:...` 有可能打不开（设备没连、PCM 名不对、
     * bluealsa 服务未起），aplay 会立刻退出。之后 GStreamer 的 fdsink
     * 继续往这条已经断掉的管道写 → 默认行为是 SIGPIPE **直接杀掉本进程**
     * （整个 miniapp 崩掉，比"没声音"严重得多）。忽略掉之后写失败只返回
     * EPIPE，fdsink 报一个 pipeline error，会话仍可控。 */
    if (!sigpipe_armed) {
        signal(SIGPIPE, SIG_IGN);
        sigpipe_armed = 1;
        CM_LOGI("SIGPIPE ignored (protects host from aplay pipe death)");
    }

    /* 设备探测（父进程，popen 安全处） */
    b->audio_dev[0] = '\0';
    if (bt_detect_pcm(b->audio_dev, (int)sizeof(b->audio_dev)) != 0)
        b->audio_dev[0] = '\0';   /* 显式清空：走默认 speaker */
    if (b->audio_dev[0]) {
        CM_LOGI("aplay dev=%s", b->audio_dev);
        CM_LOGI("AUDIO ROUTE: BLUETOOTH (%s)", b->audio_dev);
    } else {
        CM_LOGI("aplay dev=default(speaker)");
        CM_LOGI("AUDIO ROUTE: SPEAKER (default)");
    }

    if (pipe(pp) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(pp[0]); close(pp[1]); return -1; }
    if (pid == 0) {
        char rb[16], cb[16];
        /* 32 项足够：基础 11 项 + `-D <dev>` 2 项 + `-B/-F` 4 项 + NULL */
        char *argv[32];
        int i = 0;
        int lg;
        setpgid(0, 0);
        if (dup2(pp[0], 0) < 0) _exit(126);
        close(pp[0]);
        close(pp[1]);
        lg = open(APLAY_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); close(lg); }
        snprintf(rb, sizeof(rb), "%d", rate > 0 ? rate : 48000);
        snprintf(cb, sizeof(cb), "%d", channels > 0 ? channels : 2);
        argv[i++] = "aplay";
        /* 扬声器路径用 `-q`（安静）；蓝牙路径改成 `-v`，把 ALSA setup dump
         * 写进 cg_aplay.log —— 里面有 BUFFER_BYTES / PERIOD_BYTES 实测值，
         * 下次连耳机时能直接算出真实缓冲时长，不用再猜。 */
        argv[i++] = b->audio_dev[0] ? "-v" : "-q";
        /* -D 必须在文件名 `-` 之前 */
        if (b->audio_dev[0]) {
            argv[i++] = "-D";
            argv[i++] = b->audio_dev;
        }
        argv[i++] = "-t"; argv[i++] = "raw";
        argv[i++] = "-f"; argv[i++] = "S16_LE";
        argv[i++] = "-r"; argv[i++] = rb;
        argv[i++] = "-c"; argv[i++] = cb;
        /* ★ 走蓝牙时必须显式缩小 ALSA 缓冲，否则耳机音频比画面晚约 0.5 秒
         *（用户 m12491 第 3 条原话：「用耳机游玩时，耳机音频会落后于画面大概
         * 0.5 秒，但是扬声器没问题」）。
         *
         * 实测依据：对 bluealsa PCM 跑 `aplay` 时它的 setup dump 报
         * `BUFFER_BYTES: 98284` —— 98284 / 4 字节 / 2 声道 = 12285 帧，
         * 12285 / 48000 Hz = **0.256 s**，再算上 SBC 编码与耳机侧 jitter
         * buffer（~0.2 s，改不了），正好对上用户体感的 0.5 秒。
         * 也就是说 **默认缓冲本身就垫了 256 ms 死延迟**，把它压到 150 ms
         * 能直接砍掉约 100 ms 的可控延迟（剩下的 SBC 编码与耳机侧 jitter
         * buffer 属于链路固有时延，客户端改不了）。
         *
         * 选 150ms 缓冲 / 30ms 周期而不是更激进的值：云游戏音频是实时流，
         * 缓冲太小会被网络抖动打成 underrun（cg_aplay.log 刷
         * `underrun!!!`，听感是断续/爆音），比延迟更难受。150/30 是「明显
         * 变快且不欠载」的折中；真机验证用 `cat /userdisk/miniapp/data/cg_aplay.log`
         * 看有没有 underrun 记录即可判断要不要回调。
         * 扬声器路径（rk817/aw883xx 直连）本来就没有延迟问题，保持默认不动。
         *
         * 用 `-B`/`-F`（时间，微秒）而不是 `--buffer-size`/`--period-size`
         *（帧数）：换采样率时不用跟着改。 */
        if (b->audio_dev[0]) {
            argv[i++] = "-B"; argv[i++] = "150000";
            argv[i++] = "-F"; argv[i++] = "30000";
        }
        argv[i++] = "-";
        if (i >= (int)(sizeof(argv) / sizeof(argv[0]))) _exit(128); /* 不可能，纯防御 */
        argv[i] = NULL;
        execv(APLAY_PATH, argv);
        _exit(127);   /* aplay 不存在（理论上不会）；详见 cg_aplay.log */
    }
    setpgid(pid, pid);
    close(pp[0]);
    b->aplay_pid = pid;
    b->aplay_fd = pp[1];
    CM_LOGI("both: aplay spawned pid=%d fd=%d rate=%d ch=%d dev=%s",
            (int)pid, pp[1], rate > 0 ? rate : 48000, channels > 0 ? channels : 2,
            b->audio_dev[0] ? b->audio_dev : "default");
    return 0;
}

/* 判断 pid 还活着。SIGCHLD=SIG_IGN 下 waitpid 拿不到状态（ECHILD），
 * 但内核仍然会在子进程退出时释放 pid，所以 kill(pid, 0) 是可靠的存活探针：
 *   返回 0        -> 还活着
 *   -1 + ESRCH    -> 已经没了
 *   -1 + EPERM    -> 还活着（不可能是我们的错，但保守当成活着） */
static int aplay_alive(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;
}

/* 收尾外部 aplay 子进程。Bug5 里这一步出过问题，写法要点：
 *
 * ① **先关写端再发信号**：关掉管道写端后 aplay 的 read() 返回 0（EOF），
 *    它会 flush 剩余 PCM 并自己退出 —— 这是最干净的路径，也是「退出后还有
 *    残响」的正解。反过来先 SIGKILL 会让 ALSA 缓冲里那几十毫秒直接丢音。
 * ② **SIGTERM 和 SIGKILL 之间必须留时间**。旧代码两行连着发，SIGKILL 几乎
 *    总是先到，aplay 根本没机会走正常收尾（它的 atexit 要 stop/drain PCM）。
 * ③ **必须自证进程真的没了**，不能发完信号就当它死了。宿主进程
 *    SIGCHLD=SIG_IGN，waitpid 会 ECHILD，所以只能 kill(pid,0) 轮询。
 *    没有这一步的话，「aplay 卡死独占声卡」这种故障在日志里完全看不出来
 *    （Bug5 的 C 假设考的就是这条）。
 * ④ 节流总计最多约 1 秒，且只在真的卡死时才会走满。 */
static void aplay_stop(cm_box *b) {
    pid_t pid;
    int i;
    int waited_ms = 0;
    if (!b) return;

    /* ① 关写端，让 aplay 读 EOF 自己收尾 */
    if (b->aplay_fd >= 0) {
        close(b->aplay_fd);
        b->aplay_fd = -1;
    }
    pid = b->aplay_pid;
    b->aplay_pid = 0;
    if (pid <= 0) return;

    /* 给它 ~200ms 自己退（正常情况 aplay 收到 EOF 立刻退，不用我们杀） */
    for (i = 0; i < 20; i++) {
        if (!aplay_alive(pid)) {
            CM_LOGI("aplay pid=%d exited on EOF after %d ms", (int)pid, waited_ms);
            return;
        }
        cm_sleep_ms(10);
        waited_ms += 10;
    }

    /* ② 还没退：SIGTERM（让它 flush），再等 ~300ms */
    kill(pid, SIGTERM);
    for (i = 0; i < 30; i++) {
        if (!aplay_alive(pid)) {
            CM_LOGI("aplay pid=%d exited on SIGTERM after %d ms", (int)pid, waited_ms);
            return;
        }
        cm_sleep_ms(10);
        waited_ms += 10;
    }

    /* ③ 仍然活着：SIGKILL，再确认一次 */
    CM_LOGW("aplay pid=%d still alive after %d ms, SIGKILL", (int)pid, waited_ms);
    kill(pid, SIGKILL);
    for (i = 0; i < 50; i++) {
        if (!aplay_alive(pid)) {
            CM_LOGW("aplay pid=%d killed after %d ms (audio may clip)", (int)pid, waited_ms);
            return;
        }
        cm_sleep_ms(10);
        waited_ms += 10;
    }

    /* ④ 杀了 1 秒还在：这只可能是 TASK_UNINTERRUPTIBLE（卡在 ALSA 驱动里）。
     * 记下来 —— 这条日志是「声卡被我们独占」的唯一现场证据，后续排查全指望它。 */
    CM_LOGE("aplay pid=%d UNKILLABLE after %d ms (stuck in driver?)", (int)pid, waited_ms);
}

static GstElement *launch(const char *desc) {
    GError *err = NULL;
    cm_gst_enable_debug();
    GstElement *p = gst_parse_launch(desc, &err);
    if (!p) {
        CM_LOGE("gst_parse_launch failed: %s", desc);
        return NULL;
    }
    if (err) {
        /* 部分元素缺失时 parse_launch 仍返回 pipeline，只是某段没接上 */
        CM_LOGW("gst_parse_launch warning on: %s", desc);
    }
    return p;
}

/* 面板是 480x960 竖屏，但逻辑画布是 960x266 横屏（rotation=1 = 顺时针 90°）。
 * render-rectangle 要的是**面板物理绝对坐标**，所以逻辑矩形必须换算：
 *
 *     panel_x = PANEL_X_OFFSET + logical_y
 *     panel_y = PANEL_H - logical_x - logical_w
 *     panel_w = logical_h        <- 宽高互换（旋转 90°）
 *     panel_h = logical_w
 *
 * 【我上一版把 panel_y 写成 logical_y 是错的（用户报的"画面跑到最右侧"）】
 *   逻辑 (x,y) 映射到面板是 (107+y, 960-x)：逻辑 +x 是面板 -y 方向，
 *   逻辑 +y 是面板 +x 方向。矩形要按两个角换算，所以面板 y 的起点是
 *   960-(x+w)，而不是 x 或 y。写成 py=rect_y 会让 rect_y=0 的舞台
 *   落到面板 y=0..473（面板上半 = 用户视角最右侧）。
 *   验算：逻辑 (0,0,473,266) -> px=107, py=960-473=487, pw=266, ph=473，
 *   即面板 pos[107,487] rect[266x473]，占满逻辑 x=0..473 的左半舞台。
 *
 * PANEL_X_OFFSET=107：Falcon/weston 的合成面实际只占面板列 107..372（宽 266），
 * 左侧 107px 是面板死区。这个数字来自 PenBili/native/csrc/player.c:194 的真机实测，
 * 和 skill 里 documented 的映射一致。加错的症状是视频整体横向偏移一百多像素。
 * 隐藏视频（1x1）时**不能**加这个偏移，否则左侧会残留一条 107px 宽的条带。 */
/* ↓ 这两个宏只作为「cfg 里没给出时的历史默认值」保留，不再直接被 apply_rect
 * 使用：真正的换算参数来自 cm_cfg 的 panel_x_offset / panel_h（由 JS 侧
 * device_profile.canvasGeom() 下发），因为换机型时合成面在面板上的占位不同。
 * 数值与历史完全一致，所以老客户端的行为不变。 */
#define PANEL_X_OFFSET 107
/* 面板物理高度（竖屏 480x960）。逻辑 x 方向映射到面板 y 轴，换算要用它。 */
#define PANEL_H 960

static void apply_rect(cm_box *b) {
    char buf[64];
    int px, py, pw, ph;
    int xoff, ph_panel;
    if (!b || !b->sink) return;
    /* 面板换算参数来自盒子（建盒时从 cfg 拷来），不是编译期常量。 */
    xoff = b->panel_x_offset > 0 ? b->panel_x_offset : 0;
    ph_panel = b->panel_h > 0 ? b->panel_h : PANEL_H;

    /* w/h 有一个 <=0 时保持原样下发，让 kmssink 走 fullscreen 语义 */
    if (b->rect_w <= 1 && b->rect_h <= 1) {
        snprintf(buf, sizeof(buf), "<%d,%d,%d,%d>",
                 b->rect_x, b->rect_y, b->rect_w, b->rect_h);
    } else {
        px = xoff + b->rect_y;
        py = ph_panel - b->rect_x - b->rect_w;
        pw = b->rect_h;
        ph = b->rect_w;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        snprintf(buf, sizeof(buf), "<%d,%d,%d,%d>", px, py, pw, ph);
    }

    /* render-rectangle 是 GstValueArray of gint，字符串形式 "<x,y,w,h>"；
     * 它是**只写属性**，kmssink 只在 reconfigure（分配 plane）那一刻读一次，
     * PAUSED 下设无效 —— set_rect 那条路径靠 cm_box_set_rect 里的脉冲重配生效。 */
    gst_util_set_object_arg(b->sink, "render-rectangle", buf);
    /* 默认 force-aspect-ratio=true 会加黑边，我们要铺满目标矩形 */
    gst_util_set_object_arg(b->sink, "force-aspect-ratio", "false");
    CM_LOGI("kmssink rect logical=<%d,%d,%d,%d> panel->%s (xoff=%d panelH=%d panelW=%d)",
            b->rect_x, b->rect_y, b->rect_w, b->rect_h, buf,
            xoff, ph_panel, b->panel_w);
}

/* kmssink sink pad 的 BUFFER 探针：只做一件事 —— 计数。
 * 探针是在 kmssink 的流任务（也就是 GStreamer 的 sink 线程）里回调的，
 * 与媒体主循环不同线程，但只写一个 volatile 计数器，不需要加锁。 */
static GstPadProbeReturn cb_sink_buffer(GstPad *pad, GstPadProbeInfo *info,
                                        void *ud) {
    cm_box *b = (cm_box *)ud;
    (void)pad;
    (void)info;
    if (b) b->shown++;
    return GST_PAD_PROBE_OK;
}

cm_box *cm_box_create(cm_sess *s, int is_video) {
    cm_box *b;
    if (gst_ensure() != 0) return NULL;

    b = (cm_box *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->is_video = is_video;
    b->sess = s;
    /* ★ 必须显式置 -1：calloc 给的是 0，而 0 是 stdin 的 fd 号，
     * aplay_stop() 会对视频盒子误关 stdin（后果是后续 popen/system 全坏）。 */
    b->aplay_fd = -1;
    b->aplay_pid = 0;
    b->rect_x = s->cfg.rect_x;
    b->rect_y = s->cfg.rect_y;
    b->rect_w = s->cfg.rect_w;
    b->rect_h = s->cfg.rect_h;
    b->panel_x_offset = s->cfg.panel_x_offset;
    b->panel_h = s->cfg.panel_h;
    b->panel_w = s->cfg.panel_w;
    /* 兜底：rect 全 0 会让 kmssink 走 fullscreen 语义（自己去做 modesetting），
     * 那会跟 weston 抢 CRTC 并卡在 PAUSED。给一个面板内的安全矩形。 */
    if (b->rect_w <= 0 || b->rect_h <= 0) {
        b->rect_x = 0;
        b->rect_y = 0;
        b->rect_w = 473;
        b->rect_h = 266;
    }

    if (is_video) {
        /* 诊断开关：/userdisk/miniapp/data/softdec.txt 存在就跳过 mppvideodec
         * 直接走软解。目的是把「我们喂的码流坏」和「mppvideodec 对这条流
         * 的行为不对」这两层切开 —— 不用改代码、只 touch 一个文件就能 A/B：
         *   硬解全黑 + 软解有画面 ⇒ 码流是好的，锅在硬解这一层；
         *   硬解全黑 + 软解也全黑 ⇒ 锅在我们的 AU 组装 / SPS-PPS 注入。
         * 纯诊断，默认不打开（文件不存在 = 正常走硬解）。 */
        int force_soft = 0;
        {
            FILE *sf = fopen(VIDEO_SOFTDEC_SWITCH, "r");
            if (sf) { force_soft = 1; fclose(sf); }
        }
        if (force_soft) {
            CM_LOGW("video: forcing software h264 decoder (%s exists)", VIDEO_SOFTDEC_SWITCH);
            b->pipeline = launch(video_pipeline_soft());
        } else {
            b->pipeline = launch(video_pipeline_fmt());
        }
        if (!b->pipeline) {
            CM_LOGW("mppvideodec pipeline unavailable, trying software h264");
            b->pipeline = launch(video_pipeline_soft());
        }
        if (!b->pipeline) { free(b); return NULL; }
        b->src = (GstAppSrc *)gst_bin_get_by_name((GstBin *)b->pipeline, "vsrc");
        b->sink = gst_bin_get_by_name((GstBin *)b->pipeline, "vsink");
        {
            GstCaps *caps = gst_caps_from_string(
                "video/x-h264,stream-format=byte-stream,alignment=au");
            if (caps && b->src) gst_app_src_set_caps(b->src, caps);
            if (caps) gst_caps_unref(caps);
        }
        if (b->sink) {
            /* ★ 挂一个 BUFFER 探针，数「kmssink 真的吃进去多少帧」。
             * 这是判「画面有没有在动」的唯一可信信号（见 struct cm_box 里
             * shown 字段的注释）。探针返回值恒 OK，绝不改动 buffer。 */
            GstPad *sp = gst_element_get_static_pad(b->sink, "sink");
            if (sp) {
                gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER,
                                  cb_sink_buffer, b, NULL);
                gst_object_unref(sp);
            } else {
                CM_LOGW("video: sink pad not found, sink watchdog unavailable");
            }
            apply_rect(b);
        }
    } else {
        /* 开机默认 Playback Path=OFF（功放通路未接），开播前拨到 SPK。
         * ★ audio_route_ensure 会先快照，且**绝不再动 card1 音量**
         *   （Bug5：旧代码把系统音量 0,0 抬成 200,200）。 */
        char adesc[512];
        audio_route_ensure(s);
        /* 先起 aplay（拿到管道写端 fd），再让 GStreamer 的 fdsink 写进去。
         * 不用 alsasink 的原因见 cm_box 结构里 aplay_fd 的注释。 */
        if (aplay_spawn(b, 48000, 2) != 0) {
            CM_LOGE("aplay spawn failed");
            /* 走到这里是「已经改了 Playback Path 但盒子没建出来」，
             * 而 cm_box_destroy 永远不会被调用（b 还没交出去），
             * 所以必须在这里自己还原一次，否则路由会被永久留在 SPK。 */
            cm_audio_route_restore(s);
            free(b);
            return NULL;
        }
        snprintf(adesc, sizeof(adesc),
                 "appsrc name=asrc is-live=true format=time do-timestamp=true "
                 "caps=audio/x-opus ! queue ! opusdec ! audioconvert ! audioresample "
                 "! audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2 "
                 "! fdsink name=asink fd=%d sync=false", b->aplay_fd);
        b->pipeline = launch(adesc);
        if (!b->pipeline) {
            /* 同理：走 cm_box_destroy 而不是 aplay_stop + free，
             * 这样 aplay 收尾、fd 关闭、mixer 还原三件事都由同一段代码负责，
             * 不会在这条罕见失败路径上漏掉还原。 */
            cm_box_destroy(b);
            return NULL;
        }
        b->src = (GstAppSrc *)gst_bin_get_by_name((GstBin *)b->pipeline, "asrc");
        b->sink = gst_bin_get_by_name((GstBin *)b->pipeline, "asink");
        {
            /* 只写 audio/x-opus：本机 opusdec 的 sink 模板没有 rate 字段，
             * 带上 rate 会协商失败（bus 报 Internal data stream error）。 */
            /* ★ 必须把 channel-mapping-family 写死。
             * opusdec 的 sink 模板是两支：
             *   audio/x-opus, channel-mapping-family: 0
             *   audio/x-opus, channel-mapping-family: [1,255], channels, stream-count, coupled-count, rate
             * 只给 "audio/x-opus" 会同时命中两支、协商不出唯一结果，
             * appsrc 于是以 GST_FLOW_NOT_NEGOTIATED 收场，bus 报
             *   `Internal data stream error (gst_base_src_loop)`
             * 而音频一个字节都进不去（声卡停在 PREPARED / hw_ptr=0）。
             * 网关 fmtp 是 `sprop-stereo=1`，即双声道、mapping family 0。 */
            GstCaps *caps = gst_caps_from_string(
                "audio/x-opus,rate=48000,channels=2,channel-mapping-family=0");
            if (caps && b->src) gst_app_src_set_caps(b->src, caps);
            if (caps) gst_caps_unref(caps);
        }
    }

    if (!b->src) {
        CM_LOGE("%s: appsrc not found in pipeline", is_video ? "video" : "audio");
        cm_box_destroy(b);
        return NULL;
    }
    /* 不阻塞收包线程：满了丢旧数据 */
    gst_app_src_set_max_bytes(b->src, 2u * 1024u * 1024u);
    gst_app_src_set_leaky_type(b->src, GST_APP_LEAKY_DOWNSTREAM);
    gst_app_src_set_stream_type(b->src, GST_APP_STREAM_TYPE_STREAM);

    gst_element_set_state(b->pipeline, GST_STATE_PLAYING_);
    CM_LOGI("%s pipeline PLAYING", is_video ? "video" : "audio");
    return b;
}

void cm_box_destroy(cm_box *b) {
    if (!b) return;
    if (b->pipeline) {
        /* 必须先 NULL 再 unref，否则 KMS 平面（kmssink）不释放，UI 被永久遮挡。
         * set_state 是异步的：这里同步等它落地（最长 1.5s），再 unref。
         * 只发不等的写法在真机上会留下最后一帧盖住整个 UI。 */
        GstState got = GST_STATE_VOID_PENDING_;
        GstState pend = GST_STATE_VOID_PENDING_;
        int r = gst_element_set_state(b->pipeline, GST_STATE_NULL_);
        gst_element_get_state(b->pipeline, &got, &pend, 1500000000ULL /* 1.5s */);
        CM_LOGI("%s pipeline -> NULL (set=%d, state=%d, pending=%d)",
                b->is_video ? "video" : "audio", r, (int)got, (int)pend);
    }
    if (b->src) { gst_object_unref(b->src); b->src = NULL; }
    if (b->sink) { gst_object_unref(b->sink); b->sink = NULL; }
    if (b->pipeline) { gst_object_unref(b->pipeline); b->pipeline = NULL; }
    /* fdsink 释放后才关管道写端，aplay 读到 EOF 会正常 flush 退出。 */
    aplay_stop(b);
    /* ★ 还原全局 mixer（Bug5）：拆掉音频盒就把我们改过的 Playback Path 写回去。
     * 幂等 —— 四条拆管线路径（cm_media 看门狗退出 / cgmedia sess_free /
     * cgmedia js_stop / cm_cmd setAudio(0)）最终都走这里，谁先谁后都只还原一次。
     * 不在视频盒上调用：那是另一个 teammate 的文件路径，且音频快照与视频无关。 */
    if (!b->is_video) cm_audio_route_restore(b->sess);
    free(b->caps_str);
    free(b);
    CM_LOGI("box destroyed");
}

/* 把 bus 上已排队的 ERROR/WARNING/EOS 抽干并打进日志。
 * 非阻塞（timeout=0），可以每帧/每 stats 周期调一次，开销可忽略。
 * 返回：抽到的致命错误数（ERROR 计 1，EOS 计 0）。 */
int cm_box_drain_bus(cm_box *b) {
    GstBus *bus;
    int errs = 0;
    int guard = 0;
    if (!b || !b->pipeline) return 0;
    bus = gst_element_get_bus(b->pipeline);
    if (!bus) return 0;

    /* ★ 第 1 轮：只取 GST_MSG_ERROR。
     *
     * 为什么要拆成两轮而不是混着取：我们手写的 extern 里没有
     * gst_message_get_type / GST_MESSAGE_TYPE（那两个要走 GstMessage 结构布局，
     * 本文件没有 GStreamer 头，猜结构体会崩），所以「这条消息到底是 ERROR
     * 还是 WARNING」**只能靠 pop 时给的 types 过滤来区分**。
     * 拆分之后，凡是从这一轮 pop 出来的必然是 ERROR，可以放心把
     * b->failed 置位 —— 这正是 audio-route 留下的遗留
     * （docs/BUGFIX-bug3-bug4-gst.md §5.3：bus 报错只打印、盒子里永远
     * failed=0，于是 appsrc 继续收帧、画面永久停在最后一帧，表现就是全黑）。
     *
     * 对视频而言这个置位尤为关键：我们给 mppvideodec 加了
     * ignore-error=false max-errors=16，连续 16 次解码错误后它一定会
     * post flow error。以前那条 ERROR 只是被打印一句，push 继续成功，
     * 上层看到的是「fps 正常、appsrc 全部接受」，而屏幕上什么都没有。 */
    for (guard = 0; guard < 4; guard++) {
        GstMessage *msg = gst_bus_timed_pop_filtered(bus, 0, GST_MSG_ERROR);
        GErrorDetail *err = NULL;
        char *debug = NULL;
        if (!msg) break;
        gst_message_parse_error(msg, &err, &debug);
        {
            /* 首次出现用 CM_LOGE + 醒目前缀，保证一定会落进日志
             * （真机 console.warn/error 才落盘，CM_LOGE 走的是 error 级）。 */
            static int first_err_logged = 0;
            if (!first_err_logged) {
                first_err_logged = 1;
                CM_LOGE("%s bus ERROR (first) domain=%u code=%d msg=%s debug=%s",
                        b->is_video ? "video" : "audio",
                        err ? err->domain : 0u, err ? err->code : 0,
                        (err && err->message) ? err->message : "(none)",
                        debug ? debug : "(none)");
            } else {
                CM_LOGE("%s bus ERROR domain=%u code=%d msg=%s debug=%s",
                        b->is_video ? "video" : "audio",
                        err ? err->domain : 0u, err ? err->code : 0,
                        (err && err->message) ? err->message : "(none)",
                        debug ? debug : "(none)");
            }
        }
        if (b->is_video && !b->failed) {
            b->failed = 1;
            CM_LOGE("video box marked failed after decoder error: every later "
                    "cm_box_push is refused, so the plane freezes on its last "
                    "buffer (screen looks black/frozen). Look for the ERROR line above.");
        }
        /* 故意不 free：g_free 在本设备的 libglib 里查不到符号 */
        gst_message_unref(msg);
        errs++;
    }

    /* ★ 第 2 轮：WARNING / EOS 只打日志，绝不置 failed。
     * `Pipeline construction is invalid, please add queues.` 就是这一类的
     * WARNING（gst_base_sink_query_latency 发的），它是既有的、无害的，
     * 把它当致命错误会让每一次播放都瞬间 failed。 */
    for (guard = 0; guard < 4; guard++) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus, 0, GST_MSG_WARNING | GST_MSG_EOS);
        GErrorDetail *err = NULL;
        char *debug = NULL;
        if (!msg) break;
        gst_message_parse_warning(msg, &err, &debug);
        if (err && err->message) {
            CM_LOGW("%s bus WARN: %s%s%s", b->is_video ? "video" : "audio",
                    err->message,
                    debug ? " | debug=" : "", debug ? debug : "");
        } else if (debug) {
            CM_LOGW("%s bus msg: %s", b->is_video ? "video" : "audio", debug);
        }
        gst_message_unref(msg);
    }
    /* 不 unref bus：它是 pipeline 的，pipeline 析构时一起走 */
    return errs;
}

int cm_box_push(cm_box *b, const unsigned char *data, int len, uint64_t pts_ns, int key) {
    GstBuffer *buf;
    size_t wrote;
    GstFlowReturn fr;
    (void)pts_ns;   /* 见下方 hunk 2 说明：PTS 这一半无法落地 */
    if (!b || !b->src || b->failed || len <= 0) return -1;
    buf = gst_buffer_new_allocate(NULL, (size_t)len, NULL);
    if (!buf) return -1;
    wrote = gst_buffer_fill(buf, 0, data, (size_t)len);
    if (wrote != (size_t)len) {
        gst_buffer_unref(buf);
        return -1;
    }
    /* 【bug3/bug4 hunk 2（降级版）：只打 DELTA_UNIT，不打 PTS/DTS】
     *
     * 打 DELTA_UNIT 的意义：告诉 h264parse / mppvideodec「这一帧不能作为起点」。
     * 原来这里两行 `(void)` 把 key 完全丢掉，下游只能靠 H.264 NAL 类型自己猜；
     * 而 cm_rtp.c 的丢包逻辑**确实会丢帧**（`packet loss seen` / `auDrop`），
     * 被丢过帧之后 NAL 猜测与真实帧序一旦错位，解码器会把 P 帧当 I 帧用。
     * 显式标 DELTA_UNIT 是把我们已知的信息传下去，成本为零、没有行为风险。
     *
     * 为什么**不**设 PTS/DTS（任务书允许降级，理由如下，别当成漏做）：
     *   1) 本文件没有 GStreamer 头文件（全仓 grep gstbuffer.h = 0 命中，全靠手写
     *      声明）。GST_BUFFER_PTS()/GST_BUFFER_DTS() 是 gstbuffer.h 里的**结构体
     *      字段宏**，没有头文件根本无法展开，硬写结构体偏移等于赌 ABI 布局，
     *      赌错就是内存写坏。
     *   2) 官方 accessor `gst_buffer_get_pts/set_pts` 在真机 libgstreamer-1.0.so.0
     *      (1.22.0) 与本地 stub 的导出符号表里都不存在（已逐个 parse .dynsym），
     *      调用即未解析符号 → 整个 libjsapi_cgmedia.so 加载失败。
     *   3) appsrc 当前是 `format=time do-timestamp=true`，由 appsrc 按到达时间
     *      自己打时间戳；手工再塞一个 PTS 与之叠加，等于两套时间轴打架。
     *      要让 PTS 真正生效必须先改成 format=time 且关掉 do-timestamp，
     *      那会把 SINK 的调度基准也一并改掉，属于「为锦上添花冒破画出画风险」。
     *   所以时间戳这一半按任务书要求跳过，代价与后续做法见
     *   docs/BUGFIX-bug3-bug4-gst.md。
     *
     * ★ 2026-11-13 后续定案：kmssink 已改成 `sync=false`（见 KMS_SINK_PROPS
     *   上方长注释），SINK 不再按 PTS 调度 —— 也就是说上面第 3 条「两套时间轴
     *   打架」在**当前配置下已不成立**。但 PTS/DTS 仍然没加，理由回到第 1、2 条
     *   （没有头文件 + accessor 符号不存在），那两条与 sync 无关，依旧成立。
     *   也就是说：本次定案不是「补准 PTS」，而是「不再让 SINK 依赖 PTS」。 */
    if (b->is_video && !key) {
        gst_buffer_set_flags(buf,
                             gst_buffer_get_flags(buf) | GST_BUFFER_FLAG_DELTA_UNIT);
    }
    fr = gst_app_src_push_buffer(b->src, buf);
    if (fr != 0 /* GST_FLOW_OK */) {
        CM_LOGW("appsrc push flow=%d", fr);
        (void)cm_box_drain_bus(b);   /* 把 GStreamer 的原文错误挖出来 */
        b->failed = 1;
        return -1;
    }
    b->pushed++;
    b->pushed_bytes += (uint64_t)len;
    return 0;
}

/*
 * cm_box_reset_rect() —— 把管线退回 READY、按新矩形重建 plane、再回到 PLAYING。
 *
 * 为什么需要它（真机实测 + PenBili 的 6 轮对照实验）：
 *   kmssink 的 render-rectangle 是**单向棘轮**：
 *     240x427 --直接 set--> 1x1        生效
 *     1x1     --直接 set--> 240x427    无效（6 轮全部停在 1x1）
 *     1x1     --pipe PAUSED 循环-->    无效
 *     1x1     --sink READY/PLAYING-->  无效（plane 被 RELEASED 不回来）
 *     1x1     --sink NULL-->           无效
 *     1x1     --**pipe READY -> set rect -> pipe PLAYING**-->  有效
 *   即：缩小可以热改，放大必须让整条管线过一次 READY 重置。
 *
 * 之前我用「销毁重建整个 vbox」来做放大，结果是新管线在等 IDR，而网关
 * 只在收到 PLI 后补关键帧、实测补不上（日志里 rx=0 frames=0 kf=0，只有
 * 每 2 秒一条 PLI 石沉大海），画面永远回不来。
 * READY 重置不销毁 appsrc/caps，只让 h264parse 与解码器重新起一遍，
 * 配合随后的 PLI 请求 IDR 就能出画。
 *
 * READY 会把 pipeline 里所有元素 flush 掉，所以调用方必须在同一把
 * gst_mu 里把 video_ok / media_ev_sent 复位，让 JS 侧重新拿到 media 事件。
 */
int cm_box_reset_rect(cm_box *b, int x, int y, int w, int h) {
    GstState got = GST_STATE_VOID_PENDING_;
    GstState pend = GST_STATE_VOID_PENDING_;
    if (!b) return -1;
    b->rect_x = x;
    b->rect_y = y;
    b->rect_w = w;
    b->rect_h = h;
    if (!b->pipeline) return -1;
    gst_element_set_state(b->pipeline, GST_STATE_READY_);
    gst_element_get_state(b->pipeline, &got, &pend, 1500000000ULL /* 1.5s */);
    /* READY 下重新下发 render-rectangle，然后回 PLAYING，kmssink 在新一轮
     * READY->PAUSED 里会重新分配 plane 并读取这个属性。 */
    apply_rect(b);
    gst_element_set_state(b->pipeline, GST_STATE_PLAYING_);
    gst_element_get_state(b->pipeline, &got, &pend, 1500000000ULL /* 1.5s */);
    CM_LOGI("kmssink revamped via READY (state=%d pending=%d)", (int)got, (int)pend);
    return 0;
}

int cm_box_set_rect(cm_box *b, int x, int y, int w, int h) {
    if (!b) return -1;
    b->rect_x = x;
    b->rect_y = y;
    b->rect_w = w;
    b->rect_h = h;
    apply_rect(b);

    /* render-rectangle 是**只写属性**：kmssink 只在 reconfigure 那一刻
     * （分配/重配 DRM plane）读它，PLAYING 稳态下改了属性不会重新读。
     * 所以必须"脉冲"一次状态机让它重新配置 plane：
     *     PLAYING -> PAUSED -> PLAYING
     * 只 pulse 不重建管线，代价是几十毫秒黑屏，可以按 setRect 的频率承受。
     * 注意这是**单向棘轮**：一旦缩到 1x1，靠放大回不去，只能重建管线。 */
    if (b->pipeline) {
        gst_element_set_state(b->pipeline, GST_STATE_PAUSED_);
        gst_element_set_state(b->pipeline, GST_STATE_PLAYING_);
        CM_LOGI("kmssink reconfigured (pulse PAUSED->PLAYING)");
    }
    return 0;
}

void cm_box_set_caps_video(cm_box *b, int w, int h) {
    int old_w, old_h;
    if (!b || !b->src) return;
    /* h264parse 会重算 caps，这里只在拿到真实分辨率时更新统计值 */
    old_w = b->width;
    old_h = b->height;

    /* 【bug3/bug4 hunk 3：分辨率变化时逼下游重新协商】
     *
     * 为什么需要：mppvideodec 的 DPB / 输出 buffer 池是**按首个 SPS 分配的**。
     * 网关切档（或首帧前我们解析到的 SPS 是错的）时，width/height 变了但
     * 下游没收到新 caps，解码器仍旧按旧尺寸分配 —— 表现是画面挤压/花屏，
     * 或者干脆只出第一帧。
     *
     * 做法：向上游侧（appsrc 的 src pad）推一对 flush-start / flush-stop，
     * h264parse 与解码器收到 flush-stop 后会重新走一次 caps 协商 / 重分配 DPB。
     *
     * 三条**不可违反**的约束（都是真机踩过的）：
     *   1) ⚠ 绝不能用 gst_element_set_state(PAUSED→PLAYING) —— 本函数在收包线程
     *      持着 s->gst_mu 时被调用（cm_rtp.c:339-341 note_sps），set_state 会同步
     *      等待状态切换、而状态切换又要拿 gst_mu，直接死锁收包线程。
     *      只许发 event。
     *   2) 首次调用（old_w/old_h 还是 0）不 flush：那时下游根本还没协商过，
     *      发 flush 只会把刚起来的 pipeline 打回 NULL 状态附近，白丢首帧。
     *   3) flush-start 必须配一个 flush-stop(reset_time=TRUE)，否则 pipeline
     *      永远停在 flushing 状态，之后 push 的 buffer 全被丢掉（表现为
     *      video_ok=1 但屏幕不动）。
     * gst_pad_push_event 是 transfer-full（吃掉引用），所以成功失败都不要再 unref
     * event；只 unref 我们自己 get 出来的 pad。 */
    if (w > 0 && h > 0 && old_w > 0 && old_h > 0 && (w != old_w || h != old_h)) {
        GstState got = GST_STATE_VOID_PENDING_;
        GstState pend = GST_STATE_VOID_PENDING_;
        GstPad *pad;
        GstEvent *ev;
        b->width = w;
        b->height = h;
        pad = gst_element_get_static_pad((GstElement *)b->src, "src");
        if (!pad) {
            CM_LOGW("video caps %dx%d -> %dx%d: appsrc src pad not found, skip flush",
                    old_w, old_h, w, h);
            return;
        }
        /* 只在 PLAYING 下 flush：READY/NULL 时下一次 PLAYING 自然就会重新协商。 */
        if (b->pipeline) {
            gst_element_get_state(b->pipeline, &got, &pend, 0 /* 不等待 */);
        }
        if (got != GST_STATE_PLAYING_) {
            CM_LOGI("video caps %dx%d -> %dx%d (state=%d, no flush needed)",
                    old_w, old_h, w, h, (int)got);
            gst_object_unref(pad);
            return;
        }
        CM_LOGW("video caps CHANGED %dx%d -> %dx%d, flush to renegotiate",
                old_w, old_h, w, h);
        ev = gst_event_new_flush_start();
        if (ev && !gst_pad_push_event(pad, ev)) {
            CM_LOGW("flush-start push failed");
        }
        ev = gst_event_new_flush_stop(1 /* reset_time=TRUE */);
        if (ev && !gst_pad_push_event(pad, ev)) {
            CM_LOGW("flush-stop push failed");
        }
        gst_object_unref(pad);
        return;
    }
    b->width = w;
    b->height = h;
}

void *cm_box_pipeline(cm_box *b) { return b ? (void *)b->pipeline : NULL; }
const char *cm_box_desc(cm_box *b) {
    if (!b) return "";
    return b->is_video ? video_pipeline_fmt() : audio_pipeline_desc();
}

uint64_t cm_box_pushed(cm_box *b) { return b ? b->pushed : 0; }
/* 真正送到 kmssink 的帧数（探针计数）。与 cm_box_pushed 的区别见 struct
 * cm_box 里 shown 字段的注释 —— 判断画面上没上屏只能看这个。 */
uint64_t cm_box_shown(cm_box *b) { return b ? b->shown : 0; }
uint64_t cm_box_pushed_bytes(cm_box *b) { return b ? b->pushed_bytes : 0; }
