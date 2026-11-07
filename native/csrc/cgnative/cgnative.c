/*
 * cgnative —— 有道词典笔 X6 Pro / Falcon 原生 JSAPI 模块（纯 C）
 *
 * 存在理由（三条，都不可绕过）：
 *  1) 运行时的 $falcon.jsapi.http.request 会对 data 字符串做二次 JSON 序列化，
 *     **发不出裸请求体**；网易云游戏 v2 接口的请求体是裸 Base64，必须原样发送。
 *  2) JS 侧没有 crypto（无 WebCrypto、无 BigInt），登录链路需要 MD5/SHA256/
 *     HMAC-SHA1/3DES/Base64/随机数，只能下沉到 native。
 *  3) 平台完全没有 WebSocket，设备 libcurl 7.79.1 也不支持 ws://，
 *     播放信令通道只能自己用 OpenSSL + RFC6455 实现。
 *
 * 线程模型：同步接口跑在调用线程（JS 线程）上，仅供诊断与少量短请求；
 * 页面主链路用 startRequest/takeResult 轮询、wsOpen/wsPoll 取帧，避免冻住 JS 线程。
 *
 * 纯 C（不链接 libstdc++），与 httpjson.c 同源骨架。
 */

#include <jquick_config.h>
#include <jsmodules/JSCModuleExtension.h>
#include <quickjs/quickjs.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#define CG_VERSION "cgnative 1.0.0"
/* 构建日期写成字面量：zig 以 -Werror 编，__DATE__/__TIME__ 触 -Wdate-time
 * （不可复现构建），会直接编译失败。要发新版就改这一行。 */
#define CG_BUILD_DATE "2026-11-06"

/* ---------------- libcurl 最小声明（不依赖 curl 头文件） ---------------- */
typedef void CURL;
struct curl_slist;
typedef struct curl_slist CURL_slist;

extern int curl_global_init(long flags);
extern CURL *curl_easy_init(void);
extern void curl_easy_cleanup(CURL *handle);
extern int curl_easy_setopt(CURL *handle, int option, ...);
extern int curl_easy_perform(CURL *handle);
extern int curl_easy_getinfo(CURL *handle, int info, ...);
extern CURL_slist *curl_slist_append(CURL_slist *list, const char *s);
extern void curl_slist_free_all(CURL_slist *list);
extern const char *curl_easy_strerror(int code);

#define CURL_GLOBAL_DEFAULT_C     3L
#define CURLOPT_WRITEDATA         10001
#define CURLOPT_URL               10002
#define CURLOPT_POSTFIELDS        10015
#define CURLOPT_HTTPHEADER        10023
#define CURLOPT_CUSTOMREQUEST     10036
#define CURLOPT_WRITEFUNCTION     20011
#define CURLOPT_POST              47
#define CURLOPT_FOLLOWLOCATION    52
#define CURLOPT_POSTFIELDSIZE     60
#define CURLOPT_SSL_VERIFYPEER    64
#define CURLOPT_SSL_VERIFYHOST    81
#define CURLOPT_NOSIGNAL          99
#define CURLOPT_TIMEOUT_MS        155
#define CURLOPT_CONNECTTIMEOUT_MS 156
#define CURLINFO_RESPONSE_CODE    0x200002

/* ---------------- 动态字节缓冲 ---------------- */
struct buf {
    char *data;
    size_t len;
    size_t cap;
};

static int buf_reserve(struct buf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *p;
        while (cap < b->len + extra + 1) cap <<= 1;
        p = (char *)realloc(b->data, cap);
        if (!p) return -1;
        b->data = p;
        b->cap = cap;
    }
    return 0;
}

static void buf_add(struct buf *b, const void *s, size_t n) {
    if (!n) return;
    if (buf_reserve(b, n) != 0) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_adds(struct buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_addc(struct buf *b, char c) { buf_add(b, &c, 1); }

static void buf_addi(struct buf *b, long v) {
    char t[32];
    int n = snprintf(t, sizeof(t), "%ld", v);
    if (n > 0) buf_add(b, t, (size_t)n);
}

static void buf_free(struct buf *b) {
    if (b->data) free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* UTF-8 序列长度；非法首字节返回 0 */
static int utf8_seq_len(const unsigned char *p, size_t avail) {
    unsigned char c = p[0];
    int n, i;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    else return 0;
    if ((size_t)n > avail) return 0;
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
    }
    return n;
}

/* 追加 JSON 字符串字面量：控制字符转义，非法 UTF-8 字节降级为 \u00XX。
 * 保住 body 里的中文原样，同时保证 JS 侧 JSON.parse 不会踩到坏字节。 */
static void buf_addjson(struct buf *b, const char *s, size_t n) {
    size_t i = 0;
    buf_addc(b, '"');
    while (i < n) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x80) {
            switch (ch) {
                case '"':  buf_add(b, "\\\"", 2); break;
                case '\\': buf_add(b, "\\\\", 2); break;
                case '\n': buf_add(b, "\\n", 2); break;
                case '\r': buf_add(b, "\\r", 2); break;
                case '\t': buf_add(b, "\\t", 2); break;
                default:
                    if (ch < 0x20) {
                        char tmp[8];
                        snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
                        buf_adds(b, tmp);
                    } else {
                        buf_addc(b, (char)ch);
                    }
            }
            i++;
        } else {
            int sl = utf8_seq_len((const unsigned char *)s + i, n - i);
            if (sl > 1) {
                buf_add(b, s + i, (size_t)sl);
                i += (size_t)sl;
            } else {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
                buf_adds(b, tmp);
                i++;
            }
        }
    }
    buf_addc(b, '"');
}

/* ---------------- Base64 / Hex ---------------- */
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char *in, size_t len, struct buf *out) {
    size_t i = 0;
    while (i + 2 < len) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        buf_addc(out, B64_CHARS[(v >> 18) & 63]);
        buf_addc(out, B64_CHARS[(v >> 12) & 63]);
        buf_addc(out, B64_CHARS[(v >> 6) & 63]);
        buf_addc(out, B64_CHARS[v & 63]);
        i += 3;
    }
    if (len - i == 1) {
        unsigned v = (unsigned)in[i] << 16;
        buf_addc(out, B64_CHARS[(v >> 18) & 63]);
        buf_addc(out, B64_CHARS[(v >> 12) & 63]);
        buf_adds(out, "==");
    } else if (len - i == 2) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
        buf_addc(out, B64_CHARS[(v >> 18) & 63]);
        buf_addc(out, B64_CHARS[(v >> 12) & 63]);
        buf_addc(out, B64_CHARS[(v >> 6) & 63]);
        buf_addc(out, '=');
    }
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* 返回 0 成功；-1 输入非法 */
static int b64_decode(const char *in, size_t len, struct buf *out) {
    unsigned acc = 0;
    int bits = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        int ch = (unsigned char)in[i];
        int v;
        if (ch == '=') break;
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        v = b64_val(ch);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            char byte;
            bits -= 8;
            byte = (char)((acc >> bits) & 0xFF);
            buf_add(out, &byte, 1);
        }
    }
    return 0;
}

static void hex_encode(const unsigned char *in, size_t len, struct buf *out) {
    static const char HEX[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        char pair[2];
        pair[0] = HEX[(in[i] >> 4) & 0xF];
        pair[1] = HEX[in[i] & 0xF];
        buf_add(out, pair, 2);
    }
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "00112233..." -> 字节；允许冒号/空格分隔（"AB:CD:EF" 风格） */
static int hex_decode(const char *in, size_t len, unsigned char *out, size_t outCap,
                      size_t *outLen) {
    size_t i, o = 0;
    int hi = -1;
    for (i = 0; i < len; i++) {
        int v = hex_val((unsigned char)in[i]);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            if (o >= outCap) return -1;
            out[o++] = (unsigned char)((hi << 4) | v);
            hi = -1;
        }
    }
    *outLen = o;
    return 0;
}

/* ---------------- OpenSSL 摘要封装 ---------------- */
static JSValue md_hex_js(JSContext *ctx, const EVP_MD *md, const char *data, size_t len) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    struct buf hex;
    JSValue out;

    memset(&hex, 0, sizeof(hex));
    if (!EVP_Digest(data, len, digest, &dlen, md, NULL)) {
        return JS_ThrowInternalError(ctx, "cgnative: digest failed");
    }
    hex_encode(digest, dlen, &hex);
    out = JS_NewStringLen(ctx, hex.data ? hex.data : "", hex.len);
    buf_free(&hex);
    return out;
}

static const char *arg_cstr(JSContext *ctx, JSValueConst v, size_t *len) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        if (len) *len = 0;
        return NULL;
    }
    return JS_ToCStringLen(ctx, len, v);
}

/* ---------------- JS: crypto 原语 ---------------- */
static JSValue js_sha256_hex(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *s;
    size_t n = 0;
    JSValue r;
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "cgnative.sha256Hex(str)");
    s = arg_cstr(ctx, argv[0], &n);
    if (!s) return JS_EXCEPTION;
    r = md_hex_js(ctx, EVP_sha256(), s, n);
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue js_md5_hex(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *s;
    size_t n = 0;
    JSValue r;
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "cgnative.md5Hex(str)");
    s = arg_cstr(ctx, argv[0], &n);
    if (!s) return JS_EXCEPTION;
    r = md_hex_js(ctx, EVP_md5(), s, n);
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue js_b64enc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *s;
    size_t n = 0;
    struct buf out;
    JSValue r;
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "cgnative.b64enc(str)");
    s = arg_cstr(ctx, argv[0], &n);
    if (!s) return JS_EXCEPTION;
    memset(&out, 0, sizeof(out));
    b64_encode((const unsigned char *)s, n, &out);
    r = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    buf_free(&out);
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue js_b64dec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *s;
    const char *in;
    size_t n = 0;
    struct buf out;
    JSValue r;
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "cgnative.b64dec(str)");
    s = arg_cstr(ctx, argv[0], &n);
    if (!s) return JS_EXCEPTION;
    in = s;
    while (*in == ' ' || *in == '\r' || *in == '\n' || *in == '\t') in++;
    if (strncmp(in, "data:", 5) == 0) {
        const char *comma = strchr(in, ',');
        if (comma) in = comma + 1;
    }
    memset(&out, 0, sizeof(out));
    if (b64_decode(in, n - (size_t)(in - s), &out) != 0) {
        buf_free(&out);
        JS_FreeCString(ctx, s);
        return JS_ThrowInternalError(ctx, "cgnative: invalid base64");
    }
    r = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    buf_free(&out);
    JS_FreeCString(ctx, s);
    return r;
}

/* tripleDesB64(plain, key, ivHex) —— 3DES-EDE3-CBC + PKCS7，输出 Base64（无换行） */
static JSValue js_triple_des_b64(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    const char *plain = NULL;
    const char *key = NULL;
    const char *ivHex = NULL;
    size_t plen = 0, klen = 0, ivlen = 0;
    unsigned char keyBuf[24];
    unsigned char iv[8];
    size_t keyBytes = 0, ivBytes = 0;
    unsigned char *outBuf = NULL;
    int outLen = 0, finalLen = 0;
    EVP_CIPHER_CTX *c = NULL;
    struct buf b64;
    JSValue r = JS_UNDEFINED;
    (void)this_val;

    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "cgnative.tripleDesB64(plain, key, ivHex)");
    }
    plain = arg_cstr(ctx, argv[0], &plen);
    if (!plain) return JS_EXCEPTION;
    key = arg_cstr(ctx, argv[1], &klen);
    ivHex = arg_cstr(ctx, argv[2], &ivlen);

    /* key：优先按 24 字节原始字符串用；不足 24 时按 hex 解码（容错） */
    memset(keyBuf, 0, sizeof(keyBuf));
    if (key && klen >= 24) {
        memcpy(keyBuf, key, 24);
    } else if (key) {
        if (hex_decode(key, klen, keyBuf, sizeof(keyBuf), &keyBytes) != 0 || keyBytes < 24) {
            size_t copy = klen > 24 ? 24 : klen;
            memset(keyBuf, 0, sizeof(keyBuf));
            if (copy) memcpy(keyBuf, key, copy);
        }
    }

    memset(iv, 0, sizeof(iv));
    if (ivHex) {
        if (hex_decode(ivHex, ivlen, iv, sizeof(iv), &ivBytes) != 0 || ivBytes < 8) {
            memset(iv, 0, sizeof(iv));
        }
    }

    outBuf = (unsigned char *)malloc(plen + 32);
    if (!outBuf) {
        r = JS_ThrowInternalError(ctx, "cgnative: out of memory");
        goto done;
    }
    c = EVP_CIPHER_CTX_new();
    if (!c) {
        r = JS_ThrowInternalError(ctx, "cgnative: EVP_CIPHER_CTX_new failed");
        goto done;
    }
    if (EVP_EncryptInit_ex(c, EVP_des_ede3_cbc(), NULL, keyBuf, iv) != 1) {
        r = JS_ThrowInternalError(ctx, "cgnative: 3des init failed");
        goto done;
    }
    EVP_CIPHER_CTX_set_padding(c, 1);
    if (plen > 0 && EVP_EncryptUpdate(c, outBuf, &outLen, (const unsigned char *)plain,
                                      (int)plen) != 1) {
        r = JS_ThrowInternalError(ctx, "cgnative: 3des update failed");
        goto done;
    }
    if (EVP_EncryptFinal_ex(c, outBuf + outLen, &finalLen) != 1) {
        r = JS_ThrowInternalError(ctx, "cgnative: 3des final failed");
        goto done;
    }
    outLen += finalLen;

    memset(&b64, 0, sizeof(b64));
    b64_encode(outBuf, (size_t)outLen, &b64);
    r = JS_NewStringLen(ctx, b64.data ? b64.data : "", b64.len);
    buf_free(&b64);

