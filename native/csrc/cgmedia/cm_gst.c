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
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

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
    uint64_t pushed;
    uint64_t pushed_bytes;
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
 *  can-scale=true     —— 允许 RGA 硬件缩放，1280x720 直接缩到目标矩形。 */
#define KMS_SINK_PROPS \
    " ! kmssink name=vsink plane-id=76 driver-name=rockchip " \
    "force-aspect-ratio=false can-scale=true sync=true"

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
    return "appsrc name=vsrc is-live=true format=time do-timestamp=true "
           "caps=video/x-h264,stream-format=byte-stream,alignment=au "
           "! h264parse ! mppvideodec rotation=270 ! videoconvert" KMS_SINK_PROPS;
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

/* ---------------- X6 Pro 音频路由（照抄 PenBili player.c v2.3.0/v2.3.2 语义） ----------------
 * 症状：alsasink 正常 PLAYING、`/proc/asound/card1/.../status = RUNNING`，
 * 但扬声器完全无声。根因：rk817 codec 混音控件 `Playback Path`（card0 numid=45）
 * 开机默认 OFF —— 功放通路根本没接上：
 *     amixer -c 0 cget name='Playback Path'  ->  values=0 (OFF)
 *     amixer -c 0 cset name='Playback Path' 'SPK'  ->  values=2，立刻出声
 * X5 上有系统音频守护进程做这件事；X6 Pro 没人做，必须自己拨。
 * 设计：仅当当前值是 OFF 才写 SPK（用户选了 HP/BT 就不动）；
 * 解析必须两步（先建 Item #n -> 名字表，再读 `: values=n` 定位）：
 * 首行 `name='Playback Path'` 也是单引号包的，取「第一对单引号」会拿到控件名本身
 * （PenBili v2.3.2 的教训）；而取「第一个 Item 行」会永远读到 OFF（我第一版踩的坑）。
 * 顺带：card1（aw883xx smart PA）的 `Master Playback Volume` 默认 0,0，
 * 仅当为 0 时设 200,200。mixer 值掉电不保留，每次建音频盒都要 ensure。 */
#define AUDIO_AMIXER_PATH "/bin/amixer"
#define AUDIO_AMIXER_CTL  "Playback Path"
#define AUDIO_SPK_VALUE   "SPK"

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

/* 仅当 card1 功放音量为 0 时抬到 200（用户/系统调过就尊重）。 */
static void audio_amp_volume_ensure(void) {
    char cmd[192];
    FILE *fp;
    fp = popen(AUDIO_AMIXER_PATH " -c 1 cget numid=54 2>/dev/null", "r");
    if (!fp) return;
    {
        char line[256];
        int zero = 0;
        while (fgets(line, sizeof(line), fp)) {
            char *v = strstr(line, ": values=");
            if (v) { zero = (strncmp(v, ": values=0,0", 12) == 0); break; }
        }
        pclose(fp);
        if (zero) {
            snprintf(cmd, sizeof(cmd),
                     AUDIO_AMIXER_PATH " -c 1 cset numid=54 200,200 >/dev/null 2>&1");
            (void)system(cmd);   /* 同样不能信返回值（SIGCHLD=SIG_IGN） */
            CM_LOGI("audio amp volume 0 -> 200");
        }
    }
}

