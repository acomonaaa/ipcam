#define _GNU_SOURCE
/* 补光控制是独立硬件路径，沿用 BCF2 的模块化日志格式便于定位节点问题。 */
#define IPCAM_LOG_MODULE "LIGHT"

#include "ipcam_light.h"
#include "ipcam_log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 补光是临时运行状态；锁同时保护最近成功值，避免 HTTP 查询读到半更新状态。 */
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static int s_percent;

/* 仅使用板级显式配置的节点，拒绝把未知 GPIO 当作已验证能力。 */
int ipcam_light_available(void)
{
    const char *path = getenv("IPCAM_LIGHT_PATH");
    return path && *path && access(path, W_OK) == 0;
}

/* 将百分比映射到板级节点最大值；最大值由探测/部署配置提供。 */
int ipcam_light_set_percent(int percent)
{
    if (percent < 0 || percent > 100) return -1;
    const char *path = getenv("IPCAM_LIGHT_PATH");
    if (!path || !*path) return -1;

    int max_value = 100;
    const char *max_env = getenv("IPCAM_LIGHT_MAX");
    if (max_env && *max_env) max_value = atoi(max_env);
    if (max_value <= 0) return -1;

    int value = max_value * percent / 100;
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        MLOGW("补光节点 %s 打开失败: %s\n", path, strerror(errno));
        return -1;
    }
    char text[32];
    int n = snprintf(text, sizeof(text), "%d\n", value);
    if (n <= 0 || (size_t)n >= sizeof(text)) {
        close(fd);
        MLOGW("补光节点 %s 的最大值超出文本缓冲区\n", path);
        return -1;
    }
    /* sysfs brightness 节点通常不支持 fsync；写入完整文本并成功 close
     * 即作为本次板级控制的成功条件，避免把已生效的补光误报为失败。 */
    ssize_t written = write(fd, text, (size_t)n);
    int sync_ok = written == n;
    int close_ok = close(fd) == 0;
    if (!sync_ok || !close_ok) {
        MLOGW("补光节点 %s 写入失败\n", path);
        return -1;
    }

    pthread_mutex_lock(&s_mtx);
    s_percent = percent;
    pthread_mutex_unlock(&s_mtx);
    return 0;
}

/* 返回最近一次硬件写入成功的值；节点掉线时不伪造新的状态。 */
int ipcam_light_get_percent(void)
{
    pthread_mutex_lock(&s_mtx);
    int percent = s_percent;
    pthread_mutex_unlock(&s_mtx);
    return percent;
}
