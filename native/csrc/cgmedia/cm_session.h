/*
 * cm_session.h —— 一次云游戏媒体会话的全部状态
 *
 * 生命周期：start(cfgJson) 创建 → 后台收包线程驱动 → stop(id) 销毁。
 * 所有跨线程字段都受 s->mu 保护；事件队列另有 ev_mu。
 */
#ifndef CM_SESSION_H
#define CM_SESSION_H

#include <netinet/in.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>

#include "cm_util.h"

/* ---------------- 会话状态机 ---------------- */
enum {
    CM_ST_IDLE = 0,
    CM_ST_ICE,       /* 正在打 STUN Binding */
    CM_ST_DTLS,      /* ICE 通了，正在做 DTLS 握手 */
    CM_ST_MEDIA,     /* SRTP 解密中，往 appsrc 喂数据 */
    CM_ST_STOPPED,
    CM_ST_FAILED
};

enum {
    CM_ICE_IDLE = 0,
    CM_ICE_PROBING,
    CM_ICE_CONNECTED,
    CM_ICE_FAILED
};

enum {
    CM_DTLS_IDLE = 0,
    CM_DTLS_HANDSHAKING,
    CM_DTLS_CONNECTED,
    CM_DTLS_FAILED
};

/* ---------------- 配置（从 cfgJson 解析） ---------------- */
typedef struct {
    char remote_ip[64];
    int  remote_port;
    char ufrag[64];        /* 我们的 local ufrag（STUN USERNAME 后半段 / DTLS 无损） */
    char remote_ufrag[64]; /* 对端 ufrag（STUN USERNAME 前半段） */
    char pwd[128];         /* 对端 ice-pwd（STUN MESSAGE-INTEGRITY key） */
    char our_pwd[128];     /* 我方 ice-pwd（仅记录，ice-lite 下网关不校验） */
    char fingerprint[160]; /* "sha-256 AB:CD:.." */
    int  video_pt;
    int  audio_pt;
    uint32_t video_ssrc;
    uint32_t tx_ssrc;    /* 我方 RTCP sender SSRC（绝不能等于 video_ssrc） */
    uint32_t audio_ssrc;
    int  rect_x, rect_y, rect_w, rect_h;
    char codec[32];
    int  log_level;
    int  audio_enabled;
} cm_cfg;

/* ---------------- 事件（JSON 文本，poll 时拼数组） ---------------- */
typedef struct cm_ev {
    char *json;
    struct cm_ev *next;
} cm_ev;

/* ---------------- RTP 统计 ---------------- */
typedef struct {
    uint64_t rtp_rx;
    uint64_t rtp_bad;
    uint64_t bytes_rx;
    uint64_t nal_out;
    uint64_t frames;
    uint64_t srtp_auth_fail;
    uint32_t last_seq;
    int      have_last_seq;

    /* 收包分类计数（cm_udp_pump 维护）。
     * 排查"STUN keepalive 被当成 DTLS 记录"时靠这几个数对账。 */
    uint64_t rx_pkts;   /* socket 上读到的 datagram 总数 */
    uint64_t rx_stun;   /* 其中 STUN（ICE keepalive / 响应） */
    uint64_t rx_kept;   /* 我们回出去的 STUN Binding Success 个数 */
    uint64_t rx_dtls;   /* 其中 DTLS 记录（只喂给 OpenSSL） */
    uint64_t rx_rtp;
    uint64_t rx_rtcp;
    uint64_t rx_other;  /* 认不出来的，直接丢 */
    uint64_t rx_icmp;   /* recv 上冒出来的 ICMP 错误（ECONNREFUSED 等），瞬态，不算致命 */
    /* 音频侧：appsrc 收了多少包/字节，用来区分「没收到音频」与「收到了但没出声」 */
    uint64_t audio_pkts;
    uint64_t audio_bytes;
    /* 按 PT 的收包直方图（0..127）。用来回答「网关到底有没有发音频」——
     * 没有它就分不清「网关不发音频」和「我们认错 PT 丢掉了」。 */
    uint32_t pt_hist[128];
} cm_stats;

struct cm_sess;

/* 前向声明：各阶段模块在 cm_session.c 里被调用 */
typedef struct cm_box cm_box; /* GStreamer 盒子（视频/音频各一） */

