/*
 * ipcam 使用的 LVGL 9.5 配置。
 *
 * 只保留应用需要覆盖的选项，其余选项继续使用 LVGL 自带默认值。这样升级
 * LVGL 9.x 时不必复制一份容易过期的完整模板；显示和输入硬件由
 * src/services/ipcam_lvgl.c 自己适配，所以不启用 LVGL 的 fbdev/evdev 后端。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 目标板已确认 framebuffer 为 16 bpp RGB565。 */
#define LV_COLOR_DEPTH 16

/* i.MX6ULL Linux 使用 glibc；避免使用默认的 64 KiB LVGL 内存池。 */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* 所有 LVGL API 只在 ipcam_lvgl 线程调用，不启用 LVGL 内部 OS 锁。 */
#define LV_USE_OS LV_OS_NONE

/* Cortex-A7 先使用通用软件渲染，暂不依赖 PXP/DRM/GPU 驱动。 */
#define LV_USE_DRAW_SW 1
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE

/* 端口自己处理 framebuffer 和触摸快照，避免两个线程抢读/抢写设备。 */
#define LV_USE_LINUX_FBDEV 0
#define LV_USE_EVDEV       0
#define LV_USE_LINUX_DRM   0
#define LV_USE_SDL         0
#define LV_USE_X11         0
#define LV_USE_WAYLAND     0

/* 运行期先关闭 LVGL 自带日志，统一使用 ipcam 的 MLOG 宏。 */
#define LV_USE_LOG 0

/* 最小页面和视频帧对象需要的控件。 */
#define LV_USE_IMAGE  1
#define LV_USE_LABEL  1
#define LV_USE_BUTTON 1

/* 保留常用布局/控件，后续接入 LCD 设计稿时无需重新调整配置。 */
#define LV_USE_BAR    1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_FLEX   1
#define LV_USE_GRID   1

/*
 * Montserrat 只作为 LVGL 默认回退字体和 LV_SYMBOL_* 图标字体；页面正文使用
 * src/ui/ipcam_ui_font_cjk_*.c 中按当前文案裁剪的 CJK 字库，并由
 * src/services/ipcam_lvgl.c 在创建 UI 时注入，避免把完整中文字库塞进默认配置。
 */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_TXT_ENC LV_TXT_ENC_UTF8

/* 不使用 observer/theme 等额外框架，减少 i.MX6ULL 首次移植的代码体积。 */
#define LV_USE_OBSERVER       0
#define LV_USE_THEME_DEFAULT  0
#define LV_USE_THEME_SIMPLE   0
#define LV_USE_THEME_MONO     0

#endif /* LV_CONF_H */
