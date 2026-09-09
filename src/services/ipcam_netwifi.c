#define _GNU_SOURCE
/* WiFi 配置、进程和接口生命周期日志归入 WIFI 模块。 */
#define IPCAM_LOG_MODULE "WIFI"
#include "ipcam_netwifi.h"
#include "ipcam_log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ipcam_config.h"

/*
 * 用 wpa_passphrase 算 PSK（避免明文）；最终 conf 文件权限 0600。
 *
 * 通过 fork+exec+pipe 调用 wpa_passphrase，不走 shell —— 避免 SSID/PSK
 * 包含单引号/反引号/$() 等字符时的命令注入。
 *
 * 失败降级：写明文（注释提醒生产改用 wpa_passphrase）。
 */
static int run_wpa_passphrase(const char *ssid, const char *psk, char *out, size_t out_sz)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* 子：关读端，把 stdout 重定向到 pipe 写端 */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        /* 不走 shell：argv 单独传，wpa_passphrase 接受 ssid 和 passphrase 作为 argv[1]/argv[2] */
        execlp("wpa_passphrase", "wpa_passphrase", ssid, psk, (char *)NULL);
        _exit(127);
    }

    /* 父：从 pipe 读 wpa_passphrase 的输出 */
    close(pipefd[1]);
    size_t got = 0;
    while (got < out_sz - 1) {
        ssize_t n = read(pipefd[0], out + got, out_sz - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    out[got] = '\0';
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

static int write_wpa_conf(const char *path, const char *ssid, const char *psk)
{
    /* 即使 setter 已过滤，写明文 conf 前再拦一遍，防止其它路径绕过注入 */
    {
        const unsigned char *s;
        for (s = (const unsigned char *)ssid; *s; s++) {
            if (*s == '"' || *s == '\\' || *s == '\n' || *s == '\r' || *s < 0x20) {
                MLOGE("SSID contains unsafe characters\n");
                return -1;
            }
        }
        for (s = (const unsigned char *)psk; *s; s++) {
            if (*s == '"' || *s == '\\' || *s == '\n' || *s == '\r' || *s < 0x20) {
                MLOGE("PSK contains unsafe characters\n");
                return -1;
            }
        }
    }

    char passphrase_out[4096];
    int used_passphrase = (run_wpa_passphrase(ssid, psk, passphrase_out, sizeof(passphrase_out)) == 0);

    FILE *out = fopen(path, "w");
    if (!out) {
        MLOGE("open %s: %s\n", path, strerror(errno));
        return -1;
    }
    fputs("ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=root\n"
          "update_config=1\n"
          "country=CN\n\n",
          out);

    if (used_passphrase) {
        /* 复制 wpa_passphrase 输出的 network={...}，跳过明文 #psk= 注释 */
        char *p = passphrase_out;
        int in_net = 0;
        char *line_start = p;
        for (; *p; p++) {
            if (*p == '\n') {
                /* 处理 [line_start, p] 一行 */
                char saved = *(p + 1);
                *(p + 1) = '\0';
                if (strncmp(line_start, "network={", 9) == 0) in_net = 1;
                if (in_net && strncmp(line_start, "#psk=", 5) == 0) {
                    /* 跳过明文注释 */
                } else {
                    fputs(line_start, out);
                }
                if (in_net && strchr(line_start, '}')) in_net = 0;
                *(p + 1) = saved;
                line_start = p + 1;
            }
        }
    } else {
        /* 降级：明文 + 警告（未来 web UI 接入时必须改成 PBKDF2 计算或 fork+exec 传 argv） */
        fprintf(out,
                "# WARNING: wpa_passphrase unavailable; using plaintext PSK.\n"
                "# Install wpa_passphrase for production.\n"
                "network={\n"
                "    ssid=\"%s\"\n"
                "    psk=\"%s\"\n"
                "    key_mgmt=WPA-PSK\n"
                "}\n",
                ssid, psk);
    }
    fclose(out);
    chmod(path, 0600);
    return 0;
}

int ipcam_net_wifi_init(ipcam_net_wifi_ctx_t *ctx, const char *ssid, const char *psk,
                        const char *ifname)
{
    memset(ctx, 0, sizeof(*ctx));
    if (ssid) strncpy(ctx->ssid, ssid, sizeof(ctx->ssid) - 1);
    if (psk)  strncpy(ctx->psk,  psk,  sizeof(ctx->psk)  - 1);
    snprintf(ctx->ifname, sizeof(ctx->ifname), "%s", ifname ? ifname : "wlan0");
    ctx->sup_pid = -1;
    ctx->dhcp_pid = -1;
    return 0;
}

int ipcam_net_wifi_start(ipcam_net_wifi_ctx_t *ctx)
{
    if (ctx->ssid[0] == '\0') {
        MLOGE("WiFi SSID empty, skip\n");
        return -1;
    }

    if (write_wpa_conf("/etc/wpa_supplicant.conf", ctx->ssid, ctx->psk) < 0) {
        MLOGE("write /etc/wpa_supplicant.conf failed\n");
        return -1;
    }
    MLOGI("wpa_supplicant.conf written (mode 0600)\n");

    pid_t pid = fork();
    if (pid < 0) {
        MLOGE("fork wpa_supplicant: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* 前台运行：由父进程跟踪真实 PID；勿加 -B，否则子进程立刻退出会被误判为启动失败 */
        execlp("wpa_supplicant", "wpa_supplicant",
               "-i", ctx->ifname,
               "-c", "/etc/wpa_supplicant.conf",
               (char *)NULL);
        _exit(127);
    }
    ctx->sup_pid = pid;
    MLOGI("wpa_supplicant forked, pid=%d\n", (int)pid);

    sleep(2);
    pid_t dhcp_pid = fork();
    if (dhcp_pid == 0) {
        execlp("udhcpc", "udhcpc", "-i", ctx->ifname, "-t", "5", "-q", (char *)NULL);
        _exit(127);
    }
    ctx->dhcp_pid = dhcp_pid;

    /* 验证子进程没立刻死掉（execlp 失败会立即 exit(127)） */
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
        MLOGE("wpa_supplicant exited immediately (status=%d), likely exec failed\n", status);
        ctx->sup_pid = -1;
        return -1;
    }

    MLOGI("WiFi starting: ssid=%s ifname=%s\n", ctx->ssid, ctx->ifname);
    return 0;
}

void ipcam_net_wifi_stop(ipcam_net_wifi_ctx_t *ctx)
{
    if (ctx->dhcp_pid > 0) {
        kill(ctx->dhcp_pid, SIGTERM);
        waitpid(ctx->dhcp_pid, NULL, 0);
        ctx->dhcp_pid = -1;
    }
    if (ctx->sup_pid > 0) {
        kill(ctx->sup_pid, SIGTERM);
        waitpid(ctx->sup_pid, NULL, 0);
        ctx->sup_pid = -1;
    }
    MLOGI("WiFi stopped\n");
}