typedef struct cm_sess {
    int id;
    int state;
    int cancelled;

    cm_cfg cfg;

    /* 事件队列 */
    pthread_mutex_t ev_mu;
    cm_ev *ev_head;
    cm_ev *ev_tail;

    /* 收包线程 */
    pthread_t rx_thread;
    int       rx_started;
    volatile int rx_quit;
    int       sock;
    int       local_port;

    /* 对端地址（对 STUN 源地址做校验用） */
    struct sockaddr_in peer;
    int peer_set;

    cm_stats stats;

    /* 状态标志（供 poll 拼事件） */
    int ice_state;
    int dtls_state;
    char srtp_profile[64];
    int video_ok;
    int audio_ok;
    int video_w, video_h;

    /* JS 侧 poll 心跳：js_poll() 每次调用都刷新。
     * 用来判断小程序是否还活着——退到后台 / 被系统冻结后 JS 定时器停摆，
     * poll 就断了；此时必须自己拆管线，否则 KMS 平面永远盖在系统 UI 上
     * （用户实测报障：游戏结束、软件退出，画面还在）。 */
    volatile uint64_t last_poll_ms;

    /* 统计事件节流 */
    uint64_t last_stat_ms;
    uint64_t stat_base_bytes;
    uint64_t stat_base_ms;

    /* keyframe 跟踪 / PLI 调度 */
    uint64_t kf_count;      /* 收到多少个 IDR */
    uint64_t last_kf_ms;
    int      have_kf;
    /* 最近一次 SPS 的 profile_idc / level_idc。
     * 换画质时分辨率恒为 1280x720（真机实测），所以判断「码流参数变了」
     * 只能靠这两个值 —— profile 一变，mppvideodec 就必须从新 IDR 重起。 */
    int      sps_profile;
    int      sps_level;
    int      media_ev_sent; /* media 事件是否已发（只发一次，带分辨率） */
    uint64_t next_pli_ms;
    int      pli_burst;     /* 首流连发计数 */
    uint64_t pli_count;
    uint64_t rtp_ts_pkts;   /* 用于 fps 估算 */
    uint64_t fps_base_frames;
    uint64_t fps_base_ms;
    int      fps;

    /* 视频平面是否被临时隐藏（云游戏内要弹系统键盘时用）。
     * KMS 视频平面的 zpos=2，盖在 Falcon 系统键盘所用合成面**之上**，
     * 不把画面藏起来用户根本看不到键盘。隐藏 = 缩到 1x1（真机上唯一
     * 不会被 kmssink 回退成铺满的合法几何），恢复 = 销毁重建盒子
     * （cm_box_set_rect 是单向棘轮，放大回去不生效）。 */
    volatile int video_hidden;

    /* GStreamer */
    cm_box *vbox;
    cm_box *abox;
    pthread_mutex_t gst_mu;

    /* 处理阶段内部状态指针（DTLS 上下文 / SRTP 状态相互独立） */
    void *dtls_priv;
    void *srtp_priv;

    struct cm_sess *next;
} cm_sess;