done:
    if (c) EVP_CIPHER_CTX_free(c);
    if (outBuf) free(outBuf);
    if (plain) JS_FreeCString(ctx, plain);
    if (key) JS_FreeCString(ctx, key);
    if (ivHex) JS_FreeCString(ctx, ivHex);
    return r;
}

static JSValue js_hmac_sha1_b64(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    const char *key = NULL;
    const char *data = NULL;
    size_t klen = 0, dlen = 0;
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int macLen = 0;
    struct buf out;
    JSValue r;
    (void)this_val;

    if (argc < 2) return JS_ThrowTypeError(ctx, "cgnative.hmacSha1B64(key, data)");
    key = arg_cstr(ctx, argv[0], &klen);
    if (!key) return JS_EXCEPTION;
    data = arg_cstr(ctx, argv[1], &dlen);
    if (!data) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    if (!HMAC(EVP_sha1(), key, (int)klen, (const unsigned char *)data, dlen, mac, &macLen)) {
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, data);
        return JS_ThrowInternalError(ctx, "cgnative: hmac failed");
    }
    memset(&out, 0, sizeof(out));
    b64_encode(mac, macLen, &out);
    r = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    buf_free(&out);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, data);
    return r;
}

/* randomHex(n) —— n 字节随机 -> 2n 个小写 hex */
static JSValue js_random_hex(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    int n = 16;
    unsigned char stackBuf[256];
    unsigned char *p = stackBuf;
    struct buf hex;
    JSValue r;
    (void)this_val;

    if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
    if (n <= 0) n = 16;
    if (n > 1024) n = 1024;
    if (n > (int)sizeof(stackBuf)) {
        p = (unsigned char *)malloc((size_t)n);
        if (!p) return JS_ThrowInternalError(ctx, "cgnative: out of memory");
    }
    if (RAND_bytes(p, n) != 1) {
        FILE *f = fopen("/dev/urandom", "rb");
        size_t got = 0;
        if (f) {
            got = fread(p, 1, (size_t)n, f);
            fclose(f);
        }
        if (got != (size_t)n) memset(p, 0, (size_t)n);
    }
    memset(&hex, 0, sizeof(hex));
    hex_encode(p, (size_t)n, &hex);
    r = JS_NewStringLen(ctx, hex.data ? hex.data : "", hex.len);
    buf_free(&hex);
    if (p != stackBuf) free(p);
    return r;
}

static JSValue js_utf8_hex(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *s;
    size_t n = 0;
    struct buf out;
    JSValue r;
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "cgnative.utf8Hex(str)");
    s = arg_cstr(ctx, argv[0], &n);
    if (!s) return JS_EXCEPTION;
    memset(&out, 0, sizeof(out));
    hex_encode((const unsigned char *)s, n, &out);
    r = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    buf_free(&out);
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue js_version(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char tmp[128];
    (void)this_val;
    (void)argc;
    (void)argv;
    snprintf(tmp, sizeof(tmp), "%s %s", CG_VERSION, CG_BUILD_DATE);
    return JS_NewString(ctx, tmp);
}

/* ---------------- HTTP ---------------- */
static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;

static void curl_bootstrap(void) { curl_global_init(CURL_GLOBAL_DEFAULT_C); }

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct buf *b = (struct buf *)userdata;
    size_t n = size * nmemb;
    if (buf_reserve(b, n) != 0) return 0;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static char *dup_str(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* headerLines："Key: v\r\nKey2: v2"（也接受 \n），空行/无冒号行丢弃 */
static CURL_slist *build_headers(const char *headersText) {
    CURL_slist *list = NULL;
    char *copy;
    char *save = NULL;
    char *line;
    if (!headersText || !headersText[0]) return NULL;
    copy = dup_str(headersText);
    if (!copy) return NULL;
    line = strtok_r(copy, "\n", &save);
    while (line) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ' || line[l - 1] == '\t')) {
            line[--l] = '\0';
        }
        while (*line == ' ' || *line == '\t') line++;
        if (*line && strchr(line, ':')) list = curl_slist_append(list, line);
        line = strtok_r(NULL, "\n", &save);
    }
    free(copy);
    return list;
}

struct http_resp {
    struct buf body;
    long status;
    int curlCode;
    char *errMsg;
};

static void http_resp_free(struct http_resp *r) {
    buf_free(&r->body);
    if (r->errMsg) free(r->errMsg);
    r->errMsg = NULL;
}

/* 单次 curl 往返。NOSIGNAL 必开（多线程下默认的 SIGALRM 会杀进程）。 */
static int http_perform(const char *method, const char *url, const char *headersText,
                        const char *body, size_t bodyLen, long timeoutMs,
                        struct http_resp *out) {
    CURL *h;
    CURL_slist *list;
    int code;
    int isGet;
    int isHead;

    memset(out, 0, sizeof(*out));
    pthread_once(&g_curl_once, curl_bootstrap);

    if (!url || !url[0]) {
        out->errMsg = dup_str("curl#: empty url");
        out->curlCode = -1;
        return -1;
    }
    isGet = (!method || str_ieq(method, "GET"));
    isHead = (method && str_ieq(method, "HEAD"));

    h = curl_easy_init();
    if (!h) {
        out->errMsg = dup_str("curl_easy_init failed");
        out->curlCode = -1;
        return -1;
    }
    list = build_headers(headersText);
    if (list) curl_easy_setopt(h, CURLOPT_HTTPHEADER, list);

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out->body);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeoutMs > 0 ? timeoutMs : 15000L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    /* 设备无 CA 库，运行时 jsapi 同样不校验 */
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);

    if (!isGet && !isHead) {
        /* 裸 body：POSTFIELDS + POSTFIELDSIZE 保证二进制安全长度，
         * 这是 jsapi.http.request 做不到的核心能力 */
        if (!method || str_ieq(method, "POST")) {
            curl_easy_setopt(h, CURLOPT_POST, 1L);
        } else {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
        }
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)bodyLen);
    } else if (isHead) {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "HEAD");
    }

    code = curl_easy_perform(h);
    out->curlCode = code;
    if (code == 0) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &out->status);
    } else {
        char tmp[160];
        const char *msg = curl_easy_strerror(code);
        snprintf(tmp, sizeof(tmp), "curl#%d: %s", code, msg ? msg : "unknown");
        out->errMsg = dup_str(tmp);
    }

    if (list) curl_slist_free_all(list);
    curl_easy_cleanup(h);
    return code == 0 ? 0 : -1;
}

/* 结果 -> JSON 字符串（契约：{status, body, error, curlCode}） */
static char *marshal_http(long status, int curlCode, const char *errMsg, struct buf *body,
                          long elapsedMs) {
    struct buf out;
    memset(&out, 0, sizeof(out));
    buf_adds(&out, "{\"status\":");
    buf_addi(&out, status);
    buf_adds(&out, ",\"curlCode\":");
    buf_addi(&out, curlCode);
    buf_adds(&out, ",\"ms\":");
    buf_addi(&out, elapsedMs);
    buf_adds(&out, ",\"error\":");
    if (errMsg) {
        buf_addjson(&out, errMsg, strlen(errMsg));
    } else if (status >= 400) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "http#%ld", status);
        buf_addjson(&out, tmp, strlen(tmp));
    } else {
        buf_adds(&out, "null");
    }
    buf_adds(&out, ",\"body\":");
    buf_addjson(&out, body->data ? body->data : "", body->len);
    buf_addc(&out, '}');
    return out.data;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

/* 同步请求：在调用线程（JS 线程）执行，返回 JSON 字符串 */
static char *do_request_json(const char *method, const char *url, const char *headers,
                             const char *body, size_t bodyLen, long timeoutMs) {
    struct http_resp r;
    long t0 = now_ms();
    char *json;
    http_perform(method, url, headers, body, bodyLen, timeoutMs, &r);
    json = marshal_http(r.status, r.curlCode, r.errMsg, &r.body, now_ms() - t0);
    http_resp_free(&r);
    return json;
}

/* requestSync(method, url, headerLines, body, timeoutMs) -> JSON 字符串 */
static JSValue js_request_sync(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    const char *method = "GET";
    const char *url = NULL;
    const char *headers = NULL;
    const char *body = NULL;
    size_t bodyLen = 0;
    int timeoutMs = 15000;
    char *json;
    JSValue r;
    (void)this_val;

    if (argc < 2) {
        return JS_ThrowTypeError(ctx,
            "cgnative.requestSync(method, url, headerLines, body, timeoutMs)");
    }
    method = JS_ToCString(ctx, argv[0]);
    if (!method) return JS_EXCEPTION;
    url = JS_ToCString(ctx, argv[1]);
    if (!url) {
        JS_FreeCString(ctx, method);
        return JS_EXCEPTION;
    }
    if (argc > 2) headers = arg_cstr(ctx, argv[2], NULL);
    if (argc > 3) body = arg_cstr(ctx, argv[3], &bodyLen);
    if (argc > 4) JS_ToInt32(ctx, &timeoutMs, argv[4]);
    if (timeoutMs <= 0) timeoutMs = 15000;

    json = do_request_json(method, url, headers, body, bodyLen, (long)timeoutMs);
    r = JS_NewString(ctx, json ? json : "{}");
    if (json) free(json);
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    if (headers) JS_FreeCString(ctx, headers);
    if (body) JS_FreeCString(ctx, body);
    return r;
}

/* postSync(url, headerLines, body, timeoutMs) -> requestSync("POST", ...) */
static JSValue js_post_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue args[5];
    JSValue r;
    JSValue m;
    int n = 0;
    (void)this_val;

    m = JS_NewString(ctx, "POST");
    args[n++] = m;
    args[n++] = argc > 0 ? argv[0] : JS_UNDEFINED;
    args[n++] = argc > 1 ? argv[1] : JS_UNDEFINED;
    args[n++] = argc > 2 ? argv[2] : JS_UNDEFINED;
    args[n++] = argc > 3 ? argv[3] : JS_UNDEFINED;
    r = js_request_sync(ctx, JS_UNDEFINED, n, args);
    JS_FreeValue(ctx, m);
    return r;
}

/* ---------------- 异步 HTTP：job 链表 + detached 线程 ---------------- */
struct job {
    int id;
    int state;     /* 0=进行中 1=完成 */
    int cancelled;
    char *method;
    char *url;
    char *headers;
    char *body;
    size_t bodyLen;
    long timeoutMs;
    char *result;
    struct job *next;
};

static struct job *g_jobs = NULL;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_next_id = 1;

static void job_free_fields(struct job *j) {
    if (j->method) free(j->method);
    if (j->url) free(j->url);
    if (j->headers) free(j->headers);
    if (j->body) free(j->body);
    if (j->result) free(j->result);
    j->method = j->url = j->headers = j->body = j->result = NULL;
}

