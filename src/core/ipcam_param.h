#ifndef IPCAM_PARAM_H
#define IPCAM_PARAM_H

/*
 * BCF2 libmoparam 风格的运行时配置模块。
 *
 * 设计要点（参考 mo_param.h）：
 *   - #pragma pack(1) 紧凑二进制布局
 *   - magic + version + size + crc 头
 *   - 一组 get/set 访问器（运行时改完自动持久化）
 *   - 文件落盘（BCF2 用 MTD 分区；ipcam 用 /etc/ipcam.conf 普通文件）
 *   - 校验失败时回退到默认值（不阻塞启动）
 *
 * 与 BCF2 差异：
 *   - 二进制布局但不用 MTD，文件路径可配置（IPCAM_PARAM_PATH）
 *   - 字段少（一个 struct 容纳所有 ipcam 需要 runtime 改的项）
 */

#include <stddef.h>
#include <stdint.h>

/* 文件 magic 'IPCM' (little endian: 'M','C','P','I' -> 0x4950434D) */
#define IPCAM_PARAM_MAGIC      0x4950434D
#define IPCAM_PARAM_VERSION    0x00010000   /* 1.0 */
#define IPCAM_PARAM_PATH_DEF   "/etc/ipcam.conf"

/* net_mode 取值 */
#define IPCAM_NET_MODE_NONE    0
#define IPCAM_NET_MODE_4G      1
#define IPCAM_NET_MODE_WIFI    2

#pragma pack(1)
typedef struct ipcam_param_s {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t crc;

    /* system */
    char     model[32];
    char     swver[16];
    uint8_t  net_mode;          /* 0/1/2 见 IPCAM_NET_MODE_* */
    char     wifi_ssid[64];
    char     wifi_psk[64];
    char     apn[32];

    /* video */
    uint16_t capture_w;
    uint16_t capture_h;
    uint8_t  jpeg_quality;
    uint8_t  target_fps;

    /*
     * 输出（编码/交付）分辨率：0 = 跟随 capture（旁路缩放）。
     * 与 capture 独立设置，满足「sensor 有效分辨率不可改、客户要求更高
     * 输出分辨率规格」的场景（编码前 planar 域插值）。
     * 注意：这两个字段取自原 reserved 前 4 字节，结构体总大小不变，
     * 旧配置文件无需作废（旧文件这几字节为 0，恰好等于默认语义）。
     */
    uint16_t out_w;
    uint16_t out_h;

    /* http */
    uint16_t http_port;
    uint8_t  http_bind_local;   /* 1 = 绑 127.0.0.1；0 = 0.0.0.0 */

    /* log */
    uint8_t  log_level;

    uint8_t  reserved[28];  /* 原 32 字节，前 4 字节已划给 out_w/out_h */
} ipcam_param_t;
#pragma pack()

/* 初始化：从 path 加载（不存在或损坏 → 使用 ipcam_config.h 默认值） */
int  ipcam_param_init(const char *path);

/* 持久化：写到 path（写 .tmp 再 rename，原子替换） */
int  ipcam_param_save(void);

/* 当前参数指针（init 后才可用） */
const ipcam_param_t *ipcam_param_get(void);

/* Getters（线程安全只读视图） */
uint8_t  ipcam_param_get_net_mode(void);
const char *ipcam_param_get_wifi_ssid(void);
const char *ipcam_param_get_wifi_psk(void);
const char *ipcam_param_get_apn(void);
uint16_t ipcam_param_get_capture_w(void);
uint16_t ipcam_param_get_capture_h(void);
uint16_t ipcam_param_get_out_w(void);       /* 0 = 跟随采集分辨率 */
uint16_t ipcam_param_get_out_h(void);       /* 0 = 跟随采集分辨率 */
uint8_t  ipcam_param_get_jpeg_quality(void);
uint8_t  ipcam_param_get_target_fps(void);
uint16_t ipcam_param_get_http_port(void);
uint8_t  ipcam_param_get_http_bind_local(void);
uint8_t  ipcam_param_get_log_level(void);
const char *ipcam_param_get_model(void);
const char *ipcam_param_get_swver(void);

/* Setters（写入后自动 save；任意字段失败返回 -1） */
int ipcam_param_set_net_mode(uint8_t v);
int ipcam_param_set_wifi_ssid(const char *ssid);
int ipcam_param_set_wifi_psk(const char *psk);
int ipcam_param_set_apn(const char *apn);
int ipcam_param_set_capture_w(uint16_t v);  /* 偶数；2-4096 */
int ipcam_param_set_capture_h(uint16_t v);  /* 1-4096 */
int ipcam_param_set_out_w(uint16_t v);      /* 0 = 跟随；偶数；<=4096 */
int ipcam_param_set_out_h(uint16_t v);      /* 0 = 跟随；<=4096 */
int ipcam_param_set_jpeg_quality(uint8_t v);
int ipcam_param_set_target_fps(uint8_t v);
int ipcam_param_set_http_port(uint16_t v);
int ipcam_param_set_http_bind_local(uint8_t v);
int ipcam_param_set_log_level(uint8_t v);

/* 序列化为 JSON 字符串（snprintf 到 buf，返回写入字节数；buf 不足返回 -1） */
int ipcam_param_to_json(char *buf, size_t buf_sz);

/* 调试：dump 到 stderr */
void ipcam_param_dump(void);

#endif /* IPCAM_PARAM_H */