/* ---------------- 事件投递 ---------------- */
void cm_emit(cm_sess *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* 取出全部事件并拼成 JSON 数组（malloc，调用方 free）；无事件返回 "[]" */
char *cm_ev_drain(cm_sess *s);

/* ---------------- 各阶段模块 ---------------- */
/* 配置解析（cm_cfg.c） */
int  cm_cfg_parse(cm_cfg *cfg, const char *json);

/* ICE：创建 socket 并打 STUN，成功后 s->ice_state=CONNECTED（cm_ice.c） */
int  cm_ice_run(cm_sess *s);
/* DTLS：在已有 socket 上做握手 + 导出 SRTP key（cm_dtls.c） */
int  cm_dtls_run(cm_sess *s);
int  cm_dtls_get_km(cm_sess *s, unsigned char *km_out, int cap);
void cm_dtls_free(cm_sess *s);
/* 我方证书指纹（answer SDP 用） */
const char *cm_dtls_fingerprint(void);
void cm_dtls_diag(cm_sess *s, int *hdr, int *bad, int *retx,
                  int *in_msgs, int *in_bytes, int *stun_filtered);
/* SRTP：入站包解密（cm_srtp.c） */
int  cm_srtp_open(cm_sess *s);
void cm_srtp_close(cm_sess *s);
int  cm_srtp_unprotect(cm_sess *s, unsigned char *pkt, int len); /* 返回明文 RTP 长度 */
/* SRTCP 加密（发 PLI 用）；返回新长度（len+14），失败 <0 */
int  cm_srtcp_protect(cm_sess *s, unsigned char *pkt, int len, int cap);
int  cm_srtcp_ready(cm_sess *s);

/* 媒体主循环：收包 → 解密 → 拆包 → 喂 appsrc（cm_media.c） */
void cm_media_loop(cm_sess *s);

/* RTP 拆包（cm_rtp.c） */
void cm_rtp_on_packet(cm_sess *s, unsigned char *pkt, int len,
                      const char *from_ip, int from_port);

/* cm_cmd.c */
int  cm_cmd_apply(cm_sess *s, const char *op, const char *json);
void cm_send_pli(cm_sess *s);
void cm_kf_tick(cm_sess *s); /* keyframe 看门狗：缺 IDR 就补 PLI */

/* cm_gst.c：一个 appsrc→解码→输出 的盒子 */
cm_box *cm_box_create(cm_sess *s, int is_video);
void    cm_box_destroy(cm_box *b);
int     cm_box_push(cm_box *b, const unsigned char *data, int len, uint64_t pts_ns, int key);
int     cm_box_set_rect(cm_box *b, int x, int y, int w, int h);
/* 退 READY -> apply_rect -> 回 PLAYING。放大 render-rectangle 的唯一可靠途径
 * （kmssink 热改只能缩小，放大是单向棘轮）。 */
int     cm_box_reset_rect(cm_box *b, int x, int y, int w, int h);
/* 抽干 GStreamer bus 上的 ERROR/WARNING/EOS 并打进日志（非阻塞）。
 * 没有它，opusdec / alsasink 的致命错误会完全静默：appsrc push 照样返回 OK，
 * 而声卡 hw_ptr 永远是 0 —— 这正是「有音频包但没声音」的排查黑洞。 */
int     cm_box_drain_bus(cm_box *b);
void    cm_box_set_caps_video(cm_box *b, int w, int h);
uint64_t cm_box_pushed(cm_box *b);
uint64_t cm_box_pushed_bytes(cm_box *b);
/* 调试用：把内部 GstElement* pipeline 暴露给测试工具（gtest）看 bus/state。
 * 生产路径不调用；返回 NULL 表示没有管线。 */
void   *cm_box_pipeline(cm_box *b);
const char *cm_box_desc(cm_box *b);

/* UDP 工具（cm_net.c） */
int  cm_udp_open(cm_sess *s);
int  cm_udp_connect_peer(cm_sess *s);
void cm_udp_close(int fd);
int  cm_udp_send(int fd, const void *data, int len, const char *ip, int port);
/* 发到会话对端（已 connect 用 send，否则 sendto(&s->peer)） */
int  cm_udp_send_peer(cm_sess *s, const void *data, int len);
/* poll 读，超时返回 0，出错 <0，返回字节数 */
int  cm_udp_recv(int fd, unsigned char *buf, int cap, int timeout_ms,
                 char *from_ip, int *from_port);

/*
 * 统一收包泵（cm_net.c）：所有 datagram 都在这里按首字节分类。
 *
 * 用 recv(MSG_PEEK) 先看头：STUN 就消费并当场应答（ICE keepalive 必须回，
 * 否则网关认为 ICE 断了），垃圾直接丢；DTLS 记录 **不消费**，留在 socket
 * 里让 OpenSSL 的 dgram BIO 自己读 —— 这样不必自建 filter BIO，也不会
 * 破坏已在真机跑通的握手路径。
 *
 * 之所以必须这样做：网关在 ICE 连通后持续发 STUN Binding Request
 * keepalive（~1.8 秒一个）。这些包首字节是 0x00（不是合法的 DTLS
 * content type），OpenSSL 读到后会静默丢弃，而 DTLS 定时器继续重传
 * ClientHello，最终 25 秒超时 —— 真机日志 "in=14 in_bytes=182" 且
 * 每个都是 13 字节，正是 14 个 STUN 包各被读走 13 字节的记录头。
 */

/* DTLS 阶段用：清掉 STUN/垃圾。返回 1=头部有 DTLS 记录待 OpenSSL 读，
 * 0=没有，<0=socket 错误。 */
int  cm_udp_prepare_dtls(cm_sess *s, int timeout_ms);

/* 媒体阶段用：返回 1=RTP/SRTP（*out_len 有效）2=RTCP/SRTCP 0=无 <0=错误。
 * STUN 内部处理，DTLS 记录（握手之外的应用数据/alert）消费掉。 */
int  cm_udp_pump_media(cm_sess *s, unsigned char *buf, int cap, int timeout_ms,
                       char *from_ip, int *from_port, int *out_len);

/* STUN 处理（cm_ice.c）：cm_stun_is 只判断是不是 STUN；
 * cm_stun_handle 对 Binding Request 回 Binding Success（keepalive），返回
 * 1=已处理 0=不是 STUN <0=错误 */
int  cm_stun_is(const unsigned char *pkt, int n);
int  cm_stun_handle(cm_sess *s, const unsigned char *pkt, int n,
                    const char *from_ip, int from_port);

#endif /* CM_SESSION_H */