static void *job_thread(void *arg) {
    struct job *j = (struct job *)arg;
    char *res = do_request_json(j->method, j->url, j->headers, j->body, j->bodyLen,
                                j->timeoutMs);
    pthread_mutex_lock(&g_mu);
    j->result = res;
    j->state = 1;
    if (j->cancelled) {
        struct job **pp = &g_jobs;
        while (*pp && *pp != j) pp = &(*pp)->next;
        if (*pp == j) *pp = j->next;
        pthread_mutex_unlock(&g_mu);
        job_free_fields(j);
        free(j);
        return NULL;
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* startRequest(method, url, headerLines, body, timeoutMs) -> id (>0) */
static JSValue js_start_request(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    const char *method = "POST";
    const char *url = NULL;
    const char *headers = NULL;
    const char *body = NULL;
    size_t bodyLen = 0;
    int timeoutMs = 15000;
    struct job *j;
    pthread_t th;
    int id;
    (void)this_val;

    if (argc < 2) {
        return JS_ThrowTypeError(ctx,
            "cgnative.startRequest(method, url, headerLines, body, timeoutMs)");
    }
    method = JS_ToCString(ctx, argv[0]);
    if (!method) return JS_EXCEPTION;
    url = JS_ToCString(ctx, argv[1]);
    if (!url) {
        JS_FreeCString(ctx, method);
        return JS_EXCEPTION;
    }
    if (argc > 2) headers = arg_cstr(ctx, argv[2], NULL);
    if (argc > 3) body = arg_cstr(ctx, argv[3], &bodyLen);
    if (argc > 4) JS_ToInt32(ctx, &timeoutMs, argv[4]);
    if (timeoutMs <= 0) timeoutMs = 15000;

    j = (struct job *)calloc(1, sizeof(struct job));
    if (!j) {
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, url);
        if (headers) JS_FreeCString(ctx, headers);
        if (body) JS_FreeCString(ctx, body);
        return JS_ThrowInternalError(ctx, "cgnative: out of memory");
    }
    j->method = dup_str(method);
    j->url = dup_str(url);
    j->headers = dup_str(headers ? headers : "");
    j->body = (char *)malloc(bodyLen + 1);
    if (j->body) {
        if (bodyLen) memcpy(j->body, body, bodyLen);
        j->body[bodyLen] = '\0';
    }
    j->bodyLen = bodyLen;
    j->timeoutMs = (long)timeoutMs;

    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    if (headers) JS_FreeCString(ctx, headers);
    if (body) JS_FreeCString(ctx, body);

    if (!j->method || !j->url || !j->headers || !j->body) {
        job_free_fields(j);
        free(j);
        return JS_ThrowInternalError(ctx, "cgnative: out of memory");
    }

    pthread_mutex_lock(&g_mu);
    id = g_next_id++;
    j->id = id;
    j->next = g_jobs;
    g_jobs = j;
    pthread_mutex_unlock(&g_mu);

    if (pthread_create(&th, NULL, job_thread, j) != 0) {
        struct job **pp;
        pthread_mutex_lock(&g_mu);
        pp = &g_jobs;
        while (*pp && *pp != j) pp = &(*pp)->next;
        if (*pp == j) *pp = j->next;
        pthread_mutex_unlock(&g_mu);
        job_free_fields(j);
        free(j);
        return JS_NewInt32(ctx, 0); /* 0 = 失败（契约：返回 int id） */
    }
    pthread_detach(th);
    return JS_NewInt32(ctx, id);
}

/* startPost(url, headerLines, body, timeoutMs) —— 旧契约别名 */
static JSValue js_start_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue args[5];
    JSValue m;
    JSValue r;
    int n = 0;
    (void)this_val;
    m = JS_NewString(ctx, "POST");
    args[n++] = m;
    args[n++] = argc > 0 ? argv[0] : JS_UNDEFINED;
    args[n++] = argc > 1 ? argv[1] : JS_UNDEFINED;
    args[n++] = argc > 2 ? argv[2] : JS_UNDEFINED;
    args[n++] = argc > 3 ? argv[3] : JS_UNDEFINED;
    r = js_start_request(ctx, JS_UNDEFINED, n, args);
    JS_FreeValue(ctx, m);
    return r;
}

/* takeResult(id) -> null（未完成）或 JSON 字符串（完成，取走后删除） */
static JSValue js_take_result(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    int id = 0;
    struct job *j = NULL;
    struct job **pp = NULL;
    JSValue res;
    (void)this_val;

    if (argc < 1) return JS_NULL;
    if (JS_ToInt32(ctx, &id, argv[0]) != 0) return JS_NULL;
    if (id <= 0) return JS_NULL;

    pthread_mutex_lock(&g_mu);
    pp = &g_jobs;
    while (*pp && (*pp)->id != id) pp = &(*pp)->next;
    j = *pp;
    if (!j || j->state != 1) {
        pthread_mutex_unlock(&g_mu);
        return JS_NULL;
    }
    *pp = j->next;
    pthread_mutex_unlock(&g_mu);

    res = JS_NewString(ctx, j->result ? j->result : "{}");
    job_free_fields(j);
    free(j);
    return res;
}

