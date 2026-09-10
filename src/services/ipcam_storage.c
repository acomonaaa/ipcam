#define _GNU_SOURCE

/* SD 卡挂载、空间和格式化日志归入 STOR 模块。 */
#define IPCAM_LOG_MODULE "STOR"
#include "ipcam_storage.h"
#include "ipcam_config.h"
#include "ipcam_log.h"

#include <errno.h>
#include <mntent.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum storage_format_kind_e {
    STORAGE_FORMAT_NONE = 0,
    STORAGE_FORMAT_VFAT,
    STORAGE_FORMAT_EXT4,
    STORAGE_FORMAT_EXFAT
} storage_format_kind_t;

static void storage_message(char *message, size_t message_sz, const char *text)
{
    if (!message || message_sz == 0) return;
    snprintf(message, message_sz, "%s", text ? text : "");
}

/* 复制挂载表字段；getmntent 返回的指针会在下一次读取时复用。 */
static void copy_mount_field(char *out, size_t out_sz, const char *value)
{
    if (!out || out_sz == 0) return;
    snprintf(out, out_sz, "%s", value ? value : "");
}

/* 获取与 root 完全一致的挂载条目，避免把父目录或 bind mount 误当 SD 卡。 */
static int storage_mount_entry(const char *root, char *device, size_t device_sz,
                               char *fs_type, size_t fs_type_sz)
{
    if (device && device_sz > 0) device[0] = '\0';
    if (fs_type && fs_type_sz > 0) fs_type[0] = '\0';
    if (!root || !*root) return -1;

    FILE *mounts = setmntent("/proc/mounts", "r");
    if (!mounts) return -1;
    struct mntent *entry;
    int found = -1;
    while ((entry = getmntent(mounts)) != NULL) {
        if (strcmp(entry->mnt_dir, root) != 0) continue;
        copy_mount_field(device, device_sz, entry->mnt_fsname);
        copy_mount_field(fs_type, fs_type_sz, entry->mnt_type);
        found = 0;
        break;
    }
    endmntent(mounts);
    return found;
}

/* 设备号必须与父目录不同，才能证明 root 没有在 SD 未插入时落到根分区。 */
static int storage_is_independent_mount(const char *root, char *error,
                                        size_t error_sz)
{
    if (!root || !*root) {
        storage_message(error, error_sz, "存储路径为空");
        return -1;
    }
    if (root[0] != '/' || strcmp(root, "/") == 0 || strstr(root, "/../") != NULL) {
        storage_message(error, error_sz, "拒绝使用不安全的存储路径");
        return -1;
    }

    struct stat root_stat, parent_stat;
    char parent[256];
    int n = snprintf(parent, sizeof(parent), "%s/..", root);
    if (n < 0 || (size_t)n >= sizeof(parent)) {
        storage_message(error, error_sz, "存储路径过长");
        return -1;
    }
    if (stat(root, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
        snprintf(error, error_sz, "存储目录不可用: %s", root);
        return -1;
    }
    if (stat(parent, &parent_stat) != 0 || root_stat.st_dev == parent_stat.st_dev) {
        snprintf(error, error_sz, "存储目录未挂载: %s", root);
        return -1;
    }
    return 0;
}

static int multiply_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (!result || (right != 0 && left > UINT64_MAX / right)) return -1;
    *result = left * right;
    return 0;
}

/* 只允许常见 SD/eMMC/USB 读卡器块设备，防止错误挂载路径格式化网络文件系统。 */
static int storage_safe_block_device(const char *device, char *resolved,
                                     size_t resolved_sz)
{
    if (!device || strncmp(device, "/dev/", 5) != 0) return 0;
    char real_device[PATH_MAX];
    const char *candidate = device;
    if (realpath(device, real_device) != NULL) candidate = real_device;
    if (resolved && resolved_sz > 0) snprintf(resolved, resolved_sz, "%s", candidate);
    const char *name = strrchr(candidate, '/');
    name = name ? name + 1 : candidate;
    return strncmp(name, "mmc", 3) == 0 ||
           strncmp(name, "sd", 2) == 0 ||
           strncmp(name, "hd", 2) == 0;
}

