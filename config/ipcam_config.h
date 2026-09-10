#ifndef IPCAM_CONFIG_H
#define IPCAM_CONFIG_H

/* ===== 编译期默认值；可在 Makefile 里 -D 覆盖 ===== */

#ifndef IPCAM_VERSION
#define IPCAM_VERSION          "0.1.0"
#endif

#ifndef IPCAM_MODEL
#define IPCAM_MODEL            "ipcam-imx6ull"
#endif

/* 摄像头采集分辨率（OV5640 输出 YUYV 4:2:2，每像素 2 字节） */
#ifndef IPCAM_CAPTURE_WIDTH
#define IPCAM_CAPTURE_WIDTH    640
#endif
#ifndef IPCAM_CAPTURE_HEIGHT
#define IPCAM_CAPTURE_HEIGHT   480
#endif

/* LCD 显示分辨率（fb0 由内核报告；当前设计和板级默认值为 800×480） */
#ifndef IPCAM_LCD_WIDTH
#define IPCAM_LCD_WIDTH        800
#endif
#ifndef IPCAM_LCD_HEIGHT
#define IPCAM_LCD_HEIGHT       480
#endif

/* 帧率上限（实际由 V4L2 S_PARM 决定，这里是软上限） */
#ifndef IPCAM_TARGET_FPS
#define IPCAM_TARGET_FPS       15
#endif

/* MJPEG 编码质量（1-100）；配合 4:2:0 采样默认取 60，优先保障板端直播延迟。 */
#ifndef IPCAM_JPEG_QUALITY
#define IPCAM_JPEG_QUALITY     60
#endif

/* HTTP 服务端口 */
#ifndef IPCAM_HTTP_PORT
#define IPCAM_HTTP_PORT        8080
#endif

/* 环形缓冲帧数（采集侧最多缓存 N 帧，超过则丢弃最旧） */
#ifndef IPCAM_RING_DEPTH
#define IPCAM_RING_DEPTH       4
#endif

/* 录像消费者使用独立队列，避免慢盘反压采集和 HTTP 直播。 */
#ifndef IPCAM_RECORD_RING_DEPTH
#define IPCAM_RECORD_RING_DEPTH 16
#endif

/* 录像只允许写入经过挂载检查的目录；可由板级配置覆盖。 */
#ifndef IPCAM_STORAGE_ROOT
#define IPCAM_STORAGE_ROOT     "/mnt/sdcard"
#endif
#ifndef IPCAM_RECORD_RESERVE_BYTES
#define IPCAM_RECORD_RESERVE_BYTES (128ULL * 1024ULL * 1024ULL)
#endif
#ifndef IPCAM_RECORD_SEGMENT_SECONDS
#define IPCAM_RECORD_SEGMENT_SECONDS 300
#endif
/* RIFF/AVI 的 32 位尺寸字段接近 4 GiB 时提前切段，避免生成不可播放文件。 */
#ifndef IPCAM_RECORD_MAX_SEGMENT_BYTES
#define IPCAM_RECORD_MAX_SEGMENT_BYTES (3ULL * 1024ULL * 1024ULL * 1024ULL)
#endif

/*
 * 摄像头设备节点：空字符串表示自动扫描 /dev/videoN 并选择 mx6s-csi；
 * IPCAM_VIDEO_DEV 环境变量可在板端显式覆盖路径，但不会跳过能力校验。
 */
#ifndef IPCAM_VIDEO_DEV
#define IPCAM_VIDEO_DEV        ""
#endif
#ifndef IPCAM_FB_DEV
#define IPCAM_FB_DEV           "/dev/fb0"
#endif

/*
 * Linux evdev 触摸节点：本板实测 Goodix 为 event1，event0 是电源键。
 * IPCAM_TOUCH_DEV 环境变量优先，用于不同 rootfs 的节点编号变化。
 */
#ifndef IPCAM_TOUCH_DEV
#define IPCAM_TOUCH_DEV        "/dev/input/event1"
#endif

/*
 * LVGL 9 默认接管 LCD 的 UI 合成；设置 IPCAM_LVGL=0 可回退到旧的
 * display 线程直接写屏，便于新 UI 首次部署失败时保留摄像头预览。
 */
#ifndef IPCAM_LVGL_ENABLE
#define IPCAM_LVGL_ENABLE      1
#endif
/* 局部绘制缓冲按行数限额，避免在 512 MiB 板上再分配一整屏临时显存。 */
#ifndef IPCAM_LVGL_BUFFER_LINES
#define IPCAM_LVGL_BUFFER_LINES 40
#endif

/* 网络模式：'4g' 或 'wifi' 或 'none'（none = 仅本地显示） */
#ifndef IPCAM_NET_MODE
#define IPCAM_NET_MODE         "none"
#endif

/* 4G APN（pppd 拨号用） */
#ifndef IPCAM_4G_APN
#define IPCAM_4G_APN           "cmnet"
#endif

/* 4G 设备和 PPP profile 可由环境变量 IPCAM_4G_AT_DEV/PEER 覆盖；
 * 宏只保留现有部署的兼容默认值，不再把板级路径散落在业务代码中。 */
#ifndef IPCAM_4G_AT_DEV
#define IPCAM_4G_AT_DEV        "/dev/ttyUSB2"
#endif
#ifndef IPCAM_4G_PPP_PEER
#define IPCAM_4G_PPP_PEER      "quectel"
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
