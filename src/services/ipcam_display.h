#ifndef IPCAM_DISPLAY_H
#define IPCAM_DISPLAY_H

#include <signal.h>     /* sig_atomic_t */
#include <pthread.h>    /* pthread_t */
#include <stddef.h>     /* size_t */
#include <stdint.h>
#include <linux/fb.h>
#include "ipcam_ringbuffer.h"
#include "ipcam_perf.h"

/*
 * LCD 显示线程：从环形缓冲读 YUYV 帧，转换为 RGB565，写入 /dev/fb0 mmap。
 * 颜色转换用软件算法（参考 11/19_lcd/lcd_test.c 的 argb8888_to_rgb565 宏风格）。
 *
 * 当前仅支持 RGB565 16bpp framebuffer；其他 bpp 在启动时返回 -1。
 */
typedef struct ipcam_display_perf_s {
    uint64_t frames_rendered;
    uint64_t stale_input_frames;
    uint64_t convert_avg_ns;
    uint64_t convert_p95_ns;
    uint64_t convert_max_ns;
    uint64_t pan_count;
    uint64_t pan_failures;
    uint64_t pan_avg_ns;
    uint64_t pan_p95_ns;
    uint64_t pan_max_ns;
    int framebuffer_pan_enabled;
    int framebuffer_selftest_passed;
    int framebuffer_fault;
    int framebuffer_active_page;
    char framebuffer_mode[24];
} ipcam_display_perf_t;

typedef struct ipcam_display_ctx_s {
    int      fb_fd;
    int      fb_w;
    int      fb_h;
    int      fb_bpp;
    int      fb_xoffset;
    int      fb_yoffset;
    int      fb_yres_virtual;
    int      fb_line_length;    /* finfo.line_length；0 表示未设置 */
    unsigned short *fb_base;    /* mmap 后的帧缓冲基址（RGB565 16bpp） */
    size_t   fb_size;
    size_t   fb_page_size;      /* 单个可见页的字节数，用于双缓冲切页 */
    int      fb_pan_enabled;    /* 1 表示驱动接受双页 FBIOPAN_DISPLAY */
    int      fb_active_page;    /* 最近一次提交给 LCD 的虚拟页 */
    int      fb_selftest_passed;
    int      fb_fault;
    uint64_t fb_pan_count;
    unsigned int fb_pan_failures;
    int      fb_pending_page;
    uint64_t fb_last_retry_ns;
    char     fb_mode[24];       /* double-buffer / partial-degraded */
    struct fb_var_screeninfo fb_var_template;
    int      fb_var_template_valid;
    pthread_mutex_t fb_mtx;     /* 串行化 VSYNC 切页、blank 和兼容拷贝 */
    int      dynamic_video_enabled; /* 仅双页自检成功后允许本地动态视频 */

    /* 目标显示尺寸（一般是 LCD 全屏） */
    int      out_w;
    int      out_h;

    /* 协商后的源 YUYV 尺寸（capture 传入） */
    int      src_w;
    int      src_h;

    ipcam_ring_buffer_t *rb;    /* 读取端 */
    volatile sig_atomic_t *running;
    volatile sig_atomic_t service_running; /* 仅显示服务自身的生命周期 */
    pthread_t  thread;
    pthread_mutex_t view_mtx;
    pthread_mutex_t preview_mtx; /* 保护供 GUI 复制的最新 RGB565 帧 */
    pthread_mutex_t stats_mtx;    /* 保护已渲染帧累计值 */
    uint64_t       frames_rendered;
    volatile sig_atomic_t framebuffer_writer_enabled;
    unsigned short  *preview_base;
    unsigned short  *preview_work;
    size_t           preview_size;       /* 当前目标尺寸的有效字节数 */
    size_t           preview_capacity;   /* preview_base 实际分配容量 */
    ipcam_frame_t    preview_frame;
    int              preview_valid;
    int              preview_w;
    int              preview_h;
    int              preview_stride_pixels;
    uint64_t         preview_target_generation;
    uint32_t         *map_x_pair_offset;
    uint8_t          *map_x_luma_offset;
    int              *map_y_source;
    int              map_valid;
    int              map_dw;
    int              map_dh;
    int              map_sw;
    int              map_sh;
    int              map_crop_x;
    int              map_crop_y;
    int              map_crop_w;
    int              map_crop_h;
    int              map_mirror_h;
    int              map_mirror_v;
    ipcam_perf_window_t convert_window;
    ipcam_perf_window_t pan_window;
    uint64_t         stale_input_frames;
    int              view_enabled;
    int              screen_paused;
    float            zoom;
    float            center_x;
    float            center_y;

    /* 背光节点由环境变量或 /sys/class/backlight 自动探测；无节点时使用 fb blank。 */
    char             backlight_path[256];
    int              backlight_max;
    int              backlight_available;
    int              fb_blank_available;
} ipcam_display_ctx_t;

