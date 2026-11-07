/*
 * cgmedia —— 网易云游戏（163CG）WebRTC 媒体接收 · 有道 X6 Pro 原生 JSAPI（纯 C）
 *
 * 为什么存在：云游戏网关只给标准 WebRTC（ICE + DTLS-SRTP + RTP），
 * 而设备的 GStreamer 没有 webrtcbin / libnice / libsrtp。
 * 所以这里的 native 自己实现 ICE(STUN) → DTLS(OpenSSL) → SRTP → RTP 拆包，
 * 只把裸 H.264 NAL / Opus 包交给 GStreamer（h264parse ! mppvideodec ! kmssink）。
 *
 * JS 用法：
 *   import { start, poll, cmd, stop, version } from 'cgmedia'
 *   const r = JSON.parse(start(cfgJson))     // {id:1} 或 {error:'...'}
 *   const evs = JSON.parse(poll(r.id))       // 事件数组
 *   cmd(r.id, '{"op":"stats"}')
 *   stop(r.id)
 *
 * 线程模型：
 *   - start() 立即返回，创建工作会话并启一个 pthread 收包线程
 *   - 事件由收包线程写进 per-session 链表（ev_mu 保护），poll() 在主线程 drain
 *   - stop() 幂等：置 cancelled、join 线程、拆 GStreamer、释放
 */

#include <jquick_config.h>
#include <jsmodules/JSCModuleExtension.h>
#include <quickjs/quickjs.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm_util.h"
#include "cm_session.h"

/* ==================== 事件队列 ==================== */

void cm_emit(cm_sess *s, const char *fmt, ...) {
    cm_buf b;
    va_list ap;
    char stackbuf[512];
    int n;
    cm_ev *e;

    if (!s) return;
    cm_buf_init(&b);
    va_start(ap, fmt);
    n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);
    if (n < 0) { cm_buf_free(&b); return; }
    if ((size_t)n < sizeof(stackbuf)) {
        cm_buf_put(&b, stackbuf, (size_t)n);
    } else {
        if (cm_buf_reserve(&b, (size_t)n + 1) < 0) { cm_buf_free(&b); return; }
        va_start(ap, fmt);
        vsnprintf((char *)b.p, (size_t)n + 1, fmt, ap);
        va_end(ap);
        b.len = (size_t)n;
    }
    if (cm_buf_terminate(&b) < 0) { cm_buf_free(&b); return; }

    e = (cm_ev *)calloc(1, sizeof(*e));
    if (!e) { cm_buf_free(&b); return; }
    e->json = (char *)b.p;

    CM_LOGD("emit #%d: %s", s->id, e->json);

    pthread_mutex_lock(&s->ev_mu);
    if (s->ev_tail) s->ev_tail->next = e; else s->ev_head = e;
    s->ev_tail = e;
    pthread_mutex_unlock(&s->ev_mu);
}

char *cm_ev_drain(cm_sess *s) {
    cm_buf out;
    cm_ev *list = NULL, *it;
    if (!s) return cm_strdup("[]");
    cm_buf_init(&out);
    pthread_mutex_lock(&s->ev_mu);
    list = s->ev_head;
    s->ev_head = NULL;
    s->ev_tail = NULL;
    pthread_mutex_unlock(&s->ev_mu);

    cm_buf_putc(&out, '[');
    for (it = list; it; it = it->next) {
        if (it != list) cm_buf_putc(&out, ',');
        cm_buf_puts(&out, it->json ? it->json : "{}");
    }
    cm_buf_putc(&out, ']');
    if (cm_buf_terminate(&out) < 0) { cm_buf_free(&out); return cm_strdup("[]"); }

    while (list) {
        cm_ev *n = list->next;
        free(list->json);
        free(list);
        list = n;
    }
    return (char *)out.p;
}

/* ==================== 会话表 ==================== */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static cm_sess *g_sessions = NULL;
static int g_next_id = 1;

static cm_sess *sess_find(int id) {
    cm_sess *s;
    pthread_mutex_lock(&g_mu);
    for (s = g_sessions; s; s = s->next) {
        if (s->id == id) break;
    }
    pthread_mutex_unlock(&g_mu);
    return s;
}

/* 会话一旦从表里摘除就再也不会被 poll/cmd 找到，所以取到指针后
 * stop 之前都算有效（stop 会先摘表再 join） */
static cm_sess *sess_alloc(void) {
    cm_sess *s = (cm_sess *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->ev_mu, NULL);
    pthread_mutex_init(&s->gst_mu, NULL);
    s->sock = -1;
    s->state = CM_ST_IDLE;
    s->ice_state = CM_ICE_IDLE;
    s->dtls_state = CM_DTLS_IDLE;
    pthread_mutex_lock(&g_mu);
    s->id = g_next_id++;
    s->next = g_sessions;
    g_sessions = s;
    pthread_mutex_unlock(&g_mu);
    return s;
}

static void sess_unlink(cm_sess *s) {
    cm_sess **pp;
    pthread_mutex_lock(&g_mu);
    pp = &g_sessions;
    while (*pp) {
        if (*pp == s) { *pp = s->next; break; }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_mu);
    s->next = NULL;
}

