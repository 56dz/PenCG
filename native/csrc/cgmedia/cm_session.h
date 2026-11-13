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
    /* 取证开关：置 1 时主循环跳过 PLI / NACK / RR 三种上行 RTCP，
     * 用来二分「上行 RTCP 是否触发网关停推」。默认 0（正常发送）。 */
    int  no_rtcp;
    /* 取证开关：置 1 时 ClientHello 的 cipher_list 改用 Chrome 顺序
     * （只含 TLS1.2 现代套件、去掉静态 RSA），用来 A/B「网关是否按
     * DTLS/JA3 类指纹风控」。use_srtp/扩展集合不动。默认 0。 */
    int  chrome_fp;
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
    /* 我们主动发的 ICE consent（cm_ice_consent_tick）**被网关回复**的次数。
     *
     * 这个计数是判定「网关到底认不认我们的 consent」的唯一依据：
     *   consentSent 涨、consentResp 不涨 → 包发出去了但没人理（端口/凭据错、
     *     或网关根本不要求 consent），说明 consent 不是 bug1 的解
     *   consentSent 与 consentResp 同步涨 → 通道是双向健康的，那 4m18s
     *     停推就另有原因（回到 RTCP/TWCC 或服务端策略） */
    uint64_t rx_consent_resp;
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
    /* ---- 视频完整性（cm_rtp.c 维护，emit_stat 上报）----
     *
     * 这些计数每一个都对应一条「用户看得见」的故障，是判断补丁有没有生效的
     * 唯一依据（JS 侧的 frames 是滞后的一阶导数，不能用来判断画面好没好）：
     *
     *   vid_lost     RTP 序号缺口累计（估计丢包数）。丢包本身不是致命伤 ——
     *                致命的是把缺片的访问单元当完整帧推给解码器
     *   vid_dup      重复包（直接丢掉，不再 append 到 AU 里）
     *   vid_reorder  乱序/迟到包（按到达顺序 append 会把分片插错位置，必须整帧丢）
     *   vid_au_drop  因缺片/乱序而整帧丢弃的 AU 数。>0 说明补丁在起作用；
     *                长期为 0 而用户仍看到残留，说明丢包不在 RTP 层（要查 SRTP/网关）
     *   vid_gate_drop 首个完整 IDR 之前丢弃的 P 帧数（避免一上来就满屏绿/花）
     *   vid_kf_req   丢包触发的紧急 PLI 次数。和 kf_count 对比就能回答
     *                「关键帧请求到底有没有被网关响应」：
     *                kf_req 涨而 kf 不涨 = 网关不理 PLI
     *   vid_sps_inject 给裸 IDR 补插缓存 SPS/PPS 的次数。≥1 说明网关
     *                不重发参数集，旧代码在这条路径上会直接绿屏 */
    uint64_t vid_lost;
    uint64_t vid_dup;
    uint64_t vid_reorder;
    uint64_t vid_au_drop;
    uint64_t vid_gate_drop;
    uint64_t vid_kf_req;
    uint64_t vid_kf_req_ms;   /* 上次紧急 PLI 时刻（发送限速用） */
    uint64_t vid_sps_inject;

    /* ---- RTCP Generic NACK（RFC 4585 §6.2.1，PT=205 FMT=1）----
     *
     * 真机现场（2026-11-13 16:25-16:42，原神 yssy，高铁 Wi-Fi，丢包 5-8%）：
     *   `still no IDR (rx=74418 kfReq=468 gateDrop=15511 auDrop=401)`
     * 401 个残缺 AU 换掉了 15511 个正常 P 帧（约 39:1）。原因不是网关不理 PLI
     * （kfReq=468 说明我们一直在要），而是**每个 IDR 都被丢包打碎**：一个
     * 720p IDR 有 30~60 个 RTP 包，5-8% 丢包率下几乎必缺一片，缺一片就整帧
     * 报废。没有重传机制时这是个死循环：要 IDR → IDR 被撕 → 再要 → 再被撕。
     *
     * 加上 NACK 后，缺的那几个包可以点名要回来，IDR 才有可能拼完整。 */
    uint64_t vid_nack_sent;      /* 发出的 NACK 报文条数（≠ 请修的包数） */
    uint64_t vid_nack_req;       /* 累计点名请求重传的包数（FCI 覆盖的 seq 数） */
    uint64_t vid_nack_recovered; /* 重传真的送回来并填进 AU 的包数。和 nack_req
                                  * 对比就是网关的重传响应率：为 0 说明网关压根
                                  * 不支持 NACK（那就只能靠 IDR 饥饿兜底） */
    /* IDR 饥饿兜底的放行次数（见 cm_rtp.c 的 IDR_STARVE_RELAX_MS）。
     * >0 说明用户看到过花屏 —— 但那是我们**主动**用花屏换回画面在动，
     * 比永久冻结好。持续增长说明网关侧真的太差。 */
    uint64_t vid_relax;
    /* 落在当前 AU seq 范围之外的重传包数（R2 修正后才有的计数）。
     * >0 说明缺口表 TTL 跨过了 AU 边界 —— 归零不影响功能，是健康信号。 */
    uint64_t vid_stale_rtx;
    /* 因为丢包被整帧丢掉的 **P 帧 AU** 个数（不含 IDR：IDR 走 vid_au_drop）。
     * L1 修好后它同时是「drop broken P AU」那行日志的限速计数器。
     * 与 gateDrop 的区别：gateDrop 是「没关键帧，主动不放行」，
     * pDrop 是「有关键帧、但这一帧拼不全所以只丢这一帧」—— 后者增长
     * 说明丢包在发生但画面还能继续动（这是我们要的健康态）。 */
    uint64_t vid_p_drop;

    /* ---- RTCP RR（RFC 3550 §6.4.1 / §A.3）的统计基础（cm_rtp.c 维护）----
     *
     * 这一段的存在理由：我们以前**从来没发过 Receiver Report**，网关手里
     * 只有 PLI（PSFB，只在缺关键帧时发），既不知道我们还在收，也不知道
     * 丢包率和抖动。这在协议上是残缺的：RFC 3550 §6.4 要求接收端按带宽
     * 折算的间隔周期发 RR，任何合规的 SFU 都可能拿它做带宽估计与丢包反馈。
     *
     * ★ 注意定位：发 RR 是**补齐协议义务**，不是 bug1（约四分钟停推）的
     * 修复方案。现场证据（音视频同时停、三次复现落在 4m18s±1s、网关全程
     * 不回任何 RTCP、WS 会话健康）指向服务端挂机 kickout 定时器，与 RR
     * 无关。详见 docs/BUGFIX-bug1-rtcp-rr.md。
     *
     * 这里全是**只读派生量**：只做累加，不参与任何丢包判定/关键帧闸门，
     * 现有 vid_lost / have_kf 那套逻辑一行都没动。 */
    /* received 的口径按 RFC 3550 A.3：**含重复包与乱序包**（网关只要发出去了
     * 就算收到），这样 expected - received 才是真正的「网络丢包」，而不是
     * 把重复包也算成丢包。 */
    uint64_t vid_rx;          /* 通过 SSRC 校验的视频包数＝RR 的 received */
    uint32_t vid_ssrc_seen;   /* 实际收到的视频 SSRC（SDP 没给 video_ssrc 时兜底） */
    uint32_t ext_seq_max;     /* 扩展最高序号（16 位 seq + 32 位循环计数，RFC A.1） */
    uint32_t ext_seq_base;    /* 首个扩展序号，expected 的基准 */
    int      have_ext_seq;    /* ext_seq_* 是否已初始化 */
    uint64_t jitter_q4;       /* 到达间隔抖动，单位 1/16 个 RTP 时间戳（Q4 定点） */
    int32_t  last_transit;    /* 上一包的 relative transit（RFC 3550 A.8） */
    int      have_jitter;     /* last_transit / jitter_q4 已初始化 */
    uint64_t rtcp_sr_rx;      /* 收到网关 SR(PT=200) 的个数（含读不出 NTP 的 SRTCP） */
    uint32_t sr_ntp_mid;      /* 最近一个 SR 的 NTP 中间 32 位 -> RR 的 LSR */
    uint64_t sr_arr_ms;       /* 该 SR 的到达时刻（单调毫秒）-> 算 DLSR */
    /* ★ have_sr 目前恒为 0，这是有意的：入站 RTCP 是 SRTCP 密文（cm_srtp.c
     * 只解密 RTP），SR 的 NTP 时间戳落在被加密的那一段里，我们读不到。
     * 报文里 LSR/DLSR 填 0 完全合法（RFC 3550 §6.4.1：语义是"还没有可用的
     * 参考点"），网关只是算不了 RTT，不影响它收到 RR。等哪天入站 SRTCP
     * 能解密了，把这三个字段填上就能自动变成完整 RR，构造函数不用改。 */
    int      have_sr;         /* sr_ntp_mid / sr_arr_ms 是否有效（当前恒 0） */
    /* 上一次发 RR 时的 expected / received 快照：fraction lost 按 RFC 3550 A.3
     * 只在「两个 RR 之间」统计，用累计值直接算会得到开机以来的平均丢包率，
     * 网关无法据此判断「现在链路突然变差」。 */
    uint32_t rr_expected_prior;
    uint64_t rr_received_prior;

    /* ---- 音频侧的同一套 RR 统计（cm_rtp.c 维护）----
     *
     * RR 的 report block 是「每个被报告的 SSRC 一段」，音视频是两个独立
     * SSRC，必须各报一段 —— 只报视频的话，网关对音频的丢包/抖动仍然
     * 一无所知，而现场证据显示音频（PT=111）包量是视频的 2 倍多
     * （11339 vs 7362），它才是网关最需要反馈的那条流。
     * 口径与视频完全一致（RFC 3550 A.1/A.3/A.8），不参与任何判定。 */
    uint64_t aud_rx;          /* 通过 SSRC 校验的音频包数＝RR 的 received */
    uint32_t aud_ssrc_seen;   /* 实际收到的音频 SSRC（audio_ssrc 缺失时兜底） */
    uint32_t aud_ext_seq_max;
    uint32_t aud_ext_seq_base;
    int      have_aud_ext_seq;
    uint64_t aud_jitter_q4;
    int32_t  aud_last_transit;
    int      have_aud_jitter;
    uint32_t aud_rr_expected_prior;
    uint64_t aud_rr_received_prior;
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

    /* ---- ICE consent refresh（cm_ice.c 的 cm_ice_consent_tick）----
     *
     * 网关是 ice-lite，连通后**它自己也会周期发 Binding Request**（我们
     * 在 cm_stun_handle 里回 0x0101）；但 RFC 8445 §11 的 consent 是双向义务，
     * 浏览器侧实测 candidate-pair.consentRequestsSent 每 ~2.5s +1，
     * 而我们在连通后立刻停发 STUN —— 这是与官方客户端最大的行为差异。
     *
     * 现场表现（bug1）：约 4m18s 后网关单方面停推 RTP，WS 仍 open、
     * 我们的 WS 心跳仍被回显、没有任何 error/close op。
     * 分析见 docs/断网根因-163CG复核.md。 */
    uint64_t next_ice_consent_ms;  /* 下一次发 consent 的时刻（0 = 未启动） */
    uint64_t ice_consent_count;    /* 已发出的 consent 个数（对账用） */
    uint64_t ice_consent_log_ms;   /* 日志限速 */
    /* 连通时协商定的角色（0=controlling，1=controlled）。部分媒体服回 487
     * Role Conflict 后 cm_ice_run 会切到 controlled，consent 必须沿用同一角色，
     * 否则会话期又发 USE-CANDIDATE 会被当成非法提名。 */
    int ice_controlled;

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
    /* RTCP RR 调度（cm_rtcp_tick，1 秒一次）。
     * next_rr_ms 是下一次到期时刻；rr_count 用于日志编号与真机对账；
     * rr_log_ms 做日志限速（每秒一条太吵，见 cm_rtcp_tick 的注释）。 */
    uint64_t next_rr_ms;
    uint64_t rr_count;
    uint64_t rr_log_ms;
    uint32_t rr_last_lost;  /* 上次打日志时的 cumulative lost，用于「只在变化时打」 */
    uint32_t rr_last_jit;
    /* ---- RTCP Generic NACK 调度（cm_cmd.c 的 cm_nack_tick）----
     *
     * 缺口由收包侧（cm_rtp.c）记录，发送由这里限速。**必须分离**：
     * 收包线程每 20ms 一轮，缺口是连续成片出现的（丢包是突发性的），
     * 如果收一个缺口就发一条 NACK，丢 100 个包会瞬间打出上百条报文，
     * 网关那边只会看到刷屏然后忽略我们。所以收包侧只写「待重传列表」，
     * 发送侧按固定节奏批量打包（每条 NACK 能覆盖最多 17 个连续 seq）。 */
    uint64_t next_nack_ms;  /* 下一次允许发 NACK 的时刻 */
    uint64_t nack_log_ms;   /* 日志限速 */
    /* IDR 饥饿兜底的计时基准：第一次因「没有 IDR」而走闸门丢弃的时刻。
     * 0 = 当前不在饥饿状态（有 IDR，或刚恢复）。 */
    uint64_t kf_starve_ms;
    /* P1-7 修正（REVIEW-report P1-7）：饥饿放行的「重新收紧」期限。
     *
     * 旧行为：放行时把 have_kf 置 1 就再也不管了 —— 若网关此后只发
     * P 帧且都不坏，have_kf 永远是 1，闸门永久打开 → 持续花屏。
     * 现在放行时额外记下这个时刻，到期仍没有完整 IDR 就重新关闸。
     * 0 = 当前不处于「已放行待收紧」状态。见 cm_rtp.c 的 RELAX_RESET_MS。 */
    uint64_t relax_until_ms;
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

    /* ---- 音频路由快照 / 还原（Bug5）----
     *
     * 我们在开播前会动全局 mixer（card0 `Playback Path` OFF -> SPK）。
     * 那是**进程之外的全局状态**，退出时必须还原，否则用户回到系统后
     * 路由被我们留在 SPK（Bug5 的「退出后连有道查词都有声」就是这条 +
     * 功放音量被抬到 200 共同造成的）。
     *
     * 快照放在会话上而不是盒子上：盒子可能创建失败，也可能先于会话结束
     * 被销毁（setAudio(0) / setVideoVisible 重建），而还原必须在任何一条
     * 拆管线路径上幂等地跑一次。audio_snap_done 兼作「改过没有」的开关。
     *
     * ★ card1 numid=54（aw883xx Master Playback Volume）是**系统音量本体**
     *   （真机：`hal-audio -s 0|50` 只改它，card0 不动；/etc/asound.conf 的
     *   spk_softvol 也绑在 card 1 的这个控件上）。所以只快照、只回读、绝不写。
     *   这里留着它纯粹是为了退出时打一条「我们没动过系统音量」的自证日志。 */
    int  audio_snap_done;              /* 快照已做（0=没改过，还原时直接返回） */
    char audio_path_snap[32];          /* 进游戏前 Playback Path 的原文，如 "OFF" */
    char audio_path_wrote[32];         /* 我们实际写下去并回读确认的值；空=没改 */
    char audio_amp_snap[32];           /* 进游戏前 card1#54，如 "0,0"（只读） */
    char audio_amp_after[32];          /* 退出时 card1#54，用于日志自证（只读） */

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
/* ICE consent refresh（RFC 8445 §11，本实现按 2.5s 周期）。
 *
 * 网关 ice-lite 在连通后仍会周期发 Binding Request，我们回 0x0101 只能证明
 * 「它问到了、我们活着」；而 consent 是**由发起方（我们）主动证明自己还在**。
 * 浏览器侧 consentRequestsSent 每 ~2.5s +1，这是与官方客户端唯一的
 * 周期性双向报文差异 —— 不加这个，约 4m18s 后网关会单方面停推 RTP。
 *
 * 由 cm_media_loop 每轮调用（内部按 next_ice_consent_ms 限速，不阻塞）。
 * 未连通 / 无 pwd 时直接返回，安全幂等。 */
