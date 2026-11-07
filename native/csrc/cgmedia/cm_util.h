/*
 * cm_util.h —— cgmedia 公共工具（纯 C）
 *
 * 只提供最基础的能力：定长日志、可增长字节缓冲、JSON 输出辅助。
 * 不做内存池/异常处理：所有函数返回 int，<0 表示失败。
 */
#ifndef CM_UTIL_H
#define CM_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* ---------------- 日志 ---------------- */
/* 级别：0=off 1=error 2=warn 3=info 4=debug 5=trace */
extern int g_cm_log_level;
void cm_log_set_level(int lv);
void cm_log(int lv, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define CM_LOGE(...) cm_log(1, __VA_ARGS__)
#define CM_LOGW(...) cm_log(2, __VA_ARGS__)
#define CM_LOGI(...) cm_log(3, __VA_ARGS__)
#define CM_LOGD(...) cm_log(4, __VA_ARGS__)

/* ---------------- 字节/字符串缓冲 ---------------- */
typedef struct {
    unsigned char *p;
    size_t len;
    size_t cap;
} cm_buf;

void cm_buf_init(cm_buf *b);
void cm_buf_free(cm_buf *b);
void cm_buf_reset(cm_buf *b);
int  cm_buf_reserve(cm_buf *b, size_t need);
int  cm_buf_put(cm_buf *b, const void *data, size_t n);
int  cm_buf_putc(cm_buf *b, char c);
int  cm_buf_puts(cm_buf *b, const char *s);
int  cm_buf_printf(cm_buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* JSON 字符串字面量（含两侧引号，转义控制字符） */
int  cm_buf_json_str(cm_buf *b, const char *s);
int  cm_buf_json_strn(cm_buf *b, const char *s, size_t n);
/* 把 CM_STRING 保证以 '\0' 结尾（追加一个但不算进 len） */
int  cm_buf_terminate(cm_buf *b);

/* ---------------- 小工具 ---------------- */
char *cm_strdup(const char *s);
/* 大端读写 */
static inline uint16_t cm_rd16(const unsigned char *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t cm_rd24(const unsigned char *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static inline uint32_t cm_rd32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void cm_wr16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static inline void cm_wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
/* 毫秒单调时钟 */
uint64_t cm_now_ms(void);
void cm_sleep_ms(int ms);

/* ---------------- 迷你 JSON 取值 ----------------
 * 只支持我们自己的 cfgJson（扁平对象 + 一个 rect 子对象），
 * 不引入外部解析库。返回 malloc 的字符串（调用方 free）。 */
char *cm_json_get_str(const char *json, const char *key);   /* 字符串值 */
int   cm_json_get_int(const char *json, const char *key, int def);
char *cm_json_get_obj(const char *json, const char *key);   /* 子对象原文 {...} */

#endif /* CM_UTIL_H */
