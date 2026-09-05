#ifndef IPCAM_NET_4G_H
#define IPCAM_NET_4G_H

#include <sys/types.h>  /* pid_t */

/*
 * 4G 模组管理（基于 pppd 拨号）。
 * 通过 /dev/ttyUSB2 发 AT 命令确认模组在位 -> 配置 APN -> 拉起 pppd 子进程。
 */
typedef struct ipcam_net_4g_ctx_s {
    pid_t          pppd_pid;     /* -1 = 未启动；fork 时存，stop 时 SIGTERM + waitpid */
    char           apn[64];
    char           at_dev[64];   /* AT 命令设备，如 /dev/ttyUSB2 */
} ipcam_net_4g_ctx_t;

int  ipcam_net_4g_init(ipcam_net_4g_ctx_t *ctx, const char *apn, const char *at_dev);
int  ipcam_net_4g_start(ipcam_net_4g_ctx_t *ctx);
void ipcam_net_4g_stop(ipcam_net_4g_ctx_t *ctx);

#endif /* IPCAM_NET_4G_H */