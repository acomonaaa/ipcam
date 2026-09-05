#ifndef IPCAM_OTA_H
#define IPCAM_OTA_H

/*
 * BCF2 mo_ota 风格的轻量级 OTA。
 *
 * 设计要点：
 *   - 不需要自己的服务器/云；接受任意 HTTP 可达的 URL 或本地文件
 *   - SHA256 + ELF magic 校验，防止 brick
 *   - 原子切换：新文件先 rename 到 .new，原文件改 .prev，新文件 rename 到正式位
 *   - init.d 启动检测：若新版本启动失败（无 /healthz），自动回滚到 .prev
 *   - 状态通过 param 持久化（pending_version / pending_sha256 / pending_path）
 *
 * 安全约束：
 *   - 单进程内同一时刻只允许一个 OTA 任务（atomic 标志）
 *   - 校验失败/下载失败/写盘失败 → 状态记录为 OTA_STATE_FAILED，文件清理
 *   - 升级成功后旧版保留为 .prev（最多 1 份）
 */

#include <stddef.h>
#include <stdint.h>

#define IPCAM_OTA_PATH_DEF    "/usr/bin/ipcam"
#define IPCAM_OTA_PATH_PREV   "/usr/bin/ipcam.prev"
#define IPCAM_OTA_PATH_NEW     "/usr/bin/ipcam.new"
#define IPCAM_OTA_HEALTH_URL  "http://127.0.0.1:8080/healthz"
#define IPCAM_OTA_HEALTH_WAIT_S 5

/* 状态机（持久化在 param） */
typedef enum {
    IPCAM_OTA_STATE_IDLE      = 0,
    IPCAM_OTA_STATE_DOWNLOADING,
    IPCAM_OTA_STATE_VERIFYING,
    IPCAM_OTA_STATE_STAGED,     /* 已写到 .new，待重启 */
    IPCAM_OTA_STATE_INSTALLED,  /* 已切换，重启中 */
    IPCAM_OTA_STATE_FAILED,
} ipcam_ota_state_t;

/* 结果（用于 camctl ota /api/ota 返回） */
typedef struct ipcam_ota_result_s {
    ipcam_ota_state_t state;
    char     message[128];
    char     current_version[32];
    char     pending_version[32];
    uint32_t progress;     /* 0-100，下载/校验时 */
} ipcam_ota_result_t;

/* 初始化（检查 pending 升级、可能的回滚标记） */
int ipcam_ota_init(const char *bin_path);

/* 当前二进制版本（读自 ELF 的 .note 节或 param） */
const char *ipcam_ota_get_current_version(void);

/* 同步 OTA：从本地文件升级（指定 sha256=NULL 跳过校验；否则必须匹配） */
int ipcam_ota_from_file(const char *src_path,
                        const char *expected_sha256_hex,
                        ipcam_ota_result_t *out);

/* 同步 OTA：从 URL 下载并升级（http:// or https:// 或 file://） */
int ipcam_ota_from_url(const char *url,
                       const char *expected_sha256_hex,
                       ipcam_ota_result_t *out);

/* 查询状态（用于 /api/ota GET） */
void ipcam_ota_get_status(ipcam_ota_result_t *out);

/* 切换 staged → installed：rename .new -> 正式 path，原 -> .prev */
int ipcam_ota_commit(const char *bin_path);

/* 回滚到 .prev */
int ipcam_ota_rollback(const char *bin_path);

/* 计算 SHA256（hex 输出 65 字节：64 hex + NUL） */
int ipcam_sha256_hex_file(const char *path, char *out_hex65);

#endif /* IPCAM_OTA_H */