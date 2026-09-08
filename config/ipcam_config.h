#ifndef IPCAM_CONFIG_H
#define IPCAM_CONFIG_H

/* ===== 编译期默认值；可在 Makefile 里 -D 覆盖 ===== */

#ifndef IPCAM_VERSION
#define IPCAM_VERSION          "0.1.0"
#endif

#ifndef IPCAM_MODEL
#define IPCAM_MODEL            "ipcam-imx6ull"
#endif

/* 摄像头采集分辨率（OV5640 输出 packed 4:2:2，每像素 2 字节） */
#ifndef IPCAM_CAPTURE_WIDTH
#define IPCAM_CAPTURE_WIDTH    640
#endif
#ifndef IPCAM_CAPTURE_HEIGHT
#define IPCAM_CAPTURE_HEIGHT   480
#endif

/*
 * 采集像素格式字节序（YUYV/UYVY 帧大小相同，均为 w*h*2 字节）：
 *   0 = YUYV（字节序 Y0 Cb Y1 Cr；正点原子出厂 OV5640 驱动已验证支持）
 *   1 = UYVY（字节序 Cb Y0 Cr Y1；保留给其它 sensor/BSP，不是当前出厂链路的默认值）
 * capture 会在 S_FMT 后再次 G_FMT，并拒绝驱动返回的其它格式，避免 display/encode
 * 按错误字节序解释帧数据；切换到其它硬件时必须以板上实际协商结果为准。
 */
#ifndef IPCAM_CAP_PIXFMT
#define IPCAM_CAP_PIXFMT       0
#endif

/*
 * 输出（编码/交付）分辨率：0 = 跟随采集分辨率（旁路缩放）。
 * 传感器有效分辨率由硬件决定、不可更改；当客户/产品规格要求更高的
 * 输出分辨率数字时，编码前在 planar 域做插值上采样满足规格（实际
 * 画质不变）。Y 平面双线性、色度平面最临近，见 ipcam_encode.c。
 * 运行时经 /api/config 的 out_w/out_h 修改（重启生效），0 同样表示跟随。
 */
#ifndef IPCAM_OUTPUT_WIDTH
#define IPCAM_OUTPUT_WIDTH     0
#endif
#ifndef IPCAM_OUTPUT_HEIGHT
#define IPCAM_OUTPUT_HEIGHT    0
#endif

/* LCD 显示分辨率（fb0 由内核报告；这里只是默认裁剪/缩放目标） */
#ifndef IPCAM_LCD_WIDTH
#define IPCAM_LCD_WIDTH        1024
#endif
#ifndef IPCAM_LCD_HEIGHT
#define IPCAM_LCD_HEIGHT       600
#endif

/* 帧率上限（实际由 V4L2 S_PARM 决定，这里是软上限） */
#ifndef IPCAM_TARGET_FPS
#define IPCAM_TARGET_FPS       15
#endif

/* MJPEG 编码质量（1-100，越高越清晰越慢） */
#ifndef IPCAM_JPEG_QUALITY
#define IPCAM_JPEG_QUALITY     75
#endif

/* HTTP 服务端口 */
#ifndef IPCAM_HTTP_PORT
#define IPCAM_HTTP_PORT        8080
#endif

/* 环形缓冲帧数（采集侧最多缓存 N 帧，超过则丢弃最旧） */
#ifndef IPCAM_RING_DEPTH
#define IPCAM_RING_DEPTH       4
#endif

/*
 * 设备节点：空字符串表示自动扫描 /dev/videoN，并选择 mx6s-csi 采集节点；
 * 非空时作为显式覆盖路径，但仍会执行 V4L2 capability 和像素格式检查。
 */
#ifndef IPCAM_VIDEO_DEV
#define IPCAM_VIDEO_DEV        ""
#endif
#ifndef IPCAM_FB_DEV
#define IPCAM_FB_DEV           "/dev/fb0"
#endif

/* 网络模式：'4g' 或 'wifi' 或 'none'（none = 仅本地显示） */
#ifndef IPCAM_NET_MODE
#define IPCAM_NET_MODE         "none"
#endif

/* 4G APN（pppd 拨号用） */
#ifndef IPCAM_4G_APN
#define IPCAM_4G_APN           "cmnet"
#endif

/* WiFi SSID/PWD（运行时可改；编译期仅供默认） */
#ifndef IPCAM_WIFI_SSID
#define IPCAM_WIFI_SSID        ""
#endif
#ifndef IPCAM_WIFI_PSK
#define IPCAM_WIFI_PSK         ""
#endif

/* 调试日志等级（对应 log.h 的 ipcam_log_level_t）：
 *   0=FATAL  1=PRINT  2=ERROR  3=WARNING  4=INFO  5=DEBUG */
#ifndef IPCAM_LOG_LEVEL
#define IPCAM_LOG_LEVEL        4   /* default: INFO */
#endif

#endif /* IPCAM_CONFIG_H */
