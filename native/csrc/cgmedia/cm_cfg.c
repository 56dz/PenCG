/*
 * cm_cfg.c —— cfgJson 解析
 *
 * cfgJson 是扁平的（rect 是唯一子对象），所以用 cm_util 里的迷你 JSON 取值即可。
 * 缺字段用文档默认值（ARCH.md native 契约）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cm_session.h"

static void copy_field(char *dst, size_t cap, char *src) {
    if (!src) return;
    if (cap == 0) return;
    snprintf(dst, cap, "%s", src);
    free(src);
}

int cm_cfg_parse(cm_cfg *cfg, const char *json) {
    char *rect;
    int v;

    if (!cfg || !json) return -1;

    /* 文档默认值 */
    memset(cfg, 0, sizeof(*cfg));
    cfg->video_pt = 98;
    cfg->audio_pt = 111;
    cfg->rect_w = 960;
    cfg->rect_h = 266;
    cfg->log_level = 3;
    cfg->audio_enabled = 1;
    snprintf(cfg->codec, sizeof(cfg->codec), "h264");

    copy_field(cfg->remote_ip, sizeof(cfg->remote_ip), cm_json_get_str(json, "remoteIp"));
    /* ufrag 语义：remote_ufrag = 对端（offer 的 a=ice-ufrag:），ufrag = 我方。
     * 两种命名都要吃：getcfg.mjs 用 remoteUfrag/remotePwd/ourUfrag/ourPwd，
     * media.js 用 ufrag/pwd（=对端）+ ourUfrag/ourPwd。 */
    copy_field(cfg->remote_ufrag, sizeof(cfg->remote_ufrag), cm_json_get_str(json, "remoteUfrag"));
    if (cfg->remote_ufrag[0] == '\0')
        copy_field(cfg->remote_ufrag, sizeof(cfg->remote_ufrag), cm_json_get_str(json, "ufrag"));
    copy_field(cfg->ufrag, sizeof(cfg->ufrag), cm_json_get_str(json, "ourUfrag"));
    copy_field(cfg->pwd, sizeof(cfg->pwd), cm_json_get_str(json, "remotePwd"));
    if (cfg->pwd[0] == '\0')
        copy_field(cfg->pwd, sizeof(cfg->pwd), cm_json_get_str(json, "pwd"));
    copy_field(cfg->our_pwd, sizeof(cfg->our_pwd), cm_json_get_str(json, "ourPwd"));
    copy_field(cfg->fingerprint, sizeof(cfg->fingerprint), cm_json_get_str(json, "fingerprint"));

    {
        char *c = cm_json_get_str(json, "codec");
        if (c) copy_field(cfg->codec, sizeof(cfg->codec), c);
    }

    cfg->remote_port = cm_json_get_int(json, "remotePort", 0);
    cfg->video_pt = cm_json_get_int(json, "videoPt", cfg->video_pt);
    cfg->audio_pt = cm_json_get_int(json, "audioPt", cfg->audio_pt);
    cfg->log_level = cm_json_get_int(json, "logLevel", cfg->log_level);
    cfg->audio_enabled = cm_json_get_int(json, "audio", 1) ? 1 : 0;
    cfg->no_rtcp = cm_json_get_int(json, "noRtcp", 0) ? 1 : 0;
    cfg->chrome_fp = cm_json_get_int(json, "chromeFp", 0) ? 1 : 0;

    v = cm_json_get_int(json, "videoSsrc", 0);
    cfg->video_ssrc = (uint32_t)v;
    {
        /* ssrc 可能是大数，走字符串路径兜底 */
        char *ss = cm_json_get_str(json, "videoSsrc");
        if (ss) {
            cfg->video_ssrc = (uint32_t)strtoul(ss, NULL, 10);
            free(ss);
        }
    }
    v = cm_json_get_int(json, "audioSsrc", 0);
    cfg->audio_ssrc = (uint32_t)v;
    {
        char *ss = cm_json_get_str(json, "audioSsrc");
        if (ss) {
            cfg->audio_ssrc = (uint32_t)strtoul(ss, NULL, 10);
            free(ss);
        }
    }

    rect = cm_json_get_obj(json, "rect");
    if (rect) {
        cfg->rect_x = cm_json_get_int(rect, "x", cfg->rect_x);
        cfg->rect_y = cm_json_get_int(rect, "y", cfg->rect_y);
        cfg->rect_w = cm_json_get_int(rect, "w", cfg->rect_w);
        cfg->rect_h = cm_json_get_int(rect, "h", cfg->rect_h);
        free(rect);
    }

    if (cfg->remote_ip[0] == '\0') return -1;
    if (cfg->remote_port <= 0 || cfg->remote_port > 65535) return -1;
    if (cfg->video_pt < 0 || cfg->video_pt > 127) cfg->video_pt = 98;
    if (cfg->audio_pt < 0 || cfg->audio_pt > 127) cfg->audio_pt = 111;
    if (cfg->rect_w <= 0) cfg->rect_w = 960;
    if (cfg->rect_h <= 0) cfg->rect_h = 266;
    return 0;
}
