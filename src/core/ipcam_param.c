#define _GNU_SOURCE
#include "ipcam_param.h"
#include "ipcam_config.h"
#include "ipcam_log.h"
#include "ipcam_sys.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ipcam_param_t s_param;
static char          s_path[256] = IPCAM_PARAM_PATH_DEF;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* CRC32（IEEE 802.3，多项式 0xEDB88320）—— 校验字段 */
static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
        }
    }
    return ~crc;
}

static uint32_t param_crc(const ipcam_param_t *p)
{
    /*
     * 只对 crc 字段之后的业务 payload 做 CRC（从 model 起算到结构体末尾）。
     * 若把 crc 自身算进去：保存时用「旧 crc」哈希，加载时用「新 crc」再哈希，
     * 两端永远对不上，导致 /etc/ipcam.conf 每次启动都 CRC 失败并回退默认值。
     */
    const unsigned char *base = (const unsigned char *)p;
    size_t off = offsetof(ipcam_param_t, model);
    return crc32_update(0, base + off, sizeof(*p) - off);
}

/*
 * packed 4:2:2 每两个像素共享一组色度，采集宽度必须为偶数；同时限制
 * 到当前 V4L2/缩放链路允许的范围，避免异常配置在 main.c 中放大成超大分配。
 */
static int capture_width_valid(uint16_t value)
{
    return value >= 2 && value <= 4096 && (value & 1) == 0;
}

static int capture_height_valid(uint16_t value)
{
    return value >= 1 && value <= 4096;
}

/* 从 compile-time 默认填充（ipcam_config.h） */
static void load_defaults(ipcam_param_t *p)
{
    memset(p, 0, sizeof(*p));
    p->magic   = IPCAM_PARAM_MAGIC;
    p->version = IPCAM_PARAM_VERSION;
    p->size    = sizeof(*p);
    strncpy(p->model, IPCAM_MODEL, sizeof(p->model) - 1);
    strncpy(p->swver, IPCAM_VERSION, sizeof(p->swver) - 1);

    if      (strcmp(IPCAM_NET_MODE, "4g")   == 0) p->net_mode = IPCAM_NET_MODE_4G;
    else if (strcmp(IPCAM_NET_MODE, "wifi") == 0) p->net_mode = IPCAM_NET_MODE_WIFI;
    else                                           p->net_mode = IPCAM_NET_MODE_NONE;

    strncpy(p->wifi_ssid, IPCAM_WIFI_SSID, sizeof(p->wifi_ssid) - 1);
    strncpy(p->wifi_psk,  IPCAM_WIFI_PSK,  sizeof(p->wifi_psk)  - 1);
    strncpy(p->apn,       IPCAM_4G_APN,    sizeof(p->apn)       - 1);

    p->capture_w = IPCAM_CAPTURE_WIDTH;
    p->capture_h = IPCAM_CAPTURE_HEIGHT;
    p->out_w = IPCAM_OUTPUT_WIDTH;      /* 0 = 跟随采集分辨率（旁路缩放） */
    p->out_h = IPCAM_OUTPUT_HEIGHT;
    p->jpeg_quality = IPCAM_JPEG_QUALITY;
    p->target_fps   = IPCAM_TARGET_FPS;

    p->http_port       = IPCAM_HTTP_PORT;
    /* 默认绑 0.0.0.0，便于局域网/4G 侧通过板 IP 访问推流；需要仅本机时可改回 1 */
    p->http_bind_local = 0;

    p->log_level = IPCAM_LOG_LEVEL;

    p->crc = param_crc(p);
}

