#ifndef IPCAM_NET_WIFI_H
#define IPCAM_NET_WIFI_H

#include <sys/types.h>  /* pid_t */

/*
 * WiFi 模组管理（RTL8188EUS 等支持 nl80211 的驱动）。
 * 启动 wpa_supplicant，配置 SSID/PSK；udhcpc 自动获取 IP。
 */
typedef struct ipcam_net_wifi_ctx_s {
    pid_t  sup_pid;       /* wpa_supplicant pid */
    pid_t  dhcp_pid;      /* udhcpc pid */
    char  ssid[64];
    char  psk[64];
    char  ifname[16];     /* 默认 wlan0 */
} ipcam_net_wifi_ctx_t;

int  ipcam_net_wifi_init(ipcam_net_wifi_ctx_t *ctx, const char *ssid, const char *psk,
                         const char *ifname);
int  ipcam_net_wifi_start(ipcam_net_wifi_ctx_t *ctx);
void ipcam_net_wifi_stop(ipcam_net_wifi_ctx_t *ctx);

#endif /* IPCAM_NET_WIFI_H */