/* 兼容旧调用方：直接由 display 线程写 framebuffer。 */
int  ipcam_display_start(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                         int src_w, int src_h,
                         volatile sig_atomic_t *running);
/* LVGL 模式下只生成 RGB565 预览副本，把 framebuffer 写入交给 LVGL flush。 */
int  ipcam_display_start_ex(ipcam_display_ctx_t *ctx, ipcam_ring_buffer_t *rb,
                            int src_w, int src_h, int framebuffer_writer,
                            volatile sig_atomic_t *running);
void ipcam_display_stop(ipcam_display_ctx_t *ctx);

/* 在 LVGL 初始化失败时切回旧的直接写屏路径，避免 LCD 因 UI 能力缺失而黑屏。 */
void ipcam_display_set_framebuffer_writer(ipcam_display_ctx_t *ctx, int enabled);

/* 设置本地观察视口；zoom 限制在 1～4，中心坐标为源图像归一化坐标。 */
int ipcam_display_set_view(ipcam_display_ctx_t *ctx, int enabled,
                           float zoom, float center_x, float center_y);
/* 设置本地预览的实际转换尺寸；0×0 表示当前页面不需要转换视频。 */
int ipcam_display_set_preview_target(ipcam_display_ctx_t *ctx,
                                     int width, int height);
/* 熄屏期间暂停 YUYV→RGB 转换但保留用户视口，唤醒时恢复原视图。 */
int ipcam_display_set_screen_paused(ipcam_display_ctx_t *ctx, int paused);
void ipcam_display_get_view(ipcam_display_ctx_t *ctx, int *enabled,
                            float *zoom, float *center_x, float *center_y);
/* 读取本地预览已渲染帧累计值，供 5 秒性能汇总计算实际帧率。 */
void ipcam_display_get_stats(ipcam_display_ctx_t *ctx, uint64_t *rendered);
/* 返回固定容量性能快照；不访问网络、存储或 framebuffer ioctl。 */
void ipcam_display_get_perf(ipcam_display_ctx_t *ctx, ipcam_display_perf_t *out);
/* 复制最新 RGB565 预览帧；调用方提供 out_data，成功后无需释放 ring 槽。 */
int ipcam_display_preview_acquire(ipcam_display_ctx_t *ctx,
                                  void *out_data, size_t out_cap,
                                  ipcam_frame_t *out_frame);
void ipcam_display_preview_release(ipcam_display_ctx_t *ctx,
                                   ipcam_frame_t *frame);
/* 返回当前是否存在 sysfs 调光或 FBIOBLANK 亮灭能力；后者仅提供二态显示。 */
int ipcam_display_backlight_available(ipcam_display_ctx_t *ctx);
/* 通过自动探测/环境变量的背光节点设置亮度；无节点时 0/非 0 使用 FBIOBLANK。 */
int ipcam_display_set_backlight(ipcam_display_ctx_t *ctx, int percent);
/* 兼容旧调用方：使用当前唯一 display 实例的背光控制。 */
int ipcam_display_set_backlight_percent(int percent);

/* LVGL 双 framebuffer 适配：返回指定虚拟页地址，失败返回 NULL。 */
void *ipcam_display_framebuffer_page(ipcam_display_ctx_t *ctx, int page);
/* 只在最后一个 LVGL flush 区域调用，驱动负责在下一次 VSYNC 切换页面。 */
int ipcam_display_present_page(ipcam_display_ctx_t *ctx, int page);
/* pan 失败后最多每秒重试一次；成功会清除 fault，失败不做可见页 memcpy。 */
int ipcam_display_retry_pan(ipcam_display_ctx_t *ctx);
/* 查询 pan 故障；LVGL 故障期间暂停提交新绘制，避免写入当前扫描页。 */
int ipcam_display_pan_fault(ipcam_display_ctx_t *ctx);
/* 复制当前硬件可见页，熄屏提示只在状态边沿调用一次。 */
int ipcam_display_snapshot_visible(ipcam_display_ctx_t *ctx,
                                    void *out_data, size_t out_cap);
/* framebuffer 不支持切页时的兼容路径：把完整页拷贝到当前可见窗口。 */
int ipcam_display_copy_page_to_visible(ipcam_display_ctx_t *ctx, int page);
/* 单缓冲兼容模式的局部写屏，内部处理 line_length 与 framebuffer offset。 */
int ipcam_display_blit_area(ipcam_display_ctx_t *ctx, int x1, int y1,
                             int x2, int y2, const uint8_t *color_p);

#endif /* IPCAM_DISPLAY_H */