static storage_format_kind_t storage_format_kind(const char *fs_type)
{
    if (!fs_type) return STORAGE_FORMAT_NONE;
    if (strcmp(fs_type, "vfat") == 0 || strcmp(fs_type, "msdos") == 0 ||
        strcmp(fs_type, "fat") == 0)
        return STORAGE_FORMAT_VFAT;
    if (strcmp(fs_type, "ext4") == 0) return STORAGE_FORMAT_EXT4;
    if (strcmp(fs_type, "exfat") == 0) return STORAGE_FORMAT_EXFAT;
    return STORAGE_FORMAT_NONE;
}

int ipcam_storage_get_info(const char *root, ipcam_storage_info_t *info)
{
    if (!info) return -1;
    memset(info, 0, sizeof(*info));
    copy_mount_field(info->mount_path, sizeof(info->mount_path), root);

    char error[128] = "";
    if (storage_is_independent_mount(root, error, sizeof(error)) != 0) {
        storage_message(info->status, sizeof(info->status), error);
        return -1;
    }

    struct statvfs vfs;
    if (statvfs(root, &vfs) != 0 || vfs.f_frsize == 0) {
        snprintf(info->status, sizeof(info->status), "无法读取存储空间: %s",
                 root ? root : "");
        return -1;
    }
    if (multiply_u64((uint64_t)vfs.f_blocks, (uint64_t)vfs.f_frsize,
                     &info->total_bytes) != 0 ||
        multiply_u64((uint64_t)vfs.f_bavail, (uint64_t)vfs.f_frsize,
                     &info->available_bytes) != 0) {
        storage_message(info->status, sizeof(info->status), "存储容量超出支持范围");
        return -1;
    }
    uint64_t free_bytes = 0;
    if (multiply_u64((uint64_t)vfs.f_bfree, (uint64_t)vfs.f_frsize,
                     &free_bytes) != 0) {
        storage_message(info->status, sizeof(info->status), "无法计算存储使用量");
        return -1;
    }
    info->used_bytes = info->total_bytes > free_bytes ?
                       info->total_bytes - free_bytes : 0;

    (void)storage_mount_entry(root, info->device, sizeof(info->device),
                              info->fs_type, sizeof(info->fs_type));
    char resolved[PATH_MAX] = "";
    info->format_supported = storage_format_kind(info->fs_type) != STORAGE_FORMAT_NONE &&
                             storage_safe_block_device(info->device, resolved,
                                                       sizeof(resolved));
    info->mounted = 1;
    storage_message(info->status, sizeof(info->status), "SD 卡已挂载");
    return 0;
}