/* cancelAll()：丢弃全部在途/已完成请求（页面销毁时调） */
static JSValue js_cancel_all(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    struct job **pp;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)ctx;
    pthread_mutex_lock(&g_mu);
    pp = &g_jobs;
    while (*pp) {
        struct job *j = *pp;
        if (j->state == 1) {
            *pp = j->next;
            pthread_mutex_unlock(&g_mu);
            job_free_fields(j);
            free(j);
            pthread_mutex_lock(&g_mu);
            pp = &g_jobs; /* 链表头可能被动过，重扫 */
        } else {
            j->cancelled = 1;
            pp = &j->next;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return JS_UNDEFINED;
}

/* ================================================================
 * WebSocket over TLS（RFC6455 客户端）—— 播放信令通道
 *
 * 平台 JS 侧没有 WebSocket，设备 libcurl 7.79.1 也不支持 ws://，
 * 所以这里用 OpenSSL 直接做 TCP + TLS + HTTP Upgrade + 帧编解码。
 *
 * 线程模型：wsOpen 立即返回 id，后台线程负责 connect/TLS/Upgrade，
 * 成功后同一线程转为收帧循环（read → 解帧 → 入队）。
 * 所有共享状态由 w->mu 保护；JS 线程只做 wsState/wsPoll/wsSend。
 * ================================================================ */

#define WS_OP_CONT  0x0
#define WS_OP_TEXT  0x1
#define WS_OP_BIN   0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

#define WS_MAX_MSG   (8 * 1024 * 1024) /* 单条消息上限，防对端打爆内存 */

struct ws_msg {
    char *text;
    size_t len;
    struct ws_msg *next;
};

/* 待发帧队列：JS 线程只入队，唯一的 I/O 线程取出并写 socket。
 * 这样 SSL* 全程只有一个线程碰（OpenSSL 明确要求一个 SSL 对象不可并发使用）。 */
struct ws_out {
    unsigned char *data;
    size_t len;
    struct ws_out *next;
};

struct ws_conn {
    int id;
    int state;         /* 0=connecting 1=open 2=closed 3=error */
    char *msg;         /* 状态描述 / 错误原因 */
    char *url;
    char *headers;

    int fd;
    SSL_CTX *sslCtx;
    SSL *ssl;

    /* --- 握手阶段诊断（播放页按这个显示不同文案，见 wsState 的 phase/stageCode）--- */
    int phase;          /* 0=dns 1=tcp 2=tls 3=upgrade 4=open 5=closed */
    int stageCode;      /* 失败/结束阶段码：
                         *   0 未开始
                         *   1 DNS 解析失败        (phase=dns)
                         *   2 TCP 连接失败        (phase=tcp)
                         *   3 TLS 握手失败        (phase=tls)
                         *   4 HTTP 非 101        (phase=upgrade)
                         *   5 升级响应解析失败     (phase=upgrade)
                         *   6 已建立              (phase=open)
                         *   7 对端 close 帧关闭    (phase=closed)
                         *   8 本地 wsClose 关闭    (phase=closed)
                         *   9 I/O 错误导致断开     (phase=closed)
                         *  10 URL 非法            (phase=dns)
                         *  11 握手超时            (phase 见当前值)
                         *  12 TCP 连接超时        (phase=tcp；与 2 的区别：
                         *     errno=ECONNREFUSED 是被拒(网关机位过期/被封)，
                         *     12 是网络不通/丢包 —— 播放页文案不同) */
    int httpStatus;     /* upgrade 阶段拿到的 HTTP 状态码，拿不到给 0 */
    int closeCode;      /* 服务端 close 帧的 code */
    char *closeReason;  /* 服务端 close 帧的 reason */
    int lastErrno;      /* 最后一次系统调用错误 */
    char *lastErrstr;

    /* 每次连接的选项（wsOpen 第 4 参数 optsJson，全部向后兼容默认值） */
    int tlsVerify;         /* 默认 0：设备无 CA 库，必须不校验 */
    int connectTimeoutMs;  /* 默认 15000 */
    long frameMaxBytes;    /* 默认 WS_MAX_MSG */

    int wantClose;
    int polling;   /* 进入收帧循环后置 1：读超时不再视为断开，而是回主循环 flush */
    pthread_t th;
    int thStarted;

    long recvBytes;
    long sentBytes;
    long frameTotal;   /* 累计收到的完整数据帧数 */
    long lastRecvMs;   /* 最后一次成功读到字节的时间（心跳判断） */

    /* 收到的文本消息队列 */
    struct ws_msg *head;
    struct ws_msg *tail;

    /* 待发帧队列（JS 线程入队，I/O 线程出队） */
    struct ws_out *outHead;
    struct ws_out *outTail;

    /* 帧级自证（wsLastFrame）：最后一次组帧产物的前 64 字节 + 整帧长度。
     * 存在理由：网关只回 400 format invalid 时无法判断是帧格式错还是业务
     * 字段错，把真实字节吐给 JS 侧肉眼核对 81 8x <len> <mask4> <payload>。 */
    unsigned char lastFrame[64];
    size_t lastFrameHead;   /* 实际记录的前缀字节数 */
    long lastFrameLen;      /* 整帧总字节数 */
    int lastFrameOp;

    pthread_mutex_t mu;  /* 保护 state/msg/队列/计数。
                          * 注意：这里只需要 mu —— socket 上所有读写都由 I/O
                          * 线程独占（JS 线程只入队），所以不存在两个线程同时
                          * SSL_write 的 TLS 记录交错问题，不需要额外写锁。 */
    struct ws_conn *next;
};

static struct ws_conn *g_ws = NULL;
static pthread_mutex_t g_wsMu = PTHREAD_MUTEX_INITIALIZER;
static int g_wsNextId = 1;
static pthread_once_t g_ssl_once = PTHREAD_ONCE_INIT;

static void ssl_bootstrap(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

static void ws_set_state(struct ws_conn *w, int state, const char *msg) {
    pthread_mutex_lock(&w->mu);
    w->state = state;
    if (msg) {
        if (w->msg) free(w->msg);
        w->msg = dup_str(msg);
    }
    pthread_mutex_unlock(&w->mu);
}

/* 阶段推进（不改 state）。phase/stageCode 语义见 struct ws_conn 注释。 */
static void ws_set_phase(struct ws_conn *w, int phase, int stageCode) {
    pthread_mutex_lock(&w->mu);
    w->phase = phase;
    w->stageCode = stageCode;
    pthread_mutex_unlock(&w->mu);
}

/* 记录最后一次失败的系统调用，供播放页区分「连不上」和「握手被拒」 */
static void ws_set_last_err(struct ws_conn *w, int err, const char *str) {
    pthread_mutex_lock(&w->mu);
    w->lastErrno = err;
    if (w->lastErrstr) {
        free(w->lastErrstr);
        w->lastErrstr = NULL;
    }
    if (str) w->lastErrstr = dup_str(str);
    pthread_mutex_unlock(&w->mu);
}

/* 记录 upgrade 阶段拿到的 HTTP 状态码（拿不到给 0） */
static void ws_set_http_status(struct ws_conn *w, int status) {
    pthread_mutex_lock(&w->mu);
    w->httpStatus = status;
    pthread_mutex_unlock(&w->mu);
}

/* 记录服务端 close 帧的 code/reason */
static void ws_set_close_info(struct ws_conn *w, int code, const char *reason, size_t rlen) {
    pthread_mutex_lock(&w->mu);
    w->closeCode = code;
    if (w->closeReason) {
        free(w->closeReason);
        w->closeReason = NULL;
    }
    if (reason && rlen) {
        char *s = (char *)malloc(rlen + 1);
        if (s) {
            memcpy(s, reason, rlen);
            s[rlen] = '\0';
            w->closeReason = s;
        }
    }
    pthread_mutex_unlock(&w->mu);
}

static void ws_push_text(struct ws_conn *w, const char *s, size_t n) {
    struct ws_msg *m;
    if (n > (size_t)(w->frameMaxBytes > 0 ? w->frameMaxBytes : WS_MAX_MSG)) return;
    m = (struct ws_msg *)calloc(1, sizeof(struct ws_msg));
    if (!m) return;
    m->text = (char *)malloc(n + 1);
    if (!m->text) {
        free(m);
        return;
    }
    if (n) memcpy(m->text, s, n);
    m->text[n] = '\0';
    m->len = n;
    pthread_mutex_lock(&w->mu);
    if (w->tail) w->tail->next = m;
    else w->head = m;
    w->tail = m;
    pthread_mutex_unlock(&w->mu);
}

/* 发送原始字节。**只允许收帧线程调用**（见下方 ws_out 队列说明）。 */
static int ws_raw_send(struct ws_conn *w, const unsigned char *data, size_t len) {
    size_t off = 0;
    if (!w->ssl) return -1;
    while (off < len) {
        int n = SSL_write(w->ssl, data + off, (int)(len - off));
        if (n <= 0) {
            int e = SSL_get_error(w->ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* 前向声明：组帧定义在下面 */
static int ws_build_frame(int opcode, const unsigned char *payload, size_t len,
                          struct buf *out);

/* JS 线程调用：把一帧排进发件队列。真正写 socket 的是 I/O 线程。
 * 返回 0 成功入队；-1 连接已不可用 / 队列满。 */
static int ws_send_frame(struct ws_conn *w, int opcode, const unsigned char *payload,
                         size_t len) {
    struct buf frame;
    struct ws_out *o;
    int queued = 0;

    if (!w) return -1;

    memset(&frame, 0, sizeof(frame));
    if (ws_build_frame(opcode, payload, len, &frame) != 0) {
        buf_free(&frame);
        return -1;
    }
    o = (struct ws_out *)calloc(1, sizeof(struct ws_out));
    if (!o) {
        buf_free(&frame);
        return -1;
    }
    o->data = (unsigned char *)frame.data;
    o->len = frame.len;

    pthread_mutex_lock(&w->mu);
    /* 帧级自证：记下真实发出的字节前缀（与入队是否成功无关地记录） */
    w->lastFrameOp = opcode;
    w->lastFrameLen = (long)frame.len;
    w->lastFrameHead = frame.len < sizeof(w->lastFrame) ? frame.len : sizeof(w->lastFrame);
    if (w->lastFrameHead) memcpy(w->lastFrame, frame.data, w->lastFrameHead);
    if (w->state == 1 || w->state == 0) {
        if (w->outTail) w->outTail->next = o;
        else w->outHead = o;
        w->outTail = o;
        queued = 1;
    }
    pthread_mutex_unlock(&w->mu);

    if (!queued) {
        free(o->data);
        free(o);
        return -1;
    }
    /* 不需要在这里唤醒 I/O 线程：ws_pump 的 poll 超时只有 100ms，
     * 每轮循环开头都会 ws_flush_out，出站延迟 <= 100ms，足以满足播放/鉴权。 */
    return 0;
}

/* I/O 线程调用：把队列里的帧全部写出去 */
static int ws_flush_out(struct ws_conn *w) {
    struct ws_out *o;
    int rc = 0;

    for (;;) {
        pthread_mutex_lock(&w->mu);
        o = w->outHead;
        if (o) {
            w->outHead = o->next;
            if (!w->outHead) w->outTail = NULL;
        }
        pthread_mutex_unlock(&w->mu);
        if (!o) break;

        if (ws_raw_send(w, o->data, o->len) != 0) rc = -1;
        else {
            pthread_mutex_lock(&w->mu);
            w->sentBytes += (long)o->len;
            pthread_mutex_unlock(&w->mu);
        }
        free(o->data);
        free(o);
        if (rc != 0) break;
    }
    return rc;
}

static void ws_drop_out_queue(struct ws_conn *w) {
    struct ws_out *o;
    pthread_mutex_lock(&w->mu);
    o = w->outHead;
    w->outHead = NULL;
    w->outTail = NULL;
    pthread_mutex_unlock(&w->mu);
    while (o) {
        struct ws_out *nx = o->next;
        free(o->data);
        free(o);
        o = nx;
    }
}

/* 组帧（客户端必须加掩码）。opcode 1/2/8/9/10 */
static int ws_build_frame(int opcode, const unsigned char *payload, size_t len,
                          struct buf *out) {
    unsigned char hdr[14];
    unsigned char mask[4];
    size_t hlen = 0;
    size_t i;

    if (RAND_bytes(mask, 4) != 1) {
        long t = now_ms();
        mask[0] = (unsigned char)(t & 0xFF);
        mask[1] = (unsigned char)((t >> 8) & 0xFF);
        mask[2] = 0x5A;
        mask[3] = 0xA5;
    }

    hdr[hlen++] = (unsigned char)(0x80 | (opcode & 0x0F)); /* FIN=1 */
    if (len < 126) {
        hdr[hlen++] = (unsigned char)(0x80 | (unsigned char)len);
    } else if (len <= 0xFFFF) {
        hdr[hlen++] = 0x80 | 126;
        hdr[hlen++] = (unsigned char)((len >> 8) & 0xFF);
        hdr[hlen++] = (unsigned char)(len & 0xFF);
    } else {
        int i2;
        hdr[hlen++] = 0x80 | 127;
        for (i2 = 7; i2 >= 0; i2--) hdr[hlen++] = (unsigned char)((len >> (i2 * 8)) & 0xFF);
    }
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;

    buf_add(out, hdr, hlen);
    for (i = 0; i < len; i++) {
        char c = (char)(payload[i] ^ mask[i & 3]);
        buf_add(out, &c, 1);
    }
    return 0;
}

/* 从 SSL 精确读 n 字节；返回 0 成功，-1 失败/断开 */
static int ws_read_exact(struct ws_conn *w, unsigned char *dst, size_t n) {
    size_t off = 0;
    while (off < n) {
        int r = SSL_read(w->ssl, dst + off, (int)(n - off));
        if (r <= 0) {
            int e = SSL_get_error(w->ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

/* 解析 URL：wss://host:port/path 或 ws://host:port/path */
static int ws_parse_url(const char *url, char *host, size_t hostCap, int *port, int *tls,
                        char *path, size_t pathCap) {
    const char *p = url;
    const char *hp;
    const char *slash;
    const char *colon;
    size_t n;

    if (!url) return -1;
    if (strncmp(p, "wss://", 6) == 0) {
        *tls = 1;
        p += 6;
        *port = 443;
    } else if (strncmp(p, "ws://", 5) == 0) {
        *tls = 0;
        p += 5;
        *port = 80;
    } else {
        return -1;
    }
    hp = p;
    slash = strchr(p, '/');
    n = slash ? (size_t)(slash - hp) : strlen(hp);
    if (n == 0 || n >= hostCap) return -1;
    memcpy(host, hp, n);
    host[n] = '\0';

    colon = strrchr(host, ':');
    if (colon) {
        *port = atoi(colon + 1);
        if (*port <= 0) *port = *tls ? 443 : 80;
        ((char *)colon)[0] = '\0';
    }
    if (slash) {
        snprintf(path, pathCap, "%s", slash);
    } else {
        snprintf(path, pathCap, "/");
    }
    return 0;
}

/* 连 TCP。*stage 回填失败细分：
 *   0 成功 / 1 DNS 解析失败 / 2 socket() 失败 / 3 connect 失败或超时 */
static int ws_tcp_connect(const char *host, int port, int timeoutMs, int *stage,
                          int *errNum) {
    struct hostent *he;
    int fd = -1;
    int rc;
    struct sockaddr_in addr;
    struct timeval tv;

    if (stage) *stage = 0;
    if (errNum) *errNum = 0;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        he = gethostbyname(host); /* 设备无 getaddrinfo 保证，gethostbyname 足够 */
        if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
            if (stage) *stage = 1;
            if (errNum) *errNum = h_errno;
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (stage) *stage = 2;
        if (errNum) *errNum = errno;
        return -1;
    }
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 非阻塞 connect + poll：区分「被拒绝(ECONNREFUSED)」和「网络不通/超时
     * (ETIMEDOUT)」。播放页对这两种要给不同文案 —— 前者是网关机位过期或被封
     * 可以直接换台，后者是网络问题要重试。阻塞 connect 超时只会拿到
     * EINPROGRESS，对上层毫无信息量。 */
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pfd;
            int prc;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            prc = poll(&pfd, 1, timeoutMs);
            if (prc == 0) {
                if (errNum) *errNum = ETIMEDOUT;
                if (stage) *stage = 4; /* 4 = 连接超时（区别于 3 = 被拒绝） */
                close(fd);
                return -1;
            } else if (prc < 0) {
                int e = (errno == EINTR) ? ETIMEDOUT : errno;
                if (errNum) *errNum = e;
                if (stage) *stage = 4;
                close(fd);
                return -1;
            } else {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0) soerr = errno;
                if (soerr != 0) {
                    if (errNum) *errNum = soerr;
                    if (stage) *stage = 3;
                    close(fd);
                    return -1;
                }
                rc = 0; /* 连上了 */
            }
        }
        if (flags >= 0) fcntl(fd, F_SETFL, flags); /* 恢复阻塞，后续 SSL 用阻塞语义 */
    }

    if (rc != 0) {
        if (errNum) *errNum = errno;
        if (stage) *stage = 3;
        close(fd);
        return -1;
    }
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

/* 握手完成后把读超时调短：收帧循环里靠 poll 决定是否读，这里的超时只是
 * 「半帧卡住」的兜底，不能太长否则发件队列会被一起卡住。 */
static void ws_set_short_recv_timeout(int fd, int ms) {
    struct timeval tv;
    if (fd < 0) return;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* 等 fd 可读，最多 waitMs。返回 1 可读 / 0 超时 / -1 出错 */
static int ws_wait_readable(int fd, int waitMs) {
    struct pollfd pfd;
    int rc;
    if (fd < 0) return -1;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    rc = poll(&pfd, 1, waitMs);
    if (rc == 0) return 0;
    if (rc < 0) return (errno == EINTR) ? 0 : -1;
    return 1;
}

/* 读 HTTP 响应头直到 \r\n\r\n；返回状态码（-1 失败） */
static int ws_read_http_headers(struct ws_conn *w, struct buf *out) {
    char c;
    int guard = 0;
    for (;;) {
        if (ws_read_exact(w, (unsigned char *)&c, 1) != 0) return -1;
        buf_add(out, &c, 1);
        if (out->len > 32 * 1024) return -1; /* 响应头过大，判定异常 */
        if (out->len >= 4 && memcmp(out->data + out->len - 4, "\r\n\r\n", 4) == 0) break;
        if (++guard < 0) return -1;
    }
    if (strncmp(out->data, "HTTP/1.", 7) == 0) {
        const char *sp = strchr(out->data, ' ');
        if (sp) return atoi(sp + 1);
    }
    return -1;
}

/* 收帧循环：读完一个完整消息（含分片）再入队 */
/* 收帧主循环，**只有 I/O 线程执行**。
 * 每轮先 flush 发件队列，再用 poll 短等可读：这样 JS 线程 wsSend 入队的帧
 * 最迟 100ms 内就发出去，不必等对端推数据。 */
static void ws_pump(struct ws_conn *w) {
    struct buf frag;       /* continuation 累积 */
    int fragOpcode = 0;
    int running = 1;
    int rc = 0;

    memset(&frag, 0, sizeof(frag));

    while (running) {
        unsigned char h[2];
        int fin, opcode, masked, i;
        unsigned long long plen = 0;
        unsigned char mask[4];
        unsigned char *payload = NULL;
        int wait;

        /* 1) 把 JS 线程排队的帧写出去 */
        if (ws_flush_out(w) != 0) {
            ws_set_last_err(w, errno, "flush failed");
            ws_set_phase(w, 5, 9);
            break;
        }

        /* 2) 本地要求关闭（wsClose 已把 state 置 2，这里直接组帧发包，
         *    不能走 ws_send_frame —— 它只接受 state 0/1，会拒绝入队） */
        pthread_mutex_lock(&w->mu);
        if (w->wantClose) {
            pthread_mutex_unlock(&w->mu);
            {
                struct buf cf;
                unsigned char cc[2];
                memset(&cf, 0, sizeof(cf));
                cc[0] = 0x03; /* close code 1000 = 正常关闭 */
                cc[1] = 0xE8;
                if (ws_build_frame(WS_OP_CLOSE, cc, 2, &cf) == 0 && cf.len)
                    (void)ws_raw_send(w, (const unsigned char *)cf.data, cf.len);
                buf_free(&cf);
            }
            ws_set_close_info(w, 1000, "local close", 11);
            ws_set_phase(w, 5, 8);
            break;
        }
        pthread_mutex_unlock(&w->mu);

        /* 3) 短等可读（100ms），超时就回去 flush 队列 */
        wait = ws_wait_readable(w->fd, 100);
        if (wait == 0) continue;
        if (wait < 0) {
            ws_set_last_err(w, errno, "poll failed");
            ws_set_phase(w, 5, 9);
            break;
        }

        if (ws_read_exact(w, h, 2) != 0) {
            ws_set_last_err(w, errno, "peer closed (eof/reset)");
            ws_set_phase(w, 5, 9);
            break;
        }
        fin = (h[0] & 0x80) ? 1 : 0;
        opcode = h[0] & 0x0F;
        masked = (h[1] & 0x80) ? 1 : 0;
        plen = h[1] & 0x7F;
        if (plen == 126) {
            unsigned char e[2];
            if (ws_read_exact(w, e, 2) != 0) {
                ws_set_phase(w, 5, 9);
                break;
            }
            plen = ((unsigned long long)e[0] << 8) | e[1];
        } else if (plen == 127) {
            unsigned char e[8];
            if (ws_read_exact(w, e, 8) != 0) {
                ws_set_phase(w, 5, 9);
                break;
            }
            for (i = 0; i < 8; i++) plen = (plen << 8) | e[i];
        }
        if (masked && ws_read_exact(w, mask, 4) != 0) {
            ws_set_phase(w, 5, 9);
            break;
        }
        if (plen > (unsigned long long)(w->frameMaxBytes > 0 ? w->frameMaxBytes : WS_MAX_MSG)) {
            ws_set_last_err(w, 0, "frame too large");
            ws_set_phase(w, 5, 9);
            break;
        }

        if (plen > 0) {
            payload = (unsigned char *)malloc((size_t)plen);
            if (!payload) break;
            if (ws_read_exact(w, payload, (size_t)plen) != 0) {
                free(payload);
                ws_set_phase(w, 5, 9);
                break;
            }
            if (masked) {
                size_t k;
                for (k = 0; k < (size_t)plen; k++) payload[k] ^= mask[k & 3];
            }
        }

        pthread_mutex_lock(&w->mu);
        w->recvBytes += (long)plen;
        w->lastRecvMs = now_ms();
        pthread_mutex_unlock(&w->mu);

        switch (opcode) {
            case WS_OP_PING:
                /* 立即回 pong（I/O 线程直接写，最短延迟） */
                {
                    struct buf pf;
                    memset(&pf, 0, sizeof(pf));
                    if (ws_build_frame(WS_OP_PONG, payload, (size_t)plen, &pf) == 0 && pf.len)
                        (void)ws_raw_send(w, (const unsigned char *)pf.data, pf.len);
                    buf_free(&pf);
                }
                break;
            case WS_OP_PONG:
                break;
            case WS_OP_CLOSE:
                /* 服务端主动关闭：记录 code/reason 供播放页区分「被踢」 */
                {
                    int code = 0;
                    struct buf cf;
                    if (plen >= 2 && payload)
                        code = ((int)payload[0] << 8) | (int)payload[1];
                    ws_set_close_info(w, code,
                                      (plen > 2 && payload) ? (const char *)(payload + 2) : NULL,
                                      (plen > 2) ? (size_t)(plen - 2) : 0);
                    ws_set_phase(w, 5, 7);
                    /* 回显 close 帧 */
                    memset(&cf, 0, sizeof(cf));
                    if (ws_build_frame(WS_OP_CLOSE, payload, (size_t)plen, &cf) == 0 && cf.len)
                        (void)ws_raw_send(w, (const unsigned char *)cf.data, cf.len);
                    buf_free(&cf);
                }
                running = 0;
                break;
            case WS_OP_TEXT:
            case WS_OP_BIN:
                pthread_mutex_lock(&w->mu);
                w->frameTotal++;
                pthread_mutex_unlock(&w->mu);
                if (fin) {
                    ws_push_text(w, (const char *)(payload ? payload : (unsigned char *)""),
                                 (size_t)plen);
                    buf_free(&frag);
                    fragOpcode = 0;
                } else {
                    buf_free(&frag);
                    if (plen) buf_add(&frag, payload, (size_t)plen);
                    fragOpcode = opcode;
                }
                break;
            case WS_OP_CONT:
                if (plen) buf_add(&frag, payload, (size_t)plen);
                if (fin) {
                    pthread_mutex_lock(&w->mu);
                    w->frameTotal++;
                    pthread_mutex_unlock(&w->mu);
                    ws_push_text(w, frag.data ? frag.data : "", frag.len);
                    buf_free(&frag);
                    fragOpcode = 0;
                }
                break;
            default:
                break;
        }
        (void)fragOpcode;
        if (payload) free(payload);
        if (!running) break;
    }
    buf_free(&frag);
    (void)rc;
}

static void ws_teardown(struct ws_conn *w) {
    if (w->ssl) {
        SSL_shutdown(w->ssl);
        SSL_free(w->ssl);
        w->ssl = NULL;
    }
    if (w->sslCtx) {
        SSL_CTX_free(w->sslCtx);
        w->sslCtx = NULL;
    }
    if (w->fd >= 0) {
        close(w->fd);
        w->fd = -1;
    }
}

static void *ws_thread(void *arg) {
    struct ws_conn *w = (struct ws_conn *)arg;
    char host[256];
    char path[1024];
    int port = 443, tls = 1;
    int timeoutMs = 15000;
    struct buf req;
    struct buf resp;
    int status;
    unsigned char rnd[16];
    struct buf keyB64;
    char errbuf[128];

    timeoutMs = w->connectTimeoutMs > 0 ? w->connectTimeoutMs : 15000;

    if (ws_parse_url(w->url, host, sizeof(host), &port, &tls, path, sizeof(path)) != 0) {
        ws_set_phase(w, 0, 10);
        ws_set_state(w, 3, "bad url (expect wss://host:port/path)");
        return NULL;
    }

    /* --- TCP（区分 DNS 失败 / 连不上）--- */
    {
        int st = 0, en = 0;
        ws_set_phase(w, 0, 0);
        w->fd = ws_tcp_connect(host, port, timeoutMs, &st, &en);
        if (w->fd < 0) {
            char eb[160];
            if (st == 1) {
                /* h_errno 是个"类别"数，不是 errno，直接把类别翻成人话更有用 */
                const char *ht = "dns: unknown error";
                if (en == HOST_NOT_FOUND) ht = "dns: host not found";
                else if (en == TRY_AGAIN) ht = "dns: temporary failure, try again";
                else if (en == NO_RECOVERY) ht = "dns: unrecoverable error";
                else if (en == NO_DATA) ht = "dns: no address for host";
                snprintf(eb, sizeof(eb), "dns resolve failed (%s) h_errno=%d", host, en);
                ws_set_last_err(w, en, ht);
                ws_set_phase(w, 0, 1);
            } else if (st == 2) {
                snprintf(eb, sizeof(eb), "socket() failed errno=%d", en);
                ws_set_last_err(w, en, "socket failed");
                ws_set_phase(w, 1, 2);
            } else if (st == 4) {
                /* 连接超时：网络不通/被丢包，和「被拒绝」要区分开 */
                snprintf(eb, sizeof(eb), "tcp connect timeout %s:%d (%dms)", host, port, timeoutMs);
                ws_set_last_err(w, en ? en : ETIMEDOUT, "connect timeout");
                ws_set_phase(w, 1, 12);
            } else {
                snprintf(eb, sizeof(eb), "tcp connect failed %s:%d errno=%d", host, port, en);
                ws_set_last_err(w, en, "connect failed");
                ws_set_phase(w, 1, 2);
            }
            ws_set_state(w, 3, eb);
            return NULL;
        }
        ws_set_phase(w, 1, 0);
    }

    pthread_once(&g_ssl_once, ssl_bootstrap);

    if (tls) {
        ws_set_phase(w, 2, 0);
        w->sslCtx = SSL_CTX_new(TLS_client_method());
        if (!w->sslCtx) {
            ws_set_state(w, 3, "SSL_CTX_new failed");
            ws_set_phase(w, 2, 3);
            ws_teardown(w);
            return NULL;
        }
        /* 设备无 CA 库：默认不校验（tlsVerify 默认 0）。
         * 显式 optsJson {"tlsVerify":1} 时才打开校验，用于诊断。 */
        SSL_CTX_set_verify(w->sslCtx,
                           w->tlsVerify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
        if (w->tlsVerify) SSL_CTX_set_default_verify_paths(w->sslCtx);
        w->ssl = SSL_new(w->sslCtx);
        if (!w->ssl) {
            ws_set_state(w, 3, "SSL_new failed");
            ws_set_phase(w, 2, 3);
            ws_teardown(w);
            return NULL;
        }
        SSL_set_fd(w->ssl, w->fd);
        /* SNI：网关按 Host 选证书时必须带，否则部分节点直接断链 */
        SSL_set_tlsext_host_name(w->ssl, host);
        if (SSL_connect(w->ssl) != 1) {
            unsigned long e = ERR_get_error();
            snprintf(errbuf, sizeof(errbuf), "SSL_connect failed (%s)",
                     e ? ERR_error_string(e, NULL) : "unknown");
            ws_set_last_err(w, errno, e ? ERR_error_string(e, NULL) : "SSL_connect failed");
            ws_set_phase(w, 2, 3);
            ws_set_state(w, 3, errbuf);
            ws_teardown(w);
            return NULL;
        }
    }

    /* --- HTTP Upgrade --- */
    ws_set_phase(w, 3, 0);
    memset(&req, 0, sizeof(req));
    if (RAND_bytes(rnd, 16) != 1) {
        int i;
        for (i = 0; i < 16; i++) rnd[i] = (unsigned char)((now_ms() >> (i % 8)) & 0xFF);
    }
    memset(&keyB64, 0, sizeof(keyB64));
    b64_encode(rnd, 16, &keyB64);

    buf_adds(&req, "GET ");
    buf_adds(&req, path);
    buf_adds(&req, " HTTP/1.1\r\nHost: ");
    buf_adds(&req, host);
    if (port != 443 && port != 80) {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), ":%d", port);
        buf_adds(&req, pbuf);
    }
    buf_adds(&req, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n");
    buf_adds(&req, "Sec-WebSocket-Key: ");
    buf_add(&req, keyB64.data ? keyB64.data : "", keyB64.len);
    buf_adds(&req, "\r\nSec-WebSocket-Version: 13\r\n");
    {
        /* 额外 headers：逐行追加（已去掉 \r） */
        const char *ht = w->headers;
        if (ht && ht[0]) {
            char *copy = dup_str(ht);
            if (copy) {
                char *save = NULL;
                char *line = strtok_r(copy, "\n", &save);
                while (line) {
                    size_t l = strlen(line);
                    while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
                    while (*line == ' ' || *line == '\t') line++;
                    if (*line) {
                        buf_adds(&req, line);
                        buf_adds(&req, "\r\n");
                    }
                    line = strtok_r(NULL, "\n", &save);
                }
                free(copy);
            }
        }
    }
    buf_adds(&req, "\r\n");
    buf_free(&keyB64);

    if (w->ssl) {
        if (ws_raw_send(w, (const unsigned char *)req.data, req.len) != 0) {
            ws_set_last_err(w, errno, "upgrade send failed");
            ws_set_phase(w, 3, 9);
            ws_set_state(w, 3, "upgrade send failed");
            buf_free(&req);
            ws_teardown(w);
            return NULL;
        }
    } else {
        size_t off = 0;
        while (off < req.len) {
            ssize_t n = send(w->fd, req.data + off, req.len - off, 0);
            if (n <= 0) break;
            off += (size_t)n;
        }
        if (off != req.len) {
            ws_set_last_err(w, errno, "upgrade send failed");
            ws_set_phase(w, 3, 9);
            ws_set_state(w, 3, "upgrade send failed");
            buf_free(&req);
            ws_teardown(w);
            return NULL;
        }
    }
    buf_free(&req);

    memset(&resp, 0, sizeof(resp));
    status = ws_read_http_headers(w, &resp);
    /* 无论 101 还是 502，都记下拿到的状态码，播放页要按 502 决定重试 */
    ws_set_http_status(w, status > 0 ? status : 0);
    if (status != 101) {
        snprintf(errbuf, sizeof(errbuf), "upgrade rejected: status=%d", status);
        ws_set_last_err(w, 0, "upgrade rejected");
        ws_set_phase(w, 3, status > 0 ? 4 : 5);
        ws_set_state(w, 3, errbuf);
        buf_free(&resp);
        ws_teardown(w);
        return NULL;
    }
    buf_free(&resp);

    ws_set_phase(w, 4, 6);
    ws_set_state(w, 1, "open");
    /* 握手用长超时；收帧循环改用 poll(100ms) 驱动，读超时缩短兜底半帧 */
    ws_set_short_recv_timeout(w->fd, 3000);

    /* --- 收帧循环 --- */
    ws_pump(w);

    /* ws_pump 已在正常路径记录阶段；这里兜底 */
    pthread_mutex_lock(&w->mu);
    if (w->phase != 5) {
        pthread_mutex_unlock(&w->mu);
        ws_set_phase(w, 5, 9);
    } else {
        pthread_mutex_unlock(&w->mu);
    }
    ws_set_state(w, 2, "closed");
    ws_teardown(w);
    return NULL;
}

/* wsOpen(url, headers, timeoutMs, optsJson) -> id (>0)
 * optsJson（可选）: {"tlsVerify":0|1,"connectTimeoutMs":N,"frameMaxBytes":N}
 * 缺省时保持旧行为：不校验证书、超时 15000、帧上限 WS_MAX_MSG。 */
static JSValue js_ws_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url;
    const char *headers = NULL;
    struct ws_conn *w;
    int id;
    int32_t timeoutMs = 15000;
    (void)this_val;

    if (argc < 1) return JS_ThrowTypeError(ctx, "cgnative.wsOpen(url, headers, timeoutMs, optsJson)");
    url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    if (argc > 1) headers = arg_cstr(ctx, argv[1], NULL);
    if (argc > 2 && JS_IsNumber(argv[2])) JS_ToInt32(ctx, &timeoutMs, argv[2]);

    w = (struct ws_conn *)calloc(1, sizeof(struct ws_conn));
    if (!w) {
        JS_FreeCString(ctx, url);
        if (headers) JS_FreeCString(ctx, headers);
        return JS_NewInt32(ctx, 0);
    }
    w->url = dup_str(url);
    w->headers = dup_str(headers ? headers : "");
    w->fd = -1;
    w->state = 0;
    w->msg = dup_str("connecting");

    /* 默认值：向后兼容 */
    w->tlsVerify = 0;
    w->connectTimeoutMs = timeoutMs > 0 ? (int)timeoutMs : 15000;
    w->frameMaxBytes = WS_MAX_MSG;

    /* 可选 optsJson */
    if (argc > 3 && JS_IsString(argv[3])) {
        const char *oj = JS_ToCString(ctx, argv[3]);
        if (oj) {
            const char *p;
            if ((p = strstr(oj, "\"tlsVerify\"")) != NULL) {
                p = strchr(p, ':');
                if (p) w->tlsVerify = (atoi(p + 1) != 0) ? 1 : 0;
            }
            if ((p = strstr(oj, "\"connectTimeoutMs\"")) != NULL) {
                p = strchr(p, ':');
                if (p) {
                    int v = atoi(p + 1);
                    if (v > 0) w->connectTimeoutMs = v;
                }
            }
            if ((p = strstr(oj, "\"frameMaxBytes\"")) != NULL) {
                p = strchr(p, ':');
                if (p) {
                    long v = atol(p + 1);
                    if (v > 0) w->frameMaxBytes = v;
                }
            }
            JS_FreeCString(ctx, oj);
        }
    }

    pthread_mutex_init(&w->mu, NULL);

    JS_FreeCString(ctx, url);
    if (headers) JS_FreeCString(ctx, headers);

    if (!w->url || !w->headers) {
        free(w->url);
        free(w->headers);
        free(w->msg);
        pthread_mutex_destroy(&w->mu);
        free(w);
        return JS_NewInt32(ctx, 0);
    }

    pthread_mutex_lock(&g_wsMu);
    id = g_wsNextId++;
    w->id = id;
    w->next = g_ws;
    g_ws = w;
    pthread_mutex_unlock(&g_wsMu);

    if (pthread_create(&w->th, NULL, ws_thread, w) != 0) {
        struct ws_conn **pp;
        pthread_mutex_lock(&g_wsMu);
        pp = &g_ws;
        while (*pp && *pp != w) pp = &(*pp)->next;
        if (*pp == w) *pp = w->next;
        pthread_mutex_unlock(&g_wsMu);
        free(w->url);
        free(w->headers);
        free(w->msg);
        pthread_mutex_destroy(&w->mu);
        free(w);
        return JS_NewInt32(ctx, 0);
    }
    w->thStarted = 1;
    pthread_detach(w->th);
    return JS_NewInt32(ctx, id);
}

/* 在 g_ws 链表中按 id 找连接（调用方不持锁；返回时连接可能仍在使用，
 * 因为我们从不提前释放 ws_conn —— 关闭只置状态、线程 detach 时保留结构，
 * 由 wsClose 标记 + 线程自然结束后统一在 wsList/进程退出时回收） */
static struct ws_conn *ws_find(int id) {
    struct ws_conn *w;
    pthread_mutex_lock(&g_wsMu);
    for (w = g_ws; w; w = w->next) {
        if (w->id == id) break;
    }
    pthread_mutex_unlock(&g_wsMu);
    return w;
}

/* wsState(id) -> {"state":"connecting|open|closed|error","msg":"..","recv":N,"sent":N} */
/* wsState(id) -> JSON 字符串
 * {"state":"connecting|open|closed|error","msg":"...","recv":N,"sent":N,
 *  "phase":"dns|tcp|tls|upgrade|open|closed","stageCode":N,"httpStatus":N,
 *  "closeCode":N,"closeReason":"...","errno":N,"errstr":"...","frames":N}
 * 老字段（state/msg/recv/sent）名与语义保持不变，只新增字段。 */
static JSValue js_ws_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    struct ws_conn *w;
    struct buf out;
    const char *st = "closed";
    const char *ph = "closed";
    char *msg = NULL;
    char *closeReason = NULL;
    char *errstr = NULL;
    long recvBytes = 0, sentBytes = 0, frames = 0;
    int stageCode = 0, httpStatus = 0, closeCode = 0, lastErrno = 0, phase = 5;
    JSValue r;
    (void)this_val;

    memset(&out, 0, sizeof(out));
    if (argc < 1) return JS_ThrowTypeError(ctx, "cgnative.wsState(id)");
    JS_ToInt32(ctx, &id, argv[0]);
    w = ws_find(id);
    if (!w) {
        return JS_NewString(ctx,
            "{\"state\":\"closed\",\"msg\":\"no such id\",\"recv\":0,\"sent\":0,"
            "\"phase\":\"closed\",\"stageCode\":0,\"httpStatus\":0,\"closeCode\":0,"
            "\"closeReason\":\"\",\"errno\":0,\"errstr\":\"\",\"frames\":0}");
    }
    pthread_mutex_lock(&w->mu);
    switch (w->state) {
        case 0: st = "connecting"; break;
        case 1: st = "open"; break;
        case 3: st = "error"; break;
        default: st = "closed"; break;
    }
    phase = w->phase;
    switch (phase) {
        case 0: ph = "dns"; break;
        case 1: ph = "tcp"; break;
        case 2: ph = "tls"; break;
        case 3: ph = "upgrade"; break;
        case 4: ph = "open"; break;
        default: ph = "closed"; break;
    }
    msg = dup_str(w->msg ? w->msg : "");
    closeReason = dup_str(w->closeReason ? w->closeReason : "");
    errstr = dup_str(w->lastErrstr ? w->lastErrstr : "");
    recvBytes = w->recvBytes;
    sentBytes = w->sentBytes;
    frames = w->frameTotal;
    stageCode = w->stageCode;
    httpStatus = w->httpStatus;
    closeCode = w->closeCode;
    lastErrno = w->lastErrno;
    pthread_mutex_unlock(&w->mu);

    buf_adds(&out, "{\"state\":\"");
    buf_adds(&out, st);
    buf_adds(&out, "\",\"msg\":");
    if (msg) buf_addjson(&out, msg, strlen(msg));
    else buf_adds(&out, "\"\"");
    buf_adds(&out, ",\"recv\":");
    buf_addi(&out, recvBytes);
    buf_adds(&out, ",\"sent\":");
    buf_addi(&out, sentBytes);
    buf_adds(&out, ",\"phase\":\"");
    buf_adds(&out, ph);
    buf_adds(&out, "\",\"stageCode\":");
    buf_addi(&out, stageCode);
    buf_adds(&out, ",\"httpStatus\":");
    buf_addi(&out, httpStatus);
    buf_adds(&out, ",\"closeCode\":");
    buf_addi(&out, closeCode);
    buf_adds(&out, ",\"closeReason\":");
    buf_addjson(&out, closeReason ? closeReason : "", closeReason ? strlen(closeReason) : 0);
    buf_adds(&out, ",\"errno\":");
    buf_addi(&out, lastErrno);
    buf_adds(&out, ",\"errstr\":");
    buf_addjson(&out, errstr ? errstr : "", errstr ? strlen(errstr) : 0);
    buf_adds(&out, ",\"frames\":");
    buf_addi(&out, frames);
    buf_addc(&out, '}');

    r = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    buf_free(&out);
    if (msg) free(msg);
    if (closeReason) free(closeReason);
    if (errstr) free(errstr);
    return r;
}

/* wsSend(id, text) -> 1 成功 / 0 失败 */
static JSValue js_ws_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    struct ws_conn *w;
    const char *text;
    size_t n = 0;
    int rc;
    (void)this_val;

    if (argc < 2) return JS_NewInt32(ctx, 0);
    JS_ToInt32(ctx, &id, argv[0]);
    w = ws_find(id);
    if (!w) return JS_NewInt32(ctx, 0);
    text = arg_cstr(ctx, argv[1], &n);
    if (!text) return JS_NewInt32(ctx, 0);
    rc = ws_send_frame(w, WS_OP_TEXT, (const unsigned char *)text, n);
    JS_FreeCString(ctx, text);
    return JS_NewInt32(ctx, rc == 0 ? 1 : 0);
}

/* wsPoll(id) -> ["msg1","msg2"]（取走后清空；空数组 = 无新消息）
 * 注意：仍返回 JSON 数组字符串（向后兼容）；累计帧数在 wsState().frames。 */
static JSValue js_ws_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    struct ws_conn *w;
    struct ws_msg *list, *it;
    JSValue arr;
    unsigned idx = 0;
    (void)this_val;

    if (argc < 1) return JS_NewString(ctx, "[]");
    JS_ToInt32(ctx, &id, argv[0]);
    w = ws_find(id);
    if (!w) return JS_NewString(ctx, "[]");

    pthread_mutex_lock(&w->mu);
    list = w->head;
    w->head = NULL;
    w->tail = NULL;
    pthread_mutex_unlock(&w->mu);

    arr = JS_NewArray(ctx);
    for (it = list; it; it = it->next) {
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewStringLen(ctx, it->text, it->len));
    }
    while (list) {
        struct ws_msg *nx = list->next;
        free(list->text);
        free(list);
        list = nx;
    }

    /* 契约要求返回 JSON 数组字符串，这里返回字符串形式，JS 侧 JSON.parse */
    {
        JSValue json;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue jf = JS_GetPropertyStr(ctx, global, "JSON");
        JSValue sf = JS_GetPropertyStr(ctx, jf, "stringify");
        json = JS_Call(ctx, sf, jf, 1, &arr);
        JS_FreeValue(ctx, sf);
        JS_FreeValue(ctx, jf);
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, arr);
        if (JS_IsException(json) || !JS_IsString(json)) {
            JS_FreeValue(ctx, json);
            return JS_NewString(ctx, "[]");
        }
        return json;
    }
}

/* wsPollEx(id) -> 字符串 {"msgs":["..."],"total":N,"recv":N,"sent":N}
 * 为什么不直接把 total 塞进 wsPoll 的返回值：wsPoll 必须返回 JSON 数组
 * 字符串，media.js 用 Array.isArray(msgs) 判断；JSON 数组挂不了 key。
 * total = 这条连接累计收到的完整数据帧数（不只是本次取走的条数）。 */
static JSValue js_ws_poll_ex(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    struct ws_conn *w;
    struct ws_msg *list, *it;
    struct buf out;
    long total = 0, recvBytes = 0, sentBytes = 0;
    int first = 1;
    JSValue r;
    (void)this_val;

    memset(&out, 0, sizeof(out));
    if (argc < 1) return JS_NewString(ctx, "{\"msgs\":[],\"total\":0,\"recv\":0,\"sent\":0}");
    JS_ToInt32(ctx, &id, argv[0]);
    w = ws_find(id);
    if (!w) return JS_NewString(ctx, "{\"msgs\":[],\"total\":0,\"recv\":0,\"sent\":0}");

    pthread_mutex_lock(&w->mu);
    list = w->head;
    w->head = NULL;
    w->tail = NULL;
    total = w->frameTotal;
    recvBytes = w->recvBytes;
    sentBytes = w->sentBytes;
    pthread_mutex_unlock(&w->mu);

    buf_adds(&out, "{\"msgs\":[");
    for (it = list; it; it = it->next) {
        if (!first) buf_addc(&out, ',');
        first = 0;
        buf_addjson(&out, it->text, it->len);
    }
    buf_adds(&out, "],\"total\":");
    buf_addi(&out, total);
    buf_adds(&out, ",\"recv\":");
    buf_addi(&out, recvBytes);
    buf_adds(&out, ",\"sent\":");
    buf_addi(&out, sentBytes);
    buf_addc(&out, '}');

    while (list) {
        struct ws_msg *nx = list->next;
        free(list->text);
        free(list);
        list = nx;
    }

    r = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    buf_free(&out);
    return r;
}

/* wsClose(id) */
/* wsClose(id)：只置标志 + 置状态，真正的 SSL_shutdown/close 由 I/O 线程做
 * （OpenSSL 一个 SSL 对象不可并发操作，JS 线程不能碰 SSL*）。 */
static JSValue js_ws_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    struct ws_conn *w;
    (void)this_val;

    if (argc < 1) return JS_UNDEFINED;
    JS_ToInt32(ctx, &id, argv[0]);
    w = ws_find(id);
    if (!w) return JS_UNDEFINED;

    pthread_mutex_lock(&w->mu);
    w->wantClose = 1;
    pthread_mutex_unlock(&w->mu);

    /* 线程已退出（fd 已关）时由这里兜底置 closed */
    ws_set_phase(w, 5, 8);
    ws_set_state(w, 2, "closed");
    return JS_UNDEFINED;
}

/* wsList() -> [1,2] */
static JSValue js_ws_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct ws_conn *w;
    struct buf out;
    JSValue r;
    int first = 1;
    (void)this_val;
    (void)argc;
    (void)argv;

    memset(&out, 0, sizeof(out));
    buf_addc(&out, '[');
    pthread_mutex_lock(&g_wsMu);
    for (w = g_ws; w; w = w->next) {
        if (!first) buf_addc(&out, ',');
        buf_addi(&out, w->id);
        first = 0;
    }
    pthread_mutex_unlock(&g_wsMu);
    buf_addc(&out, ']');
    r = JS_NewStringLen(ctx, out.data ? out.data : "[]", out.len);
    buf_free(&out);
    return r;
}