void cm_ice_consent_tick(cm_sess *s);
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
/* SRTP 解密失败/收到 RTCP NACK 时把「当前正在拼的访问单元」标脏：
 * 下一帧 flush 时整帧丢弃，并补一次 PLI。丢的是 FU-A 中间片时，
 * 旧代码会把残缺 AU 推给解码器 → 上一帧马赛克残留。 */
void cm_rtp_note_stale(cm_sess *s, const char *why);
/* 换局/重连时复位拆包状态（seq 基准 / 残缺 AU / 缓存的 SPS-PPS）。 */
void cm_rtp_reset(cm_sess *s);
/* 取走「最近发现的视频 seq 缺口」并清空。
 *
 * cm_cmd.c 的 NACK 发送在拿到这批缺口后，用 BLP 位图压成 FCI 发出去。
 * 返回实际写入 out[] 的条目数（0 = 没有缺口）。
 *
 * out[] 每组 4 字节：序号(2B 大端) + 优先级(1B) + 保留(1B)。
 * 优先级：2 = 属于 IDR AU 或携带 SPS/PPS（最高，重发轮数放宽），
 *         1 = 普通 P 帧缺口。
 * 之所以把优先级一起带出来：真机 401 个 auDrop 换掉 15511 个 P 帧，
 * 说明「IDR 被撕」才是灾难源头，重传预算必须优先给它。 */
