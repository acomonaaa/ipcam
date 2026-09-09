#ifndef IPCAM_NET_4G_H
#define IPCAM_NET_4G_H

#include <sys/types.h>  /* pid_t */

/*
 * 4G 模组管理（基于 pppd 拨号）。
 * AT 设备和 PPP profile 由启动配置传入，避免把具体板型写死在服务中。
 */
typedef struct ipcam_net_4g_ctx_s {
    pid_t          pppd_pid;     /* -1 = 未启动；fork 时存，stop 时 SIGTERM + waitpid */
    char           apn[64];
    char           at_dev[64];   /* AT 命令设备 */
    char           ppp_peer[64]; /* /etc/ppp/peers 下的 profile 名称 */
} ipcam_net_4g_ctx_t;

int  ipcam_net_4g_init(ipcam_net_4g_ctx_t *ctx, const char *apn,
                       const char *at_dev, const char *ppp_peer);
int  ipcam_net_4g_start(ipcam_net_4g_ctx_t *ctx);
void ipcam_net_4g_stop(ipcam_net_4g_ctx_t *ctx);

#endif /* IPCAM_NET_4G_H */