static void sess_free(cm_sess *s) {
    cm_ev *e;
    if (!s) return;
    while ((e = s->ev_head) != NULL) {
        s->ev_head = e->next;
        free(e->json);
        free(e);
    }
    if (s->vbox) cm_box_destroy(s->vbox);
    if (s->abox) cm_box_destroy(s->abox);
    cm_srtp_close(s);
    pthread_mutex_destroy(&s->ev_mu);
    pthread_mutex_destroy(&s->gst_mu);
    free(s);
}

/* ==================== 收包主线程（阶段 2..5 的驱动） ==================== */

static void *cm_rx_main(void *arg) {
    cm_sess *s = (cm_sess *)arg;

    s->state = CM_ST_ICE;
    s->ice_state = CM_ICE_PROBING;
    cm_emit(s, "{\"t\":\"ice\",\"state\":\"probing\"}");

    if (cm_ice_run(s) != 0) {
        s->ice_state = CM_ICE_FAILED;
        s->state = CM_ST_FAILED;
        cm_emit(s, "{\"t\":\"ice\",\"state\":\"failed\"}");
        cm_emit(s, "{\"t\":\"error\",\"msg\":\"ice failed\",\"code\":-1}");
        return NULL;
    }
    if (s->cancelled) return NULL;
    s->ice_state = CM_ICE_CONNECTED;
    s->state = CM_ST_DTLS;
    cm_emit(s, "{\"t\":\"ice\",\"state\":\"connected\"}");
    CM_LOGI("#%d ICE connected local_port=%d", s->id, s->local_port);

    s->dtls_state = CM_DTLS_HANDSHAKING;
    if (cm_dtls_run(s) != 0) {
        s->dtls_state = CM_DTLS_FAILED;
        s->state = CM_ST_FAILED;
        cm_emit(s, "{\"t\":\"dtls\",\"state\":\"failed\"}");
        cm_emit(s, "{\"t\":\"error\",\"msg\":\"dtls handshake failed\",\"code\":-2}");
        return NULL;
    }
    if (s->cancelled) return NULL;
    s->dtls_state = CM_DTLS_CONNECTED;
    cm_emit(s, "{\"t\":\"dtls\",\"state\":\"connected\",\"profile\":\"%s\"}",
            s->srtp_profile[0] ? s->srtp_profile : "SRTP_AES128_CM_SHA1_80");

    if (cm_srtp_open(s) != 0) {
        s->state = CM_ST_FAILED;
        cm_emit(s, "{\"t\":\"error\",\"msg\":\"srtp key init failed\",\"code\":-3}");
        return NULL;
    }

    s->state = CM_ST_MEDIA;
    CM_LOGI("#%d entering media loop", s->id);

    cm_media_loop(s);

    s->state = CM_ST_STOPPED;
    return NULL;
}

/* ==================== JS 导出 ==================== */

/* start(cfgJson) -> {"id":n} | {"error":".."} */
static JSValue js_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *json = NULL;
    cm_sess *s;
    cm_buf out;
    char *r;
    (void)this_val;

    if (argc < 1) return JS_NewString(ctx, "{\"error\":\"cfgJson required\"}");
    json = JS_ToCString(ctx, argv[0]);
    if (!json) return JS_NewString(ctx, "{\"error\":\"cfgJson not a string\"}");

    s = sess_alloc();
    if (!s) { JS_FreeCString(ctx, json); return JS_NewString(ctx, "{\"error\":\"nomem\"}"); }

    if (cm_cfg_parse(&s->cfg, json) != 0) {
        sess_unlink(s);
        sess_free(s);
        JS_FreeCString(ctx, json);
        return JS_NewString(ctx, "{\"error\":\"bad cfgJson\"}");
    }
    JS_FreeCString(ctx, json);

    cm_log_set_level(s->cfg.log_level);
    CM_LOGI("start #%d -> %s:%d ufrag=%s pt=%d/%d rect=%d,%d,%d,%d",
            s->id, s->cfg.remote_ip, s->cfg.remote_port, s->cfg.ufrag,
            s->cfg.video_pt, s->cfg.audio_pt,
            s->cfg.rect_x, s->cfg.rect_y, s->cfg.rect_w, s->cfg.rect_h);

    if (pthread_create(&s->rx_thread, NULL, cm_rx_main, s) != 0) {
        sess_unlink(s);
        sess_free(s);
        return JS_NewString(ctx, "{\"error\":\"thread create failed\"}");
    }
    s->rx_started = 1;

    cm_buf_init(&out);
    cm_buf_printf(&out, "{\"id\":%d}", s->id);
    if (cm_buf_terminate(&out) < 0) { cm_buf_free(&out); return JS_NewString(ctx, "{\"error\":\"nomem\"}"); }
    r = (char *)out.p;
    {
        JSValue v = JS_NewString(ctx, r);
        free(r);
        return v;
    }
}

