#ifndef IPCAM_UI_FONTS_H
#define IPCAM_UI_FONTS_H

/*
 * IPCam 固定界面的静态 CJK 字体声明。
 *
 * 字体由 scripts/generate_ui_fonts.sh 从当前 src/ui 文案裁剪生成并随 ipcam
 * 一起链接，不在 i.MX6ULL 上运行时解析 OTF/TTF；这样启动不依赖 NFS 中的额外
 * 资源文件，且只为实际界面字符保留字形。LVGL 9 的字体描述符是只读数据。
 */
#include <lvgl.h>

extern const lv_font_t ipcam_font_cjk_14;
extern const lv_font_t ipcam_font_cjk_16;
extern const lv_font_t ipcam_font_cjk_20;

#endif /* IPCAM_UI_FONTS_H */
