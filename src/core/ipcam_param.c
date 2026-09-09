#define _GNU_SOURCE
/* 参数加载、校验和持久化日志归入 PARAM 模块。 */
#define IPCAM_LOG_MODULE "PARAM"
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
static uint32_t s_generation = 1;

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
    p->jpeg_quality = IPCAM_JPEG_QUALITY;
    p->target_fps   = IPCAM_TARGET_FPS;

    p->http_port       = IPCAM_HTTP_PORT;
    /* 默认绑 0.0.0.0，便于局域网/4G 侧通过板 IP 访问推流；需要仅本机时可改回 1 */
    p->http_bind_local = 0;

    p->log_level = IPCAM_LOG_LEVEL;
    /* 新字段位于旧 reserved 区；给出安全默认值，避免旧配置把背光置黑。 */
    p->mirror_horizontal = 0;
    p->mirror_vertical = 0;
    p->preview_enabled = 1;
    p->backlight_percent = 100;
    p->screen_timeout_min = 3;

    p->crc = param_crc(p);
}

/* 加载并校验配置；损坏/版本不兼容时回退默认，保证守护进程仍能启动。 */
int ipcam_param_init(const char *path)
{
    if (path && *path) {
        strncpy(s_path, path, sizeof(s_path) - 1);
        s_path[sizeof(s_path) - 1] = '\0';
    }

    load_defaults(&s_param);
    s_generation = 1;
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

    s_param = loaded;
    /* 早期配置没有本地控制字段，读取出的零值按安全默认值补齐；同时
     * 对 CRC 正确但字段越界的旧文件做内存级迁移，避免启动时把非法值交给 V4L2。 */
    if (s_param.capture_w == 0 || s_param.capture_w > 4096 || (s_param.capture_w & 1))
        s_param.capture_w = IPCAM_CAPTURE_WIDTH;
    if (s_param.capture_h == 0 || s_param.capture_h > 4096)
        s_param.capture_h = IPCAM_CAPTURE_HEIGHT;
    if (s_param.jpeg_quality < 1 || s_param.jpeg_quality > 100)
        s_param.jpeg_quality = IPCAM_JPEG_QUALITY;
    if (s_param.target_fps == 0 || s_param.target_fps > 60)
        s_param.target_fps = IPCAM_TARGET_FPS;
    if (s_param.net_mode > IPCAM_NET_MODE_WIFI)
        s_param.net_mode = IPCAM_NET_MODE_NONE;
    if (s_param.http_port == 0)
        s_param.http_port = IPCAM_HTTP_PORT;
    if (s_param.http_bind_local > 1)
        s_param.http_bind_local = 0;
    if (s_param.log_level >= IPCAM_LOG_BUTT)
        s_param.log_level = IPCAM_LOG_LEVEL;
    s_param.mirror_horizontal = s_param.mirror_horizontal ? 1 : 0;
    s_param.mirror_vertical = s_param.mirror_vertical ? 1 : 0;
    s_param.preview_enabled = s_param.preview_enabled ? 1 : 0;
    if (s_param.backlight_percent == 0 || s_param.backlight_percent > 100)
        s_param.backlight_percent = 100;
    if (s_param.screen_timeout_min != 0 && s_param.screen_timeout_min != 1 &&
        s_param.screen_timeout_min != 3 && s_param.screen_timeout_min != 5 &&
        s_param.screen_timeout_min != 10)
        s_param.screen_timeout_min = 3;
    MLOGI("param loaded from %s\n", s_path);
    /* 同步更新 crash handler 快照，确保 daemon 启动期崩溃也能看到正确配置 */
    ipcam_sys_take_snapshot();
    return 0;
}