/* wsLastFrame(id) -> {"len":N,"op":1,"hex":"8189...","head":64}
 * 帧级自证：把最后一次 wsSend 真正组出来的帧前 64 字节 hex 吐出来，
 * 便于脱离网关肉眼核对 RFC6455 结构：
 *   byte0 = 0x81 (FIN=1, opcode=text)
 *   byte1 = 0x80|len (<126)  / 0x80|126 + 2B  / 0x80|127 + 8B
 *   随后 4 字节掩码，再后是 payload ^ mask。
 * 无记录时返回 len=0、hex=""。 */
static JSValue js_ws_last_frame(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    int id = 0;
    struct ws_conn *w;
    struct buf out;
    struct buf hex;
    JSValue r;
    unsigned char head[64];
    size_t hn = 0;
    long len = 0;
    int op = 0;
    (void)this_val;

    if (argc < 1) return JS_NewString(ctx, "{\"len\":0,\"op\":0,\"hex\":\"\"}");
    JS_ToInt32(ctx, &id, argv[0]);
    w = ws_find(id);
    if (!w) return JS_NewString(ctx, "{\"len\":0,\"op\":0,\"hex\":\"\"}");

    pthread_mutex_lock(&w->mu);
    hn = w->lastFrameHead;
    if (hn > sizeof(head)) hn = sizeof(head);
    if (hn) memcpy(head, w->lastFrame, hn);
    len = w->lastFrameLen;
    op = w->lastFrameOp;
    pthread_mutex_unlock(&w->mu);

    memset(&hex, 0, sizeof(hex));
    if (hn) hex_encode(head, hn, &hex);
    memset(&out, 0, sizeof(out));
    buf_adds(&out, "{\"len\":");
    buf_addi(&out, len);
    buf_adds(&out, ",\"op\":");
    buf_addi(&out, op);
    buf_adds(&out, ",\"head\":");
    buf_addi(&out, (long)hn);
    buf_adds(&out, ",\"hex\":");
    buf_addjson(&out, hex.data ? hex.data : "", hex.len);
    buf_addc(&out, '}');
    r = JS_NewStringLen(ctx, out.data ? out.data : "{}", out.len);
    buf_free(&out);
    buf_free(&hex);
    return r;
}