int ipcam_param_init(const char *path)
{
    if (path && *path) {
        strncpy(s_path, path, sizeof(s_path) - 1);
        s_path[sizeof(s_path) - 1] = '\0';
    }

    load_defaults(&s_param);
    /* 默认情况（无文件）也拍一次快照 */
    ipcam_sys_take_snapshot();

    int fd = open(s_path, O_RDONLY);
    if (fd < 0) {
        MLOGW("param file %s missing, using defaults (will save on first set)\n", s_path);
        return 0;
    }

    ipcam_param_t loaded;
    ssize_t n = read(fd, &loaded, sizeof(loaded));
    close(fd);
    if (n != (ssize_t)sizeof(loaded)) {
        MLOGW("param file %s short read (%zd), using defaults\n", s_path, n);
        return 0;
    }

    if (loaded.magic != IPCAM_PARAM_MAGIC || loaded.version != IPCAM_PARAM_VERSION) {
        MLOGW("param file %s magic/version mismatch, using defaults\n", s_path);
        return 0;
    }
    if (loaded.size != sizeof(loaded)) {
        MLOGW("param file %s size mismatch, using defaults\n", s_path);
        return 0;
    }
    uint32_t expected = param_crc(&loaded);
    if (expected != loaded.crc) {
        MLOGW("param file %s CRC mismatch (got %08x want %08x), using defaults\n",
              s_path, loaded.crc, expected);
        return 0;
    }

    /* CRC 正确不代表字段满足当前 packed 4:2:2 约束，旧配置也必须重新校验。 */
    if (!capture_width_valid(loaded.capture_w) ||
        !capture_height_valid(loaded.capture_h)) {
        MLOGW("param file %s has invalid capture size %ux%u, using defaults\n",
              s_path, loaded.capture_w, loaded.capture_h);
        return 0;
    }

    s_param = loaded;
    MLOGI("param loaded from %s\n", s_path);
    /* 同步更新 crash handler 快照，确保 daemon 启动期崩溃也能看到正确配置 */
    ipcam_sys_take_snapshot();
    return 0;
}

int ipcam_param_save(void)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_path);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        MLOGE("open %s for write: %s\n", tmp, strerror(errno));
        return -1;
    }

    /* 整个 read-modify-write 在锁内完成，避免与并发 setter 交错 */
    pthread_mutex_lock(&s_mtx);
    s_param.crc = param_crc(&s_param);
    ssize_t n = write(fd, &s_param, sizeof(s_param));
    pthread_mutex_unlock(&s_mtx);

    if (n != (ssize_t)sizeof(s_param)) {
        MLOGE("write %s: %s\n", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (fsync(fd) < 0) {
        MLOGE("fsync %s: %s\n", tmp, strerror(errno));
        /* 不致命 */
    }
    close(fd);

    if (rename(tmp, s_path) < 0) {
        MLOGE("rename %s -> %s: %s\n", tmp, s_path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    /* 同步更新 crash handler 的快照（如果已注册） */
    ipcam_sys_take_snapshot();
    return 0;
}

const ipcam_param_t *ipcam_param_get(void)
{
    return &s_param;
}

/* --- getters --- */
#define PARAM_GETTER(T, name, field)              \
    T ipcam_param_get_##name(void) {              \
        pthread_mutex_lock(&s_mtx);                \
        T v = s_param.field;                       \
        pthread_mutex_unlock(&s_mtx);              \
        return v;                                  \
    }
#define PARAM_STR_GETTER(name, field)             \
    const char *ipcam_param_get_##name(void) {     \
        static __thread char buf[128];             \
        pthread_mutex_lock(&s_mtx);                \
        strncpy(buf, s_param.field, sizeof(buf)-1);\
        buf[sizeof(buf)-1] = '\0';                 \
        pthread_mutex_unlock(&s_mtx);              \
        return buf;                                \
    }

PARAM_GETTER(uint8_t,  net_mode,         net_mode)
PARAM_STR_GETTER(      wifi_ssid,        wifi_ssid)
PARAM_STR_GETTER(      wifi_psk,         wifi_psk)
PARAM_STR_GETTER(      apn,              apn)
PARAM_GETTER(uint16_t, capture_w,        capture_w)
PARAM_GETTER(uint16_t, capture_h,        capture_h)
PARAM_GETTER(uint16_t, out_w,            out_w)
PARAM_GETTER(uint16_t, out_h,            out_h)
PARAM_GETTER(uint8_t,  jpeg_quality,     jpeg_quality)
PARAM_GETTER(uint8_t,  target_fps,       target_fps)
PARAM_GETTER(uint16_t, http_port,        http_port)
PARAM_GETTER(uint8_t,  http_bind_local,  http_bind_local)
PARAM_GETTER(uint8_t,  log_level,        log_level)
PARAM_STR_GETTER(      model,            model)
PARAM_STR_GETTER(      swver,            swver)

/* --- setters --- */
/*
 * 拒绝会破坏 wpa_supplicant 明文 conf 引号，或可注入 AT+CGDCONT 字符串的字符。
 * 含双引号、反斜杠、换行及其它控制字符时直接判失败，避免配置层注入。
 */
static int string_has_unsafe_chars(const char *s)
{
    if (!s) return 1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p < 0x20)
            return 1;
    }
    return 0;
}

