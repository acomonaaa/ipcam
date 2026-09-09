#ifndef IPCAM_LIGHT_H
#define IPCAM_LIGHT_H

/*
 * 板级补光适配层。
 *
 * 板端实际可能使用 LED class 的 brightness 节点，也可能由其它驱动导出
 * 一个可写的亮度节点；本模块只接受环境变量 IPCAM_LIGHT_PATH 指定的节点，
 * 不猜测 GPIO 编号。补光属于临时状态，不写入 ipcam_param。
 */

/* 返回当前板级补光节点是否可写。 */
int ipcam_light_available(void);

/* 写入 0～100 的补光百分比；只有写入并关闭成功才更新内存状态。 */
int ipcam_light_set_percent(int percent);

/* 返回最近一次成功写入的补光百分比，默认 0（开机关闭补光）。 */
int ipcam_light_get_percent(void);

#endif /* IPCAM_LIGHT_H */