/* 建音频盒（或每次开播）前调用。失败全部静音处理（别的机型没有该控件）。 */
static void audio_route_ensure(void) {
    char cur[64];
    char after[64];
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
    /* 回读确认：这是唯一可信的成功判据（和 imgConv 用产物文件自证同理）。 */
    if (audio_mixer_get_path(after, sizeof(after)) == 0) {
        CM_LOGI("Playback Path OFF -> %s (now=%s)", AUDIO_SPK_VALUE, after);
    } else {
        CM_LOGI("Playback Path OFF -> %s (readback failed)", AUDIO_SPK_VALUE);
    }
    audio_amp_volume_ensure();
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
    static int dbg_once = 0;
    if (dbg_once) return;
    dbg_once = 1;
    {
        FILE *df = fopen("/userdisk/miniapp/data/gstdebug.txt", "r");
        if (df) {
            char lv[256];
            if (fgets(lv, sizeof(lv), df)) {
                size_t n = strlen(lv);
                while (n && (lv[n - 1] == '\n' || lv[n - 1] == '\r')) lv[--n] = 0;
                if (n) {
                    gst_debug_set_threshold_from_string(lv, 1);
                    gst_debug_add_log_function((void *)cm_gst_log_handler, NULL, NULL);
                    CM_LOGW("GST_DEBUG enabled: %s", lv);
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
        /* "Device B0:A3:F2:AD:7E:FA Redmi Buds 6" */
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

static void aplay_stop(cm_box *b) {
    if (!b) return;
    if (b->aplay_fd >= 0) { close(b->aplay_fd); b->aplay_fd = -1; }
    if (b->aplay_pid > 0) {
        /* 宿主进程 SIGCHLD=SIG_IGN，子进程会被自动回收，waitpid 会 ECHILD；
         * 所以这里只发信号，不 waitpid。SIGTERM 后 aplay 会 flush 并退出。 */
        kill(b->aplay_pid, SIGTERM);
        kill(b->aplay_pid, SIGKILL);
        b->aplay_pid = 0;
    }
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
#define PANEL_X_OFFSET 107
/* 面板物理高度（竖屏 480x960）。逻辑 x 方向映射到面板 y 轴，换算要用它。 */
#define PANEL_H 960

static void apply_rect(cm_box *b) {
    char buf[64];
    int px, py, pw, ph;
    if (!b || !b->sink) return;

    /* w/h 有一个 <=0 时保持原样下发，让 kmssink 走 fullscreen 语义 */
    if (b->rect_w <= 1 && b->rect_h <= 1) {
        snprintf(buf, sizeof(buf), "<%d,%d,%d,%d>",
                 b->rect_x, b->rect_y, b->rect_w, b->rect_h);
    } else {
        px = PANEL_X_OFFSET + b->rect_y;
        py = PANEL_H - b->rect_x - b->rect_w;
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
    CM_LOGI("kmssink rect logical=<%d,%d,%d,%d> panel->%s",
            b->rect_x, b->rect_y, b->rect_w, b->rect_h, buf);
}

cm_box *cm_box_create(cm_sess *s, int is_video) {
    cm_box *b;
    if (gst_ensure() != 0) return NULL;

    b = (cm_box *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->is_video = is_video;
    /* ★ 必须显式置 -1：calloc 给的是 0，而 0 是 stdin 的 fd 号，
     * aplay_stop() 会对视频盒子误关 stdin（后果是后续 popen/system 全坏）。 */
    b->aplay_fd = -1;
    b->aplay_pid = 0;
    b->rect_x = s->cfg.rect_x;
    b->rect_y = s->cfg.rect_y;
    b->rect_w = s->cfg.rect_w;
    b->rect_h = s->cfg.rect_h;
    /* 兜底：rect 全 0 会让 kmssink 走 fullscreen 语义（自己去做 modesetting），
     * 那会跟 weston 抢 CRTC 并卡在 PAUSED。给一个面板内的安全矩形。 */
    if (b->rect_w <= 0 || b->rect_h <= 0) {
        b->rect_x = 0;
        b->rect_y = 0;
        b->rect_w = 473;
        b->rect_h = 266;
    }

    if (is_video) {
        b->pipeline = launch(video_pipeline_fmt());
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
        if (b->sink) apply_rect(b);
    } else {
        /* 开机默认 Playback Path=OFF（功放通路未接），必须在出声前拨到 SPK。 */
        char adesc[512];
        audio_route_ensure();
        /* 先起 aplay（拿到管道写端 fd），再让 GStreamer 的 fdsink 写进去。
         * 不用 alsasink 的原因见 cm_box 结构里 aplay_fd 的注释。 */
        if (aplay_spawn(b, 48000, 2) != 0) {
            CM_LOGE("aplay spawn failed");
            free(b);
            return NULL;
        }
        snprintf(adesc, sizeof(adesc),
                 "appsrc name=asrc is-live=true format=time do-timestamp=true "
                 "caps=audio/x-opus ! queue ! opusdec ! audioconvert ! audioresample "
                 "! audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2 "
                 "! fdsink name=asink fd=%d sync=false", b->aplay_fd);
        b->pipeline = launch(adesc);
        if (!b->pipeline) { aplay_stop(b); free(b); return NULL; }
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
    for (guard = 0; guard < 8; guard++) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus, 0 /* 不阻塞 */,
            GST_MSG_ERROR | GST_MSG_WARNING | GST_MSG_EOS);
        if (!msg) break;
        {
            GErrorDetail *err = NULL;
            char *debug = NULL;
            /* ERROR 与 WARNING 的解析函数内部实现相同，都用 parse_error 也行，
             * 但语义上分开叫更清楚；这里两个都试，谁给出 message 用谁。 */
            gst_message_parse_error(msg, &err, &debug);
            if (err && err->message) {
                CM_LOGE("%s bus: %s%s%s", b->is_video ? "video" : "audio",
                        err->message,
                        debug ? " | debug=" : "", debug ? debug : "");
                errs++;
            } else if (debug) {
                /* 可能是 EOS / WARNING：EOS 也要打出来，说明上游断流 */
                CM_LOGW("%s bus: %s", b->is_video ? "video" : "audio", debug);
            }
            /* 故意不 free：g_free 在本设备的 libglib 里 strings 查不到，
             * 引入未解析符号会让整个 .so 加载失败。错误路径只泄漏几十字节。 */
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
    (void)pts_ns;
    (void)key;
    if (!b || !b->src || b->failed || len <= 0) return -1;
    buf = gst_buffer_new_allocate(NULL, (size_t)len, NULL);
    if (!buf) return -1;
    wrote = gst_buffer_fill(buf, 0, data, (size_t)len);
    if (wrote != (size_t)len) {
        gst_buffer_unref(buf);
        return -1;
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
    if (!b || !b->src) return;
    /* h264parse 会重算 caps，这里只在拿到真实分辨率时更新统计值 */
    b->width = w;
    b->height = h;
}

void *cm_box_pipeline(cm_box *b) { return b ? (void *)b->pipeline : NULL; }
const char *cm_box_desc(cm_box *b) {
    if (!b) return "";
    return b->is_video ? video_pipeline_fmt() : audio_pipeline_desc();
}

uint64_t cm_box_pushed(cm_box *b) { return b ? b->pushed : 0; }
uint64_t cm_box_pushed_bytes(cm_box *b) { return b ? b->pushed_bytes : 0; }