static int set_string_field(char *dst, size_t dst_sz, const char *src)
{
    if (!src) return -1;
    pthread_mutex_lock(&s_mtx);
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}

int ipcam_param_set_net_mode(uint8_t v)
{
    if (v > IPCAM_NET_MODE_WIFI) return -1;
    pthread_mutex_lock(&s_mtx);
    s_param.net_mode = v;
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}
int ipcam_param_set_wifi_ssid(const char *ssid)
{
    if (string_has_unsafe_chars(ssid)) return -1;
    return set_string_field(s_param.wifi_ssid, sizeof(s_param.wifi_ssid), ssid);
}
int ipcam_param_set_wifi_psk(const char *psk)
{
    if (string_has_unsafe_chars(psk)) return -1;
    return set_string_field(s_param.wifi_psk, sizeof(s_param.wifi_psk), psk);
}
int ipcam_param_set_apn(const char *apn)
{
    if (string_has_unsafe_chars(apn)) return -1;
    return set_string_field(s_param.apn, sizeof(s_param.apn), apn);
}

int ipcam_param_set_capture_w(uint16_t v)
{
    if (!capture_width_valid(v)) return -1;
    pthread_mutex_lock(&s_mtx);
    s_param.capture_w = v;
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}
int ipcam_param_set_capture_h(uint16_t v)
{
    if (!capture_height_valid(v)) return -1;
    pthread_mutex_lock(&s_mtx);
    s_param.capture_h = v;
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}
/*
 * 输出宽度必须为偶数：4:2:2 色度按 2 像素一组采样，奇数宽无法整除出色度平面半宽。
 * 0 = 跟随采集分辨率（编码旁路缩放）；与 capture_w/h 独立设置，重启后生效。
 */
int ipcam_param_set_out_w(uint16_t v)
{
    if (v != 0 && ((v & 1) || v > 4096)) return -1;
    pthread_mutex_lock(&s_mtx);
    s_param.out_w = v;
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}
int ipcam_param_set_out_h(uint16_t v)
{
    if (v > 4096) return -1;    /* 0 = 跟随采集分辨率 */
    pthread_mutex_lock(&s_mtx);
    s_param.out_h = v;
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}
int ipcam_param_set_jpeg_quality(uint8_t v)
{
    if (v < 1 || v > 100) return -1;
    pthread_mutex_lock(&s_mtx);
    s_param.jpeg_quality = v;
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}
int ipcam_param_set_target_fps(uint8_t v)
{
    if (v == 0 || v > 60) return -1;
    pthread_mutex_lock(&s_mtx);
    s_param.target_fps = v;
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}
int ipcam_param_set_http_port(uint16_t v)
{
    if (v == 0) return -1;
    pthread_mutex_lock(&s_mtx);
    s_param.http_port = v;
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}
int ipcam_param_set_http_bind_local(uint8_t v)
{
    if (v > 1) return -1;
    pthread_mutex_lock(&s_mtx);
    s_param.http_bind_local = v ? 1 : 0;
    pthread_mutex_unlock(&s_mtx);
    return ipcam_param_save();
}
int ipcam_param_set_log_level(uint8_t v)
{
    if (v >= IPCAM_LOG_BUTT) return -1;
    pthread_mutex_lock(&s_mtx);
    s_param.log_level = v;
    pthread_mutex_unlock(&s_mtx);
    ipcam_log_setlevel((ipcam_log_level_t)v);
    return ipcam_param_save();
}

