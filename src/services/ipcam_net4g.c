#define _GNU_SOURCE
#include "ipcam_net4g.h"
#include "ipcam_log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <wait.h>

#include <net/if.h>
#include <sys/socket.h>

#include "ipcam_config.h"

/*
 * 注：本实现是简化版"够用即可"。生产环境请用 quectel-CM（含 QMI WWAN）。
 *
 * 流程：
 *   1) 打开配置的 AT 设备，termios 115200 8N1 + blocking，发 AT
 *   2) select() 等 OK/ERROR，超时 timeout_ms
 *   3) AT+CFUN=1, AT+CGDCONT=1,"IP","<apn>"
 *   4) fork+exec pppd call <configured-peer>
 *   5) 周期性查 default route dev ppp0
 */

static int at_open(const char *dev, int *fd_out)
{
    int fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    /* CLOCAL: 忽略 modem DCD；CREAD: 启用接收 */
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tio.c_cflag |= CS8;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        MLOGE("tcsetattr %s: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }

    *fd_out = fd;
    return 0;
}

/*
 * 用 select() 等响应。读出来塞 buf；遇到 \nOK\n 或 \nERROR\n 即返回。
 * 超时返回 -1；buf 里的内容 caller 用完即弃。
 */
static int at_send_wait(int fd, const char *cmd, const char *expect, int timeout_ms)
{
    if (write(fd, cmd, strlen(cmd)) < 0) return -1;

    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    char buf[512];
    size_t got = 0;
    int rc = -1;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int sr = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sr == 0) return -1;  /* timeout */
        ssize_t n = read(fd, buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) return -1;
        got += (size_t)n;
        buf[got] = '\0';
        /* URC 行以 \r\n 开头，跳过；命令回显同样 */
        if (expect && strstr(buf, expect)) { rc = 0; break; }
        if (strstr(buf, "\nERROR") || strstr(buf, "\rERROR")) { rc = -1; break; }
    }
    /* 尽力把 buffer 排空（防止下次读阻塞） */
    {
        struct timeval drain_tv = { 0, 50 * 1000 };
        fd_set rfds;
        FD_ZERO(&rfds); FD_SET(fd, &rfds);
        while (select(fd + 1, &rfds, NULL, NULL, &drain_tv) > 0) {
            char tmp[128];
            if (read(fd, tmp, sizeof(tmp)) <= 0) break;
        }
    }
    return rc;
}

static int at_check_modem(const char *dev)
{
    int fd;
    if (at_open(dev, &fd) < 0) return -1;
    int rc = at_send_wait(fd, "AT\r\n", "OK", 1500);
    close(fd);
    return rc;
}

static int at_set_apn(const char *dev, const char *apn)
{
    /* APN 会拼进 AT+CGDCONT 字符串，禁止引号与控制字符，防止额外 AT 命令注入 */
    if (!apn || !*apn) return -1;
    for (const unsigned char *p = (const unsigned char *)apn; *p; p++) {
        if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p < 0x20)
            return -1;
    }

    int fd;
    if (at_open(dev, &fd) < 0) return -1;

    int rc = -1;
    if (at_send_wait(fd, "AT+CFUN=1\r\n", "OK", 5000) < 0) goto done;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"\r\n", apn);
    rc = at_send_wait(fd, cmd, "OK", 3000);
done:
    close(fd);
    return rc;
}

/* /proc/net/route 解析：找 default route dev 是否匹配 ifname */
static int has_default_route(const char *ifname)
{
    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp) return 0;
    char line[256];
    int found = 0;
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    while (fgets(line, sizeof(line), fp)) {
        char ifname_buf[32];
        unsigned int dest, gw;
        if (sscanf(line, "%31s %x %x", ifname_buf, &dest, &gw) != 3) continue;
        if (dest == 0 && strcmp(ifname_buf, ifname) == 0) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

int ipcam_net_4g_init(ipcam_net_4g_ctx_t *ctx, const char *apn,
                      const char *at_dev, const char *ppp_peer)
{
    memset(ctx, 0, sizeof(*ctx));
    if (apn) strncpy(ctx->apn, apn, sizeof(ctx->apn) - 1);
    if (at_dev) snprintf(ctx->at_dev, sizeof(ctx->at_dev), "%s", at_dev);
    else snprintf(ctx->at_dev, sizeof(ctx->at_dev), "%s", IPCAM_4G_AT_DEV);
    /* 环境覆盖值可能来自启动脚本；snprintf 保证截断后仍有 NUL 结尾，
     * 避免异常长配置把后续 execlp 参数读出数组边界。 */
    if (ppp_peer) snprintf(ctx->ppp_peer, sizeof(ctx->ppp_peer), "%s", ppp_peer);
    else snprintf(ctx->ppp_peer, sizeof(ctx->ppp_peer), "%s", IPCAM_4G_PPP_PEER);
    ctx->pppd_pid = -1;
    return 0;
}

int ipcam_net_4g_start(ipcam_net_4g_ctx_t *ctx)
{
    if (at_check_modem(ctx->at_dev) < 0) {
        MLOGE("4G modem at %s not responding (AT failed)\n", ctx->at_dev);
        return -1;
    }
    MLOGI("4G modem at %s OK\n", ctx->at_dev);

    if (at_set_apn(ctx->at_dev, ctx->apn) < 0) {
        MLOGE("4G APN set failed\n");
        return -1;
    }
    MLOGI("4G APN set to %s\n", ctx->apn);

    pid_t pid = fork();
    if (pid < 0) {
        MLOGE("fork pppd: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* 子进程：exec pppd；exec 失败直接退出（parent 通过 waitpid 看到非 0 status） */
        execlp("pppd", "pppd", "call", ctx->ppp_peer, "-detach", (char *)NULL);
        _exit(127);
    }

    ctx->pppd_pid = pid;
    MLOGI("pppd forked, pid=%d, waiting for default route...\n", (int)pid);

    /* 等待 ppp0 默认路由出现（最长 30s）；同时轮询 child 状态以检测 execlp 失败 */
    for (int i = 0; i < 30; i++) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) {
                MLOGE("pppd exited code=%d before route came up\n", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                MLOGE("pppd killed by signal %d before route came up\n", WTERMSIG(status));
            }
            ctx->pppd_pid = -1;
            return -1;
        }
        if (has_default_route("ppp0")) {
            MLOGI("ppp0 default route up\n");
            return 0;
        }
        sleep(1);
    }
    MLOGW("ppp0 default route did not appear in 30s\n");
    return 0;  /* 不致命：用户可后续重试 */
}

void ipcam_net_4g_stop(ipcam_net_4g_ctx_t *ctx)
{
    if (ctx->pppd_pid <= 0) return;
    MLOGI("stopping pppd pid=%d\n", (int)ctx->pppd_pid);

    /* 只杀我们自己的 pppd，不影响别人 */
    if (kill(ctx->pppd_pid, SIGTERM) == 0) {
        int status;
        /* 给 5 秒优雅退出，超时再 SIGKILL */
        for (int i = 0; i < 50; i++) {
            pid_t r = waitpid(ctx->pppd_pid, &status, WNOHANG);
            if (r == ctx->pppd_pid) goto done;
            if (r < 0 && errno != EINTR) break;
            usleep(100 * 1000);
        }
        MLOGW("pppd did not exit in 5s, sending SIGKILL\n");
        kill(ctx->pppd_pid, SIGKILL);
        waitpid(ctx->pppd_pid, &status, 0);
    }
done:
    ctx->pppd_pid = -1;
    MLOGI("4G stopped\n");
}
