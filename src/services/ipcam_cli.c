#define _GNU_SOURCE
#include "ipcam_cli.h"
#include "ipcam_log.h"
#include "ipcam_ota.h"

#include <libgen.h>
#include <limits.h>     /* PATH_MAX */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ipcam_config.h"

static int do_ver(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("ipcam version: %s\n", IPCAM_VERSION);
    printf("model        : %s\n", IPCAM_MODEL);
    printf("capture      : %dx%d\n", IPCAM_CAPTURE_WIDTH, IPCAM_CAPTURE_HEIGHT);
    printf("jpeg q       : %d\n", IPCAM_JPEG_QUALITY);
    printf("http port    : %d\n", IPCAM_HTTP_PORT);
    printf("net mode     : %s\n", IPCAM_NET_MODE);
    return 0;
}

static int do_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("ipcam STATUS (static info):\n");
    printf("  model        : %s\n", IPCAM_MODEL);
    printf("  swver        : %s\n", IPCAM_VERSION);
    printf("  capture      : %dx%d\n", IPCAM_CAPTURE_WIDTH, IPCAM_CAPTURE_HEIGHT);
    printf("  net_mode     : %s\n", IPCAM_NET_MODE);
    printf("  http_port    : %d\n", IPCAM_HTTP_PORT);
    printf("  jpeg_q       : %d\n", IPCAM_JPEG_QUALITY);
    return 0;
}

static int do_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (geteuid() != 0) {
        fprintf(stderr, "camctl reboot: permission denied (need root)\n");
        return 1;
    }
    sync();
    return reboot(RB_AUTOBOOT);
}

/*
 * camctl ota <local-path> [sha256]
 * camctl ota url <url> [sha256]
 * camctl ota commit    — 将 .new 切换到正式位并 reboot
 * camctl ota rollback  — 回滚到 .prev
 * camctl ota status    — 打印当前 OTA 状态
 */
static int do_ota(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage:\n"
            "  camctl ota <path> [sha256]   # local file\n"
            "  camctl ota url <url> [sha256]\n"
            "  camctl ota commit           # activate staged .new\n"
            "  camctl ota rollback         # restore .prev\n"
            "  camctl ota status\n");
        return 1;
    }

    ipcam_ota_result_t res;
    memset(&res, 0, sizeof(res));

    if (geteuid() != 0) {
        fprintf(stderr, "camctl ota: permission denied (need root)\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        ipcam_ota_get_status(&res);
        printf("ota state  : %d\n", (int)res.state);
        printf("ota message: %s\n", res.message);
        printf("current ver: %s\n", ipcam_ota_get_current_version());
        return 0;
    }
    if (strcmp(argv[1], "commit") == 0) {
        if (ipcam_ota_commit(IPCAM_OTA_PATH_DEF) < 0) {
            ipcam_ota_get_status(&res);
            fprintf(stderr, "ota commit failed: %s\n", res.message);
            return 1;
        }
        printf("ota committed; rebooting in 2s...\n");
        sync(); sleep(2);
        reboot(RB_AUTOBOOT);
        return 0;
    }
    if (strcmp(argv[1], "rollback") == 0) {
        if (ipcam_ota_rollback(IPCAM_OTA_PATH_DEF) < 0) {
            ipcam_ota_get_status(&res);
            fprintf(stderr, "rollback failed: %s\n", res.message);
            return 1;
        }
        printf("rolled back to .prev; rebooting in 2s...\n");
        sync(); sleep(2);
        reboot(RB_AUTOBOOT);
        return 0;
    }

    const char *src = argv[1];
    const char *sha = (argc >= 3) ? argv[2] : NULL;
    int rc;

    if (strcmp(src, "url") == 0) {
        if (argc < 3) {
            fprintf(stderr, "camctl ota url <url> [sha256]\n");
            return 1;
        }
        const char *url = argv[2];
        sha = (argc >= 4) ? argv[3] : NULL;
        rc = ipcam_ota_from_url(url, sha, &res);
    } else {
        /* local path */
        rc = ipcam_ota_from_file(src, sha, &res);
    }

    if (rc < 0) {
        ipcam_ota_get_status(&res);
        fprintf(stderr, "ota failed: %s\n", res.message);
        return 1;
    }
    printf("ota staged (.new written). Run `camctl ota commit` to activate & reboot.\n");
    return 0;
}

ipcam_cli_action_t ipcam_cli_dispatch(int argc, char **argv)
{
    if (argc <= 0) return IPCAM_CLI_EXIT_ERR;

    /* 取 argv[0] 的 basename（用 PATH_MAX 长度的副本避免截断） */
    char buf[PATH_MAX];
    strncpy(buf, argv[0], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *name = basename(buf);

    if (strcmp(name, "camver") == 0) {
        return do_ver(argc, argv) ? IPCAM_CLI_EXIT_ERR : IPCAM_CLI_EXIT_OK;
    }
    if (strcmp(name, "camctl") == 0) {
        if (argc < 2) {
            fprintf(stderr, "usage: camctl <status|reboot>\n");
            return IPCAM_CLI_EXIT_ERR;
        }
        if (strcmp(argv[1], "status") == 0) {
            do_status(argc - 1, argv + 1);
            return IPCAM_CLI_EXIT_OK;
        }
        if (strcmp(argv[1], "reboot") == 0) {
            return do_reboot(argc - 1, argv + 1) ? IPCAM_CLI_EXIT_ERR : IPCAM_CLI_EXIT_OK;
        }
        if (strcmp(argv[1], "ota") == 0) {
            return do_ota(argc - 1, argv + 1) ? IPCAM_CLI_EXIT_ERR : IPCAM_CLI_EXIT_OK;
        }
        fprintf(stderr, "unknown camctl subcommand: %s\n", argv[1]);
        return IPCAM_CLI_EXIT_ERR;
    }
    /* 默认 / ipcam ：进入守护进程 */
    return IPCAM_CLI_RUN_DAEMON;
}