/* ================= 图片格式侦测 / webp 落地转换 ================= */
/*
 * 存在理由：设备上 libfalcon / gdk-pixbuf / gst 全都没有 webp 解码器
 * （strings 全 0、loader 里没有），而网易图床拒绝转格式（25 种变体实测
 * 全部原样返回 image/webp）。所以 webp 图标放进 <image> 就是全透明。
 *
 * 出路：native 侧把图拉下来，用设备自带的 /usr/bin/ffmpeg 转成 png，
 * 再用 file:// 交给 <image>（falcon 支持 http:/file:///data:image 三种 scheme）。
 *
 * imgScan 只发 Range: bytes=0-63 的探测请求（图床支持 206），三十多个图标
 * 并发探测也就几十毫秒，不整张下载。
 */

#define IMG_PROBE_TIMEOUT_MS 5000
#define IMG_PROBE_BYTES      64
/* 首页一次要扫 38 个图标；每个 worker 只是一条阻塞的 curl，成本极低，
 * 给 12 并发，最坏情况也不会把 JS 线程按秒级卡住。 */
#define IMG_SCAN_WORKERS     12
#define IMG_CACHE_DIR        "/userdisk/miniapp/data/mini_app/cache/lru"
#define IMG_CONV_MAX_W       160
#define IMG_LRU_SIZE         64
#define IMG_MIN_PNG_BYTES    200
#define IMG_MAX_DOWNLOAD     (8L * 1024L * 1024L)