/* 在子进程中执行固定路径工具；绝不通过 /bin/sh 解释设备路径。 */
static int run_exec(const char *path, const char *const argv[])
{
    if (!path || !argv) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(path, (char *const *)argv);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* 先使用标准工具名，再尝试 BusyBox applet，兼容精简 rootfs。 */
static int run_tool(const char *const paths[], const char *const argv[],
                    const char *busybox_applet)
{
    for (size_t i = 0; paths && paths[i]; i++) {
        if (access(paths[i], X_OK) == 0) return run_exec(paths[i], argv);
    }
    const char *busybox_paths[] = { "/bin/busybox", "/sbin/busybox",
                                    "/usr/bin/busybox", "/usr/sbin/busybox", NULL };
    for (size_t i = 0; busybox_applet && busybox_paths[i]; i++) {
        if (access(busybox_paths[i], X_OK) != 0) continue;
        const char *busybox_argv[8];
        size_t n = 0;
        busybox_argv[n++] = "busybox";
        busybox_argv[n++] = busybox_applet;
        for (size_t j = 1; argv[j] && n + 1 < sizeof(busybox_argv) / sizeof(busybox_argv[0]); j++)
            busybox_argv[n++] = argv[j];
        busybox_argv[n] = NULL;
        return run_exec(busybox_paths[i], busybox_argv);
    }
    return -1;
}

int ipcam_storage_format(const char *root, char *message, size_t message_sz)
{
    storage_message(message, message_sz, "格式化失败");
    ipcam_storage_info_t info;
    if (ipcam_storage_get_info(root, &info) != 0 || !info.mounted) {
        storage_message(message, message_sz, "SD 卡未挂载，拒绝格式化");
        return -1;
    }
    if (!info.format_supported) {
        storage_message(message, message_sz, "设备或文件系统不支持安全格式化");
        return -1;
    }

    char device[PATH_MAX] = "";
    if (!storage_safe_block_device(info.device, device, sizeof(device))) {
        storage_message(message, message_sz, "未识别到安全的 SD 卡块设备");
        return -1;
    }
    storage_format_kind_t kind = storage_format_kind(info.fs_type);
    const char *format_fs = kind == STORAGE_FORMAT_VFAT ? "vfat" :
                            (kind == STORAGE_FORMAT_EXT4 ? "ext4" : "exfat");
    const char *umount_paths[] = { "/bin/umount", "/sbin/umount",
                                   "/usr/bin/umount", "/usr/sbin/umount", NULL };
    const char *mount_paths[] = { "/bin/mount", "/sbin/mount",
                                  "/usr/bin/mount", "/usr/sbin/mount", NULL };
    const char *umount_argv[] = { "umount", root, NULL };
    const char *mount_argv[] = { "mount", "-t", format_fs, device, root, NULL };

    /* 格式化属于破坏性操作，调用者必须已经完成二次确认；这里再做设备级
     * 检查，防止 UI、HTTP 或未来其它入口绕过挂载身份校验。 */
    sync();
    if (run_tool(umount_paths, umount_argv, "umount") != 0) {
        storage_message(message, message_sz, "卸载 SD 卡失败，未执行格式化");
        return -1;
    }

    const char *format_paths_vfat[] = { "/sbin/mkfs.vfat", "/sbin/mkfs.fat",
                                        "/usr/sbin/mkfs.vfat", "/usr/sbin/mkfs.fat",
                                        "/bin/mkfs.vfat", "/usr/bin/mkfs.vfat", NULL };
    const char *format_paths_ext4[] = { "/sbin/mkfs.ext4", "/usr/sbin/mkfs.ext4",
                                        "/bin/mkfs.ext4", "/usr/bin/mkfs.ext4", NULL };
    const char *format_paths_exfat[] = { "/sbin/mkfs.exfat", "/usr/sbin/mkfs.exfat",
                                         "/bin/mkfs.exfat", "/usr/bin/mkfs.exfat", NULL };
    const char *const *format_paths = kind == STORAGE_FORMAT_VFAT ? format_paths_vfat :
                                      (kind == STORAGE_FORMAT_EXT4 ? format_paths_ext4 :
                                       format_paths_exfat);
    const char *format_argv_vfat[] = { "mkfs.vfat", device, NULL };
    const char *format_argv_ext4[] = { "mkfs.ext4", "-F", device, NULL };
    const char *format_argv_exfat[] = { "mkfs.exfat", device, NULL };
    const char *const *format_argv = kind == STORAGE_FORMAT_VFAT ? format_argv_vfat :
                                     (kind == STORAGE_FORMAT_EXT4 ? format_argv_ext4 :
                                      format_argv_exfat);

    int format_rc = run_tool(format_paths, format_argv,
                             kind == STORAGE_FORMAT_VFAT ? "mkfs.vfat" :
                             (kind == STORAGE_FORMAT_EXT4 ? "mkfs.ext4" : "mkfs.exfat"));
    /* 无论 mkfs 成败都尝试重新挂载，避免一次失败把用户留在“卡消失”的状态。 */
    int mount_rc = run_tool(mount_paths, mount_argv, "mount");
    if (format_rc != 0) {
        storage_message(message, message_sz, mount_rc == 0 ?
                         "格式化失败，SD 卡已重新挂载" :
                         "格式化失败且 SD 卡重新挂载失败");
        return -1;
    }
    if (mount_rc != 0) {
        storage_message(message, message_sz, "格式化完成，但 SD 卡重新挂载失败");
        return -1;
    }
    MLOGI("storage formatted: root=%s device=%s fs=%s\n", root, device, format_fs);
    storage_message(message, message_sz, "SD 卡格式化完成");
    return 0;
}