/* 调用者已持有 s_mtx 时原子写入配置；失败不改变“已保存”代次。 */
static int param_save_locked(void)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_path);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        int saved_errno = errno;
        MLOGE("open %s for write: %s\n", tmp, strerror(saved_errno));
        return -1;
    }
    /* 先写独立镜像，只有 rename 成功后才修改内存中的 crc；失败回退时
     * 不会留下与当前字段不匹配的校验值。 */
    ipcam_param_t image = s_param;
    image.crc = param_crc(&image);
    ssize_t n = write(fd, &image, sizeof(image));
    if (n != (ssize_t)sizeof(s_param)) {
        MLOGE("write %s: %s\n", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (fsync(fd) < 0) {
        MLOGE("fsync %s: %s\n", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);

    if (rename(tmp, s_path) < 0) {
        MLOGE("rename %s -> %s: %s\n", tmp, s_path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    s_param.crc = image.crc;
    s_generation++;

    /* 目录同步确保掉电时 rename 本身不会丢失；目录打不开不影响已替换文件。 */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", s_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");
    int dfd = open(dir[0] ? dir : ".", O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return 0;
}

int ipcam_param_save(void)
{
    /* 锁覆盖临时文件、fsync 和 rename，避免两个 setter 争用同一 .tmp。 */
    pthread_mutex_lock(&s_mtx);
    int rc = param_save_locked();
    pthread_mutex_unlock(&s_mtx);
    if (rc != 0) return rc;
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
PARAM_GETTER(uint8_t,  jpeg_quality,     jpeg_quality)
PARAM_GETTER(uint8_t,  target_fps,       target_fps)
PARAM_GETTER(uint16_t, http_port,        http_port)
PARAM_GETTER(uint8_t,  http_bind_local,  http_bind_local)
PARAM_GETTER(uint8_t,  log_level,        log_level)
PARAM_GETTER(uint8_t,  mirror_horizontal, mirror_horizontal)
PARAM_GETTER(uint8_t,  mirror_vertical, mirror_vertical)
PARAM_GETTER(uint8_t,  preview_enabled, preview_enabled)
PARAM_GETTER(uint8_t,  backlight_percent, backlight_percent)
PARAM_GETTER(uint8_t,  screen_timeout_min, screen_timeout_min)

/* 返回已成功落盘的配置代次；每次原子替换文件后递增。 */
uint32_t ipcam_param_get_generation(void)
{
    pthread_mutex_lock(&s_mtx);
    uint32_t v = s_generation;
    pthread_mutex_unlock(&s_mtx);
    return v;
}
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
    if (!src || dst_sz == 0 || dst_sz > 128) return -1;
    char old[128];
    pthread_mutex_lock(&s_mtx);
    memcpy(old, dst, dst_sz);
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
    int rc = param_save_locked();
    if (rc != 0) memcpy(dst, old, dst_sz);
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}

/* 修改网络模式并原子保存；失败恢复内存旧值，避免状态与文件分叉。 */
int ipcam_param_set_net_mode(uint8_t v)
{
    if (v > IPCAM_NET_MODE_WIFI) return -1;
    pthread_mutex_lock(&s_mtx);
    uint8_t old = s_param.net_mode;
    s_param.net_mode = v;
    int rc = param_save_locked();
    if (rc != 0) s_param.net_mode = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}
/* 校验并保存 SSID；危险字符已在进入文件/命令前拒绝。 */
int ipcam_param_set_wifi_ssid(const char *ssid)
{
    if (string_has_unsafe_chars(ssid)) return -1;
    return set_string_field(s_param.wifi_ssid, sizeof(s_param.wifi_ssid), ssid);
}
/* 校验并保存 WiFi 密钥；失败时恢复原字符串。 */
int ipcam_param_set_wifi_psk(const char *psk)
{
    if (string_has_unsafe_chars(psk)) return -1;
    return set_string_field(s_param.wifi_psk, sizeof(s_param.wifi_psk), psk);
}
/* 校验并保存蜂窝 APN，避免注入拨号命令。 */
int ipcam_param_set_apn(const char *apn)
{
    if (string_has_unsafe_chars(apn)) return -1;
    return set_string_field(s_param.apn, sizeof(s_param.apn), apn);
}

/* 保存采集宽度；这里只做字段范围校验，档位能力由控制服务仲裁。 */
int ipcam_param_set_capture_w(uint16_t v)
{
    /* YUYV 4:2:2 按两像素共享色度，奇数宽度会破坏 JPEG 平面，
     * 因此参数层和 V4L2 协商层都拒绝奇数宽度。 */
    if (v == 0 || v > 4096 || (v & 1)) return -1;
    pthread_mutex_lock(&s_mtx);
    uint16_t old = s_param.capture_w;
    s_param.capture_w = v;
    int rc = param_save_locked();
    if (rc != 0) s_param.capture_w = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}
/* 保存采集高度；失败时回滚，避免半组视频参数落盘。 */
int ipcam_param_set_capture_h(uint16_t v)
{
    if (v == 0 || v > 4096) return -1;
    pthread_mutex_lock(&s_mtx);
    uint16_t old = s_param.capture_h;
    s_param.capture_h = v;
    int rc = param_save_locked();
    if (rc != 0) s_param.capture_h = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}
/* 保存 JPEG 质量；编码线程下一帧读取该值即可热更新。 */
int ipcam_param_set_jpeg_quality(uint8_t v)
{
    if (v < 1 || v > 100) return -1;
    pthread_mutex_lock(&s_mtx);
    uint8_t old = s_param.jpeg_quality;
    s_param.jpeg_quality = v;
    int rc = param_save_locked();
    if (rc != 0) s_param.jpeg_quality = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}
/* 保存目标帧率；真正是否可达由 V4L2 协商和软件选帧报告。 */
int ipcam_param_set_target_fps(uint8_t v)
{
    if (v == 0 || v > 60) return -1;
    pthread_mutex_lock(&s_mtx);
    uint8_t old = s_param.target_fps;
    s_param.target_fps = v;
    int rc = param_save_locked();
    if (rc != 0) s_param.target_fps = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}

/*
 * 以一组参数完成视频配置提交，避免逐字段保存造成中间状态。
 * 媒体重协商由上层控制器在确认硬件档位后执行；这里负责校验与持久化。
 */
int ipcam_param_set_video_group(uint16_t width, uint16_t height,
                                uint8_t target_fps, uint8_t jpeg_quality)
{
    if (width == 0 || width > 4096 || (width & 1) || height == 0 || height > 4096 ||
        target_fps == 0 || target_fps > 60 || jpeg_quality < 1 || jpeg_quality > 100)
        return -1;
    pthread_mutex_lock(&s_mtx);
    uint16_t old_w = s_param.capture_w;
    uint16_t old_h = s_param.capture_h;
    uint8_t old_fps = s_param.target_fps;
    uint8_t old_quality = s_param.jpeg_quality;
    s_param.capture_w = width;
    s_param.capture_h = height;
    s_param.target_fps = target_fps;
    s_param.jpeg_quality = jpeg_quality;
    int rc = param_save_locked();
    if (rc != 0) {
        s_param.capture_w = old_w;
        s_param.capture_h = old_h;
        s_param.target_fps = old_fps;
        s_param.jpeg_quality = old_quality;
    }
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}

/* 保存 HTTP 端口；端口重绑仍按旧语义要求重启服务。 */
int ipcam_param_set_http_port(uint16_t v)
{
    if (v == 0) return -1;
    pthread_mutex_lock(&s_mtx);
    uint16_t old = s_param.http_port;
    s_param.http_port = v;
    int rc = param_save_locked();
    if (rc != 0) s_param.http_port = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}
/* 保存监听地址范围开关；不在运行中偷偷重建监听 socket。 */
int ipcam_param_set_http_bind_local(uint8_t v)
{
    if (v > 1) return -1;
    pthread_mutex_lock(&s_mtx);
    uint8_t old = s_param.http_bind_local;
    s_param.http_bind_local = v ? 1 : 0;
    int rc = param_save_locked();
    if (rc != 0) s_param.http_bind_local = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}
/* 持久化日志等级并同步运行时阈值，失败不改变当前输出等级。 */
int ipcam_param_set_log_level(uint8_t v)
{
    if (v >= IPCAM_LOG_BUTT) return -1;
    pthread_mutex_lock(&s_mtx);
    uint8_t old = s_param.log_level;
    s_param.log_level = v;
    int rc = param_save_locked();
    if (rc != 0) s_param.log_level = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) {
        ipcam_log_setlevel((ipcam_log_level_t)v);
        ipcam_sys_take_snapshot();
    }
    return rc;
}

/* 布尔配置统一校验、加锁和持久化，避免各入口出现不同边界规则。 */
static int set_bool_field(uint8_t *field, uint8_t value)
{
    if (value > 1) return -1;
    pthread_mutex_lock(&s_mtx);
    uint8_t old = *field;
    *field = value;
    int rc = param_save_locked();
    if (rc != 0) *field = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}

/* 兼容单向水平翻转入口；新控制入口优先使用成对 setter。 */
int ipcam_param_set_mirror_horizontal(uint8_t v)
{
    return set_bool_field(&s_param.mirror_horizontal, v);
}

/* 兼容单向垂直翻转入口；新控制入口优先使用成对 setter。 */
int ipcam_param_set_mirror_vertical(uint8_t v)
{
    return set_bool_field(&s_param.mirror_vertical, v);
}

/* 翻转是同一组输出契约，成对保存避免水平/垂直设置产生中间代次。 */
int ipcam_param_set_mirror_pair(uint8_t horizontal, uint8_t vertical)
{
    if (horizontal > 1 || vertical > 1) return -1;
    pthread_mutex_lock(&s_mtx);
    uint8_t old_h = s_param.mirror_horizontal;
    uint8_t old_v = s_param.mirror_vertical;
    s_param.mirror_horizontal = horizontal;
    s_param.mirror_vertical = vertical;
    int rc = param_save_locked();
    if (rc != 0) {
        s_param.mirror_horizontal = old_h;
        s_param.mirror_vertical = old_v;
    }
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}

/* 持久化本地预览开关；显示线程按下一帧边界停止转换。 */
int ipcam_param_set_preview_enabled(uint8_t v)
{
    return set_bool_field(&s_param.preview_enabled, v);
}

/* 持久化正常背光亮度；熄屏的 0 仅是临时硬件状态。 */
int ipcam_param_set_backlight_percent(uint8_t v)
{
    if (v < 10 || v > 100) return -1;
    pthread_mutex_lock(&s_mtx);
    uint8_t old = s_param.backlight_percent;
    s_param.backlight_percent = v;
    int rc = param_save_locked();
    if (rc != 0) s_param.backlight_percent = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
}

/* 持久化允许的自动熄屏档位，0 表示永不熄屏。 */
int ipcam_param_set_screen_timeout_min(uint8_t v)
{
    if (v != 0 && v != 1 && v != 3 && v != 5 && v != 10) return -1;
    pthread_mutex_lock(&s_mtx);
    uint8_t old = s_param.screen_timeout_min;
    s_param.screen_timeout_min = v;
    int rc = param_save_locked();
    if (rc != 0) s_param.screen_timeout_min = old;
    pthread_mutex_unlock(&s_mtx);
    if (rc == 0) ipcam_sys_take_snapshot();
    return rc;
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

/* 复制参数快照后序列化，避免持有配置锁执行较慢的字符串转义和格式化。 */
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
        "\"jpeg_quality\":%u,\"target_fps\":%u,"
        "\"http_port\":%u,\"http_bind_local\":%u,"
        "\"log_level\":%u,"
        "\"mirror_horizontal\":%u,\"mirror_vertical\":%u,"
        "\"preview_enabled\":%u,\"backlight_percent\":%u,"
        "\"screen_timeout_min\":%u"
        "}\n",
        model_esc, swver_esc,
        p.net_mode, ssid_esc, apn_esc,
        p.capture_w, p.capture_h,
        p.jpeg_quality, p.target_fps,
        p.http_port, p.http_bind_local,
        p.log_level, p.mirror_horizontal, p.mirror_vertical,
        p.preview_enabled, p.backlight_percent, p.screen_timeout_min);
    return (n > 0 && (size_t)n < buf_sz) ? n : -1;
}

/* 将启动期参数快照输出到 stderr，便于板端无 HTTP 时核对实际配置。 */
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
    fprintf(stderr, "  jpeg_quality   : %u\n", p.jpeg_quality);
    fprintf(stderr, "  target_fps     : %u\n", p.target_fps);
    fprintf(stderr, "  http_port      : %u\n", p.http_port);
    fprintf(stderr, "  http_bind_local: %u\n", p.http_bind_local);
    fprintf(stderr, "  log_level      : %u\n", p.log_level);
    fprintf(stderr, "  mirror         : h=%u v=%u\n",
            p.mirror_horizontal, p.mirror_vertical);
    fprintf(stderr, "  preview        : %u\n", p.preview_enabled);
    fprintf(stderr, "  backlight      : %u%%\n", p.backlight_percent);
    fprintf(stderr, "  screen_timeout : %u min\n", p.screen_timeout_min);
    fprintf(stderr, "--- end param ---\n");
}