/* 只认 magic，不信 Content-Type（图床的 CT 实测不可靠，永远 image/webp） */
static const char *img_kind(const unsigned char *p, size_t n) {
    if (n >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
        p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P') {
        return "webp";
    }
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G' &&
        p[4] == 0x0D && p[5] == 0x0A && p[6] == 0x1A && p[7] == 0x0A) {
        return "png";
    }
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "jpg";
    return "other";
}

struct probe_buf {
    unsigned char data[IMG_PROBE_BYTES];
    size_t len;
};

/* 只留前 IMG_PROBE_BYTES 字节。服务端若忽略 Range 返回 200 全图，这里也
 * 不会把整张图读进内存；返回值仍报"已全部消费"以免 curl 抛 WRITE_ERROR。 */
static size_t probe_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct probe_buf *b = (struct probe_buf *)userdata;
    size_t n = size * nmemb;
    size_t room = sizeof(b->data) - b->len;
    if (n > room) n = room;
    if (n) {
        memcpy(b->data + b->len, ptr, n);
        b->len += n;
    }
    return size * nmemb;
}

/* 返回 "webp"/"png"/"jpg"/"other"/"err"（静态字符串，调用方不要 free） */
static const char *img_probe_one(const char *url) {
    CURL *h;
    CURL_slist *list = NULL;
    struct probe_buf pb;
    int code;
    long status = 0;

    if (!url || !url[0]) return "err";
    pthread_once(&g_curl_once, curl_bootstrap);
    h = curl_easy_init();
    if (!h) return "err";
    memset(&pb, 0, sizeof(pb));

    /* Range 探测：图床返回 206 + 前 64 字节，省掉整张图的流量 */
    list = curl_slist_append(list, "Range: bytes=0-63");
    if (list) curl_easy_setopt(h, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, probe_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &pb);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)IMG_PROBE_TIMEOUT_MS);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, (long)IMG_PROBE_TIMEOUT_MS);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);

    code = curl_easy_perform(h);
    if (code == 0) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (list) curl_slist_free_all(list);
    curl_easy_cleanup(h);

    if (code != 0) return "err";
    if (status != 200 && status != 206) return "err";
    if (pb.len < 3) return "err";
    return img_kind(pb.data, pb.len);
}

/* --- 极简 JSON 字符串数组解析：["a","b"]（够用，不引第三方依赖） --- */
static int json_skip_ws(const char *s, int i) {
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') i++;
    return i;
}

/* 解析一个 JSON 字符串到 out（调用方给 4 倍长度余量，\uXXXX 会展开成 UTF-8） */
static int json_parse_string(const char *s, int i, char *out, int outCap) {
    int o = 0;
    if (s[i] != '"') return -1;
    i++;
    while (s[i] && s[i] != '"') {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\\') {
            i++;
            switch (s[i]) {
                case 'n': ch = '\n'; i++; break;
                case 'r': ch = '\r'; i++; break;
                case 't': ch = '\t'; i++; break;
                case 'b': ch = '\b'; i++; break;
                case 'f': ch = '\f'; i++; break;
                case 'u': {
                    unsigned cp = 0;
                    int k;
                    for (k = 1; k <= 4; k++) {
                        int v = hex_val((unsigned char)s[i + k]);
                        if (v < 0) return -1;
                        cp = (cp << 4) | (unsigned)v;
                    }
                    i += 5;
                    /* BMP 内直接编 UTF-8；代理对简化处理（图标 URL 不会出现） */
                    if (cp < 0x80) {
                        if (o + 1 >= outCap) return -1;
                        out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (o + 2 >= outCap) return -1;
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (o + 3 >= outCap) return -1;
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                    continue;
                }
                default: ch = (unsigned char)s[i]; i++; break;
            }
        } else {
            i++;
        }
        if (o + 1 >= outCap) return -1;
        out[o++] = (char)ch;
    }
    if (s[i] != '"') return -1;
    out[o] = '\0';
    return i + 1;
}

/* 解析 ["u1","u2"]，返回条数；失败 -1。out 由调用方 free。 */
static int json_parse_str_array(const char *s, char ***out, int maxItems) {
    int i = 0, n = 0, cap = 8;
    char **arr;

    if (!s) return -1;
    i = json_skip_ws(s, i);
    if (s[i] != '[') return -1;
    i++;
    arr = (char **)calloc((size_t)cap, sizeof(char *));
    if (!arr) return -1;
    for (;;) {
        i = json_skip_ws(s, i);
        if (s[i] == ']') {
            i++;
            break;
        }
        if (s[i] == '\0') {
            goto fail;
        }
        {
            size_t slen = strlen(s + i);
            int bufCap = (int)(slen * 4 + 8);
            char *item = (char *)malloc((size_t)bufCap);
            int ni;
            if (!item) goto fail;
            ni = json_parse_string(s, i, item, bufCap);
            if (ni < 0) {
                free(item);
                goto fail;
            }
            i = ni;
            if (n >= maxItems) {
                free(item);
                goto fail;
            }
            if (n >= cap) {
                char **gr = (char **)realloc(arr, (size_t)(cap * 2) * sizeof(char *));
                if (!gr) {
                    free(item);
                    goto fail;
                }
                arr = gr;
                cap *= 2;
            }
            arr[n++] = item;
        }
        i = json_skip_ws(s, i);
        if (s[i] == ',') {
            i++;
            continue;
        }
        if (s[i] == ']') {
            i++;
            break;
        }
        goto fail;
    }
    *out = arr;
    return n;
fail:
    {
        int k;
        for (k = 0; k < n; k++) free(arr[k]);
        free(arr);
    }
    return -1;
}

/* --- 并发探测 --- */
struct scan_ctx {
    char **urls;
    const char **kinds;
    int count;
    int next;
    pthread_mutex_t mu;
};

static void *scan_worker(void *arg) {
    struct scan_ctx *c = (struct scan_ctx *)arg;
    for (;;) {
        int i;
        pthread_mutex_lock(&c->mu);
        i = c->next++;
        pthread_mutex_unlock(&c->mu);
        if (i >= c->count) break;
        c->kinds[i] = img_probe_one(c->urls[i]);
    }
    return NULL;
}

/* imgScan(urlsJson) -> {"<url>":"webp", ...}
 * 同步 API（JS 侧 await 一次即可）；内部多线程并发，32 个 URL 也不会卡 UI。 */