/* poll(id) -> "[]" | "[{...},...]" */
static JSValue js_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    cm_sess *s;
    char *r;
    JSValue v;
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "[]");
    JS_ToInt32(ctx, &id, argv[0]);
    s = sess_find(id);
    if (!s) return JS_NewString(ctx, "[]");
    /* 心跳：媒体循环靠它判断小程序是否还活着（见 cm_media.c 的 SESSION_WATCHDOG） */
    s->last_poll_ms = cm_now_ms();
    r = cm_ev_drain(s);
    v = JS_NewString(ctx, r ? r : "[]");
    free(r);
    return v;
}

/* cmd(id, cmdJson) -> {"ok":true} | {"error":".."} */
static JSValue js_cmd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    cm_sess *s;
    const char *cj = NULL;
    char *op;
    int rc;
    (void)this_val;
    if (argc < 2) return JS_NewString(ctx, "{\"error\":\"cmd(id,cmdJson)\"}");
    JS_ToInt32(ctx, &id, argv[0]);
    cj = JS_ToCString(ctx, argv[1]);
    if (!cj) return JS_NewString(ctx, "{\"error\":\"cmdJson not a string\"}");
    s = sess_find(id);
    if (!s) { JS_FreeCString(ctx, cj); return JS_NewString(ctx, "{\"error\":\"no such id\"}"); }

    op = cm_json_get_str(cj, "op");
    rc = cm_cmd_apply(s, op ? op : "", cj);
    free(op);
    JS_FreeCString(ctx, cj);
    if (rc != 0) return JS_NewString(ctx, "{\"error\":\"cmd failed\"}");
    return JS_NewString(ctx, "{\"ok\":true}");
}

/* stop(id) -> {"ok":true} 幂等 */
static JSValue js_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    cm_sess *s;
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "{\"error\":\"stop(id)\"}");
    JS_ToInt32(ctx, &id, argv[0]);
    s = sess_find(id);
    if (!s) return JS_NewString(ctx, "{\"ok\":true,\"already\":true}");

    sess_unlink(s); /* 立刻摘表：之后 poll/cmd 拿不到，stop 也不会重入 */
    s->cancelled = 1;
    s->rx_quit = 1;
    if (s->rx_started) {
        pthread_join(s->rx_thread, NULL);
        s->rx_started = 0;
    }
    cm_box_destroy(s->vbox);
    s->vbox = NULL;
    cm_box_destroy(s->abox);
    s->abox = NULL;
    cm_srtp_close(s);
    if (s->sock >= 0) { cm_udp_close(s->sock); s->sock = -1; }
    CM_LOGI("stop #%d", id);
    sess_free(s);
    return JS_NewString(ctx, "{\"ok\":true}");
}

static JSValue js_version(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewString(ctx, "cgmedia/1.0.0 (ICE+DTLS-SRTP+H264/Opus->GStreamer)");
}

/* ==================== 模块注册 ==================== */

static int cgmedia_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, def, "start", JS_NewCFunction(ctx, js_start, "start", 1));
    JS_SetPropertyStr(ctx, def, "poll", JS_NewCFunction(ctx, js_poll, "poll", 1));
    JS_SetPropertyStr(ctx, def, "cmd", JS_NewCFunction(ctx, js_cmd, "cmd", 2));
    JS_SetPropertyStr(ctx, def, "stop", JS_NewCFunction(ctx, js_stop, "stop", 1));
    JS_SetPropertyStr(ctx, def, "version", JS_NewCFunction(ctx, js_version, "version", 0));
    JS_SetModuleExport(ctx, m, "default", def);
    JS_SetModuleExport(ctx, m, "start", JS_NewCFunction(ctx, js_start, "start", 1));
    JS_SetModuleExport(ctx, m, "poll", JS_NewCFunction(ctx, js_poll, "poll", 1));
    JS_SetModuleExport(ctx, m, "cmd", JS_NewCFunction(ctx, js_cmd, "cmd", 2));
    JS_SetModuleExport(ctx, m, "stop", JS_NewCFunction(ctx, js_stop, "stop", 1));
    JS_SetModuleExport(ctx, m, "version", JS_NewCFunction(ctx, js_version, "version", 0));
    return 0;
}

static JSModuleDef *cgmedia_module_load(JSContext *ctx, const char *moduleName) {
    if (strcmp(moduleName, "cgmedia") == 0) {
        JSModuleDef *m = JS_NewCModule(ctx, moduleName, cgmedia_module_init);
        if (!m) return NULL;
        JS_AddModuleExport(ctx, m, "default");
        JS_AddModuleExport(ctx, m, "start");
        JS_AddModuleExport(ctx, m, "poll");
        JS_AddModuleExport(ctx, m, "cmd");
        JS_AddModuleExport(ctx, m, "stop");
        JS_AddModuleExport(ctx, m, "version");
        return m;
    }
    return NULL;
}

extern void registerCModuleLoader(const char *moduleName, LoadCModuleFunction loader);

JQUICK_EXPORT void custom_init_jsapis(void) {
    registerCModuleLoader("cgmedia", &cgmedia_module_load);
}
