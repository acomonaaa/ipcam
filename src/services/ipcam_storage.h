#ifndef IPCAM_STORAGE_H
#define IPCAM_STORAGE_H

/*
 * SD 卡状态与格式化服务。
 *
 * 录像服务只需要“能否安全写入”和剩余空间，而 GUI 还需要设备、文件系统
 * 和挂载点等诊断信息；这些字段统一从 /proc/mounts 与 statvfs 读取，避免
 * 页面各自猜测 SD 卡是否真的挂载。格式化接口只接受已确认的块设备挂载，
 * 并且内部使用 fork/exec，不把设备路径拼进 shell 命令。
 */

#include <stddef.h>
#include <stdint.h>

typedef struct ipcam_storage_info_s {
    int mounted;
    int format_supported;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t available_bytes;
    char mount_path[256];
    char device[256];
    char fs_type[32];
    char status[128];
} ipcam_storage_info_t;

/* 读取指定存储根目录的挂载身份和空间快照；未挂载时返回 -1 但仍填充状态。 */
int ipcam_storage_get_info(const char *root, ipcam_storage_info_t *info);

/* 在明确二次确认后格式化 SD 卡并尝试重新挂载；失败时返回 -1 和中文原因。 */
int ipcam_storage_format(const char *root, char *message, size_t message_sz);

#endif /* IPCAM_STORAGE_H */