/*
 * JSON 转义字符串字段（写到 out，out 容量 out_sz）。返回写入字节数；-1 失败。
 * 替换 " \ \b\f\n\r\t 控制字符 → \uXXXX；其它直接透传。
 */
static int json_escape(const char *str, char *out, size_t out_sz)
{
    size_t o = 0;
    for (const char *p = str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (o + 2 >= out_sz) return -1;
            out[o++] = '\\';
            out[o++] = c;
        } else if (c < 0x20) {
            if (o + 6 >= out_sz) return -1;
            int n = snprintf(out + o, out_sz - o, "\\u%04x", c);
            if (n < 0 || (size_t)n >= out_sz - o) return -1;
            o += (size_t)n;
        } else {
            if (o + 1 >= out_sz) return -1;
            out[o++] = c;
        }
    }
    if (o >= out_sz) return -1;
    out[o] = '\0';
    return (int)o;
}

int ipcam_param_to_json(char *buf, size_t buf_sz)
{
    pthread_mutex_lock(&s_mtx);
    ipcam_param_t p = s_param;
    pthread_mutex_unlock(&s_mtx);

    char ssid_esc[256], apn_esc[128], model_esc[64], swver_esc[32];
    if (json_escape(p.wifi_ssid, ssid_esc, sizeof(ssid_esc)) < 0) ssid_esc[0] = '\0';
    if (json_escape(p.apn,       apn_esc,  sizeof(apn_esc))  < 0) apn_esc[0]  = '\0';
    if (json_escape(p.model,     model_esc,sizeof(model_esc))< 0) model_esc[0]='\0';
    if (json_escape(p.swver,     swver_esc,sizeof(swver_esc))< 0) swver_esc[0]='\0';

    int n = snprintf(buf, buf_sz,
        "{"
        "\"model\":\"%s\",\"swver\":\"%s\","
        "\"net_mode\":%u,\"wifi_ssid\":\"%s\",\"apn\":\"%s\","
        "\"capture_w\":%u,\"capture_h\":%u,"
        "\"out_w\":%u,\"out_h\":%u,"
        "\"jpeg_quality\":%u,\"target_fps\":%u,"
        "\"http_port\":%u,\"http_bind_local\":%u,"
        "\"log_level\":%u"
        "}\n",
        model_esc, swver_esc,
        p.net_mode, ssid_esc, apn_esc,
        p.capture_w, p.capture_h,
        p.out_w, p.out_h,
        p.jpeg_quality, p.target_fps,
        p.http_port, p.http_bind_local,
        p.log_level);
    return (n > 0 && (size_t)n < buf_sz) ? n : -1;
}

void ipcam_param_dump(void)
{
    pthread_mutex_lock(&s_mtx);
    ipcam_param_t p = s_param;
    pthread_mutex_unlock(&s_mtx);

    fprintf(stderr, "--- ipcam param ---\n");
    fprintf(stderr, "  model          : %s\n", p.model);
    fprintf(stderr, "  swver          : %s\n", p.swver);
    fprintf(stderr, "  net_mode       : %u\n", p.net_mode);
    fprintf(stderr, "  wifi_ssid      : %s\n", p.wifi_ssid);
    fprintf(stderr, "  apn            : %s\n", p.apn);
    fprintf(stderr, "  capture        : %ux%u\n", p.capture_w, p.capture_h);
    fprintf(stderr, "  output         : %ux%u (0=follow capture)\n", p.out_w, p.out_h);
    fprintf(stderr, "  jpeg_quality   : %u\n", p.jpeg_quality);
    fprintf(stderr, "  target_fps     : %u\n", p.target_fps);
    fprintf(stderr, "  http_port      : %u\n", p.http_port);
    fprintf(stderr, "  http_bind_local: %u\n", p.http_bind_local);
    fprintf(stderr, "  log_level      : %u\n", p.log_level);
    fprintf(stderr, "--- end param ---\n");
}