int  cm_rtp_take_nack_gaps(cm_sess *s, unsigned char *out, int cap_entries);
/* IDR 的 hold 窗口到期检查。cm_media.c 的主循环每轮调一次。
 *
 * 一个 AU 的 marker 到了就该 flush，但被 NACK 点名的包最快也要一个 RTT
 * 才回来。IDR 丢一片整帧就废（真机实测：1 个坏 IDR 赔 39 个 P 帧），
 * 所以对 IDR 单独压住 IDR_HOLD_MS 再放行。P 帧不压（压了纯属加延迟）。 */
void cm_rtp_flush_held(cm_sess *s);

/* cm_cmd.c */
int  cm_cmd_apply(cm_sess *s, const char *op, const char *json);
void cm_send_pli(cm_sess *s);
void cm_kf_tick(cm_sess *s); /* keyframe 看门狗：缺 IDR 就补 PLI */
/* RTCP Receiver Report 周期发送（RFC 3550 §6.4.2，PT=201），1 秒一次。
 * 见 cm_cmd.c 头部注释：为什么必须发 RR、为什么走 SRTCP。 */
void cm_rtcp_tick(cm_sess *s);
/* RTCP Generic NACK 周期发送（RFC 4585 §6.2.1，PT=205 FMT=1）。
 *
 * 和 cm_rtcp_tick 一样是「我们主动对网关说的话」，但节奏不同：
 * RR 是 1Hz 的链路质量汇报，NACK 是 30~50ms 的紧急补包请求
 * （晚了网关的发送缓冲已经丢了，重传也没有意义）。
 * 没有缺口时它什么都不发，空转成本只是取一次列表。 */
void cm_nack_tick(cm_sess *s);

/* cm_gst.c：一个 appsrc→解码→输出 的盒子 */
cm_box *cm_box_create(cm_sess *s, int is_video);
void    cm_box_destroy(cm_box *b);
/* 还原我们在开播前改过的全局 mixer（card0 `Playback Path`），并回读 card1
 * 系统音量做自证。幂等，可以在任何收尾路径上多调。cm_box_destroy 的音频
 * 分支已经自动调它，这里导出是为了「盒子已经被置 NULL 之后还要再兜一次」
 * 的场景（cm_cmd 的 setAudio(0) 就是这么用的）。 */
void    cm_audio_route_restore(cm_sess *s);
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