static JSValue js_img_scan(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *arg;
    size_t argLen = 0;
    char **urls = NULL;
    const char **kinds = NULL;
    struct scan_ctx c;
    pthread_t th[IMG_SCAN_WORKERS];
    int started[IMG_SCAN_WORKERS];
    int n, i, workers;
    struct buf out;
    JSValue r;
    (void)this_val;

    if (argc < 1) return JS_ThrowTypeError(ctx, "cgnative.imgScan(urlsJson)");

    /* 入参两种都接受：真 JS 数组 ["u1","u2"]，或 JSON 文本 "['u1','u2']" 形式。
     * 早期只认字符串，导致 JS 侧直接传数组时被 ToCString 成 "u1,u2" 解析失败。 */
    if (JS_IsArray(ctx, argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        int32_t alen = 0;
        JS_ToInt32(ctx, &alen, lv);
        JS_FreeValue(ctx, lv);
        if (alen < 0) alen = 0;
        if (alen > 256) alen = 256;
        urls = (char **)calloc((size_t)(alen > 0 ? alen : 1), sizeof(char *));
        if (!urls) return JS_NewString(ctx, "{}");
        n = 0;
        for (i = 0; i < alen; i++) {
            JSValue iv = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            size_t ilen = 0;
            const char *ic = JS_ToCStringLen(ctx, &ilen, iv);
            JS_FreeValue(ctx, iv);
            if (!ic) continue;
            urls[n] = (char *)malloc(ilen + 1);
            if (!urls[n]) {
                JS_FreeCString(ctx, ic);
                continue;
            }
            memcpy(urls[n], ic, ilen);
            urls[n][ilen] = '\0';
            JS_FreeCString(ctx, ic);
            n++;
        }
    } else {
        arg = arg_cstr(ctx, argv[0], &argLen);
        if (!arg) return JS_EXCEPTION;
        n = json_parse_str_array(arg, &urls, 256);
        JS_FreeCString(ctx, arg);
    }
    if (n < 0) return JS_NewString(ctx, "{}");

    memset(&out, 0, sizeof(out));
    buf_addc(&out, '{');
    if (n > 0) {
        kinds = (const char **)calloc((size_t)n, sizeof(const char *));
        if (!kinds) {
            for (i = 0; i < n; i++) free(urls[i]);
            free(urls);
            return JS_NewString(ctx, "{}");
        }
        for (i = 0; i < n; i++) kinds[i] = "err";

        memset(&c, 0, sizeof(c));
        c.urls = urls;
        c.kinds = kinds;
        c.count = n;
        c.next = 0;
        pthread_mutex_init(&c.mu, NULL);

        workers = n < IMG_SCAN_WORKERS ? n : IMG_SCAN_WORKERS;
        for (i = 0; i < workers; i++) {
            started[i] = (pthread_create(&th[i], NULL, scan_worker, &c) == 0);
        }
        for (i = 0; i < workers; i++) {
            if (started[i]) pthread_join(th[i], NULL);
        }
        pthread_mutex_destroy(&c.mu);

        for (i = 0; i < n; i++) {
            if (i) buf_addc(&out, ',');
            buf_addjson(&out, urls[i], strlen(urls[i]));
            buf_addc(&out, ':');
            buf_addjson(&out, kinds[i], strlen(kinds[i]));
        }
        free(kinds);
    }
    buf_addc(&out, '}');

    for (i = 0; i < n; i++) free(urls[i]);
    free(urls);

    r = JS_NewStringLen(ctx, out.data ? out.data : "{}", out.len);
    buf_free(&out);
    return r;
}

/* --- 进程内 LRU：避免每次拿图标都 stat 一遍磁盘 --- */
struct lru_ent {
    char url[512];
    char path[320];
    unsigned long tick;
    int used;
};

static struct lru_ent g_lru[IMG_LRU_SIZE];
static unsigned long g_lruTick = 0;
static pthread_mutex_t g_lruMu = PTHREAD_MUTEX_INITIALIZER;

static int lru_get(const char *url, char *pathOut, size_t cap) {
    int i, hit = -1;
    pthread_mutex_lock(&g_lruMu);
    for (i = 0; i < IMG_LRU_SIZE; i++) {
        if (g_lru[i].used && strcmp(g_lru[i].url, url) == 0) {
            hit = i;
            break;
        }
    }
    if (hit >= 0) {
        g_lru[hit].tick = ++g_lruTick;
        if (pathOut && cap) {
            strncpy(pathOut, g_lru[hit].path, cap - 1);
            pathOut[cap - 1] = '\0';
        }
    }
    pthread_mutex_unlock(&g_lruMu);
    return hit >= 0;
}

static void lru_put(const char *url, const char *path) {
    int i, slot = -1;
    unsigned long oldest = (unsigned long)-1;
    pthread_mutex_lock(&g_lruMu);
    for (i = 0; i < IMG_LRU_SIZE; i++) {
        if (!g_lru[i].used) {
            slot = i;
            break;
        }
        if (g_lru[i].tick < oldest) {
            oldest = g_lru[i].tick;
            slot = i;
        }
    }
    if (slot >= 0) {
        strncpy(g_lru[slot].url, url, sizeof(g_lru[slot].url) - 1);
        g_lru[slot].url[sizeof(g_lru[slot].url) - 1] = '\0';
        strncpy(g_lru[slot].path, path, sizeof(g_lru[slot].path) - 1);
        g_lru[slot].path[sizeof(g_lru[slot].path) - 1] = '\0';
        g_lru[slot].tick = ++g_lruTick;
        g_lru[slot].used = 1;
    }
    pthread_mutex_unlock(&g_lruMu);
}

static int file_size_at_least(const char *path, long minBytes) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    return (long)st.st_size >= minBytes;
}

/* 整张图下载到 dest。返回 0 成功。 */
static int img_download_to(const char *url, const char *dest) {
    CURL *h;
    CURL_slist *list = NULL;
    struct buf body;
    int code;
    long status = 0;
    FILE *fp;

    pthread_once(&g_curl_once, curl_bootstrap);
    memset(&body, 0, sizeof(body));
    h = curl_easy_init();
    if (!h) return -1;
    list = curl_slist_append(list, "Accept: image/*,*/*");
    if (list) curl_easy_setopt(h, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);

    code = curl_easy_perform(h);
    if (code == 0) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (list) curl_slist_free_all(list);
    curl_easy_cleanup(h);

    if (code != 0 || status != 200 || body.len < 64 || (long)body.len > IMG_MAX_DOWNLOAD) {
        buf_free(&body);
        return -1;
    }
    fp = fopen(dest, "wb");
    if (!fp) {
        buf_free(&body);
        return -1;
    }
    if (fwrite(body.data, 1, body.len, fp) != body.len) {
        fclose(fp);
        buf_free(&body);
        return -1;
    }
    fclose(fp);
    buf_free(&body);
    return 0;
}

/* 调设备自带 ffmpeg 把 webp 转 png。
 * 注意 -vf 里的逗号必须转义成 "\,"，否则 filtergraph 解析器会把
 * "min(160" 当成两个参数直接报 Invalid size（实测）。 */
static int img_ffmpeg_to_png(const char *src, const char *dst) {
    pid_t pid;
    int status = 0;
    char vf[64];
    int i;

    snprintf(vf, sizeof(vf), "scale=min(%d\\,iw):-1", IMG_CONV_MAX_W);

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        execl("/usr/bin/ffmpeg", "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
              "-nostdin", "-i", src, "-vf", vf, "-frames:v", "1", dst, (char *)NULL);
        _exit(127);
    }
    /* 设备上的宿主进程把 SIGCHLD 设成了 SIG_IGN（/proc/<pid>/status 的
     * SigIgn 含 0x10000），子进程会被内核自动回收，于是 waitpid() 立刻返回
     * -1/ECHILD。早期把 -1 当失败，导致 ffmpeg 明明转码成功却返回 ""。
     * 这里改成「以产物文件为准、退出状态仅作参考」：
     *   - 每 50ms 探一次输出文件大小，同时尽可能 waitpid 一次（ECHILD 不算错）。
     *   - 文件 >IMG_MIN_PNG_BYTES 且已被回收/或已 ECHILD，即判定成功。
     *   - 15s 还没出结果就 SIGKILL，绝不留僵尸也不卡 JS 线程。 */
    for (i = 0; i < 300; i++) {
        pid_t r;
        int reaped = 0;
        struct stat st;
        usleep(50000);
        r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            reaped = 1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                /* 非 0 退出：产物若已完整写出仍然可用，交给下面的大小判断 */
                if (!file_size_at_least(dst, IMG_MIN_PNG_BYTES)) return -1;
            }
        } else if (r < 0) {
            reaped = 1; /* ECHILD：已被 SIG_IGN 自动回收，按产物判定 */
        }
        if (stat(dst, &st) == 0 && S_ISREG(st.st_mode) &&
            (long)st.st_size >= IMG_MIN_PNG_BYTES) {
            if (reaped) return 0;
        }
        if (reaped && (stat(dst, &st) != 0 || (long)st.st_size < IMG_MIN_PNG_BYTES)) {
            /* 进程已结束但没产物（或太小），再给一次机会等落盘 */
            usleep(100000);
            return file_size_at_least(dst, IMG_MIN_PNG_BYTES) ? 0 : -1;
        }
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, WNOHANG);
    return file_size_at_least(dst, IMG_MIN_PNG_BYTES) ? 0 : -1;
}

/* imgConv(url) -> "file:///abs/path.png"（失败返回 ""，绝不抛异常） */
static JSValue js_img_conv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url;
    size_t urlLen = 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    struct buf hex;
    char outPath[320];
    char binPath[320];
    char cached[320];
    int ok = 0;
    JSValue r;
    (void)this_val;

    if (argc < 1) return JS_NewString(ctx, "");
    url = arg_cstr(ctx, argv[0], &urlLen);
    if (!url || urlLen == 0) {
        if (url) JS_FreeCString(ctx, url);
        return JS_NewString(ctx, "");
    }

    /* 1) LRU 命中直接返回，不碰磁盘 */
    if (lru_get(url, cached, sizeof(cached))) {
        r = JS_NewString(ctx, cached);
        JS_FreeCString(ctx, url);
        return r;
    }

    memset(&hex, 0, sizeof(hex));
    if (!EVP_Digest(url, urlLen, digest, &dlen, EVP_md5(), NULL)) {
        JS_FreeCString(ctx, url);
        return JS_NewString(ctx, "");
    }
    hex_encode(digest, dlen, &hex);
    buf_addc(&hex, '\0');

    mkdir(IMG_CACHE_DIR, 0755); /* 已存在会 EEXIST，忽略 */
    snprintf(outPath, sizeof(outPath), "%s/pc_%s.png", IMG_CACHE_DIR, hex.data);
    snprintf(binPath, sizeof(binPath), "%s/pc_%s.bin", IMG_CACHE_DIR, hex.data);

    /* 2) 已转换过：缓存命中（稳态路径，只 stat 一次） */
    if (file_size_at_least(outPath, IMG_MIN_PNG_BYTES)) {
        char full[360];
        snprintf(full, sizeof(full), "file://%s", outPath);
        lru_put(url, full);
        r = JS_NewString(ctx, full);
        buf_free(&hex);
        JS_FreeCString(ctx, url);
        return r;
    }

    /* 3) 下载整张图 */
    if (img_download_to(url, binPath) != 0) {
        buf_free(&hex);
        JS_FreeCString(ctx, url);
        return JS_NewString(ctx, "");
    }

    /* 4) 按 magic 决定怎么处理 */
    {
        unsigned char magic[12];
        size_t got = 0;
        FILE *fp = fopen(binPath, "rb");
        if (!fp) {
            unlink(binPath);
            buf_free(&hex);
            JS_FreeCString(ctx, url);
            return JS_NewString(ctx, "");
        }
        got = fread(magic, 1, sizeof(magic), fp);
        fclose(fp);

        {
            const char *kind = img_kind(magic, got);
            if (strcmp(kind, "webp") == 0) {
                /* webp 才需要转码 */
                unlink(outPath);
                if (img_ffmpeg_to_png(binPath, outPath) == 0 &&
                    file_size_at_least(outPath, IMG_MIN_PNG_BYTES)) {
                    ok = 1;
                }
            } else if (strcmp(kind, "png") == 0 || strcmp(kind, "jpg") == 0) {
                /* falcon 按内容解码，不按后缀，直接改名即可 */
                unlink(outPath);
                if (rename(binPath, outPath) == 0 &&
                    file_size_at_least(outPath, IMG_MIN_PNG_BYTES)) {
                    ok = 1;
                }
            }
        }
    }
    unlink(binPath); /* webp 转完后 bin 没用了；png/jpg 已经 rename 走 */

    if (ok) {
        char full[360];
        snprintf(full, sizeof(full), "file://%s", outPath);
        lru_put(url, full);
        r = JS_NewString(ctx, full);
    } else {
        r = JS_NewString(ctx, "");
    }
    buf_free(&hex);
    JS_FreeCString(ctx, url);
    return r;
}

/* ---------------- 模块定义 ---------------- */
#define CG_EXPORTS(F) \
    F("postSync",     js_post_sync,      4) \
    F("requestSync",  js_request_sync,   5) \
    F("startRequest", js_start_request,  5) \
    F("startPost",    js_start_post,     4) \
    F("takeResult",   js_take_result,    1) \
    F("cancelAll",    js_cancel_all,     0) \
    F("sha256Hex",    js_sha256_hex,     1) \
    F("md5Hex",       js_md5_hex,        1) \
    F("b64enc",       js_b64enc,         1) \
    F("b64dec",       js_b64dec,         1) \
    F("tripleDesB64", js_triple_des_b64, 3) \
    F("randomHex",    js_random_hex,     1) \
    F("hmacSha1B64",  js_hmac_sha1_b64,  2) \
    F("utf8Hex",      js_utf8_hex,       1) \
    F("version",      js_version,        0) \
    F("wsOpen",       js_ws_open,        3) \
    F("wsState",      js_ws_state,       1) \
    F("wsSend",       js_ws_send,        2) \
    F("wsPoll",       js_ws_poll,        1) \
    F("wsPollEx",     js_ws_poll_ex,     1) \
    F("wsClose",      js_ws_close,       1) \
    F("wsList",       js_ws_list,        0) \
    F("wsLastFrame",  js_ws_last_frame,  1) \
    F("imgScan",      js_img_scan,       1) \
    F("imgConv",      js_img_conv,       1)

static int cgnative_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue def = JS_NewObject(ctx);
#define CG_DEF(name, fn, len)                                                   \
    do {                                                                        \
        JS_SetPropertyStr(ctx, def, name, JS_NewCFunction(ctx, fn, name, len));  \
        JS_SetModuleExport(ctx, m, name, JS_NewCFunction(ctx, fn, name, len));   \
    } while (0);
    CG_EXPORTS(CG_DEF)
#undef CG_DEF
    JS_SetModuleExport(ctx, m, "default", def);
    return 0;
}

static JSModuleDef *cgnative_module_load(JSContext *ctx, const char *moduleName) {
    if (strcmp(moduleName, "cgnative") == 0) {
        JSModuleDef *m = JS_NewCModule(ctx, moduleName, cgnative_module_init);
        if (!m) return NULL;
#define CG_ADD(name, fn, len) JS_AddModuleExport(ctx, m, name);
        CG_EXPORTS(CG_ADD)
#undef CG_ADD
        JS_AddModuleExport(ctx, m, "default");
        return m;
    }
    return NULL;
}

extern void registerCModuleLoader(const char *moduleName, LoadCModuleFunction loader);

JQUICK_EXPORT void custom_init_jsapis(void) {
    registerCModuleLoader("cgnative", &cgnative_module_load);
}
