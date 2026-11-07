#include "cm_util.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_cm_log_level = 3;

void cm_log_set_level(int lv) {
    if (lv < 0) lv = 0;
    if (lv > 5) lv = 5;
    g_cm_log_level = lv;
}

void cm_log(int lv, const char *fmt, ...) {
    va_list ap;
    if (lv > g_cm_log_level) return;
    /* 统一前缀，便于 adb logcat / [pencg] 抓取 */
    fprintf(stdout, "[cgmedia] ");
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

/* ---------------- cm_buf ---------------- */

void cm_buf_init(cm_buf *b) {
    if (!b) return;
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void cm_buf_free(cm_buf *b) {
    if (!b) return;
    free(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void cm_buf_reset(cm_buf *b) {
    if (!b) return;
    b->len = 0;
}

int cm_buf_reserve(cm_buf *b, size_t need) {
    size_t cap;
    unsigned char *np;
    if (b->cap >= need) return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > (size_t)1 << 30) return -1;
        cap *= 2;
    }
    np = (unsigned char *)realloc(b->p, cap);
    if (!np) return -1;
    b->p = np;
    b->cap = cap;
    return 0;
}

int cm_buf_put(cm_buf *b, const void *data, size_t n) {
    if (!n) return 0;
    if (cm_buf_reserve(b, b->len + n) < 0) return -1;
    memcpy(b->p + b->len, data, n);
    b->len += n;
    return 0;
}

int cm_buf_putc(cm_buf *b, char c) {
    if (cm_buf_reserve(b, b->len + 1) < 0) return -1;
    b->p[b->len++] = (unsigned char)c;
    return 0;
}

int cm_buf_puts(cm_buf *b, const char *s) {
    if (!s) return 0;
    return cm_buf_put(b, s, strlen(s));
}

int cm_buf_printf(cm_buf *b, const char *fmt, ...) {
    va_list ap;
    char stackbuf[512];
    int n, r;
    va_start(ap, fmt);
    n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(stackbuf)) return cm_buf_put(b, stackbuf, (size_t)n);
    if (cm_buf_reserve(b, b->len + (size_t)n + 1) < 0) return -1;
    va_start(ap, fmt);
    r = vsnprintf((char *)b->p + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    if (r < 0) return -1;
    b->len += (size_t)n;
    return 0;
}

int cm_buf_json_strn(cm_buf *b, const char *s, size_t n) {
    size_t i;
    if (cm_buf_putc(b, '"') < 0) return -1;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  if (cm_buf_put(b, "\\\"", 2) < 0) return -1; break;
            case '\\': if (cm_buf_put(b, "\\\\", 2) < 0) return -1; break;
            case '\n': if (cm_buf_put(b, "\\n", 2) < 0) return -1; break;
            case '\r': if (cm_buf_put(b, "\\r", 2) < 0) return -1; break;
            case '\t': if (cm_buf_put(b, "\\t", 2) < 0) return -1; break;
            case '\b': if (cm_buf_put(b, "\\b", 2) < 0) return -1; break;
            case '\f': if (cm_buf_put(b, "\\f", 2) < 0) return -1; break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    if (cm_buf_put(b, tmp, 6) < 0) return -1;
                } else {
                    if (cm_buf_putc(b, (char)c) < 0) return -1;
                }
        }
    }
    return cm_buf_putc(b, '"');
}

int cm_buf_json_str(cm_buf *b, const char *s) {
    if (!s) return cm_buf_put(b, "null", 4);
    return cm_buf_json_strn(b, s, strlen(s));
}

int cm_buf_terminate(cm_buf *b) {
    if (cm_buf_reserve(b, b->len + 1) < 0) return -1;
    b->p[b->len] = '\0';
    return 0;
}

/* ---------------- misc ---------------- */

char *cm_strdup(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

uint64_t cm_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void cm_sleep_ms(int ms) {
    struct timespec ts;
    if (ms <= 0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---------------- 迷你 JSON 取值 ---------------- */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* 在 json 里定位 key: 并返回值的起始处（已跳过空白）。找不到返回 NULL。 */
static const char *find_value(const char *json, const char *key) {
    size_t klen = strlen(key);
    const char *p = json;
    int depth = 0;
    if (!json || !key) return NULL;
    while (*p) {
        char c = *p;
        if (c == '{' || c == '[') { depth++; p++; continue; }
        if (c == '}' || c == ']') { depth--; p++; continue; }
        if (c == '"') {
            const char *ks = p + 1;
            const char *ke = ks;
            while (*ke && *ke != '"') {
                if (*ke == '\\' && ke[1]) ke++;
                ke++;
            }
            if (*ke == '"') {
                const char *after = skip_ws(ke + 1);
                if (*after == ':' && (size_t)(ke - ks) == klen && strncmp(ks, key, klen) == 0) {
                    return skip_ws(after + 1);
                }
                p = ke + 1;
                continue;
            }
            p++;
            continue;
        }
        p++;
    }
    return NULL;
}

char *cm_json_get_str(const char *json, const char *key) {
    const char *v = find_value(json, key);
    cm_buf out;
    if (!v || *v != '"') return NULL;
    v++;
    cm_buf_init(&out);
    while (*v) {
        if (*v == '"') break;
        if (*v == '\\' && v[1]) {
            v++;
            switch (*v) {
                case 'n': cm_buf_putc(&out, '\n'); break;
                case 'r': cm_buf_putc(&out, '\r'); break;
                case 't': cm_buf_putc(&out, '\t'); break;
                case 'b': cm_buf_putc(&out, '\b'); break;
                case 'f': cm_buf_putc(&out, '\f'); break;
                case 'u': {
                    /* 只处理 ASCII 区，够用了（ip/ufrag/pwd/fingerprint 都是 ASCII） */
                    unsigned int cp = 0;
                    int i;
                    for (i = 0; i < 4 && v[1]; i++) {
                        char h = *++v;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned int)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned int)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned int)(h - 'A' + 10);
                    }
                    if (cp < 0x80) cm_buf_putc(&out, (char)cp);
                    else { cm_buf_putc(&out, '?'); }
                    break;
                }
                default: cm_buf_putc(&out, *v); break;
            }
            v++;
            continue;
        }
        cm_buf_putc(&out, *v);
        v++;
    }
    if (cm_buf_terminate(&out) < 0) { cm_buf_free(&out); return NULL; }
    return (char *)out.p;
}

int cm_json_get_int(const char *json, const char *key, int def) {
    const char *v = find_value(json, key);
    long r;
    char *end = NULL;
    if (!v) return def;
    if (*v == '"') {
        v++;
        r = strtol(v, &end, 10);
        if (end == v) return def;
        return (int)r;
    }
    r = strtol(v, &end, 10);
    if (end == v) return def;
    return (int)r;
}

char *cm_json_get_obj(const char *json, const char *key) {
    const char *v = find_value(json, key);
    const char *s;
    int depth = 0;
    if (!v || *v != '{') return NULL;
    s = v;
    while (*v) {
        if (*v == '{') depth++;
        else if (*v == '}') {
            depth--;
            if (depth == 0) {
                size_t n = (size_t)(v - s) + 1;
                char *out = (char *)malloc(n + 1);
                if (!out) return NULL;
                memcpy(out, s, n);
                out[n] = '\0';
                return out;
            }
        }
        v++;
    }
    return NULL;
}
