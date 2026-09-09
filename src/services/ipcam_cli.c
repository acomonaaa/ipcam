#define _GNU_SOURCE
/* camctl 走 HTTP 控制面；命令发起与结果日志归入 CLI 模块。 */
#define IPCAM_LOG_MODULE "CLI "

#include "ipcam_cli.h"
#include "ipcam_log.h"
#include "ipcam_ota.h"

#include <libgen.h>
#include <limits.h>     /* PATH_MAX */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ipcam_config.h"

/*
 * camctl 通过本机 HTTP 回环访问运行中的 daemon，保证 status/录像/拍照
 * 与 GUI 走同一套控制契约。只处理短 JSON 响应，设置超时避免命令行永久阻塞。
 */
static int local_http_request(const char *method, const char *path, const char *body)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    const char *port_env = getenv("IPCAM_HTTP_PORT");
    int port = port_env && *port_env ? atoi(port_env) : IPCAM_HTTP_PORT;
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    addr.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    size_t body_len = body ? strlen(body) : 0;
    char request[1024];
    int n = snprintf(request, sizeof(request),
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "Connection: close\r\nContent-Length: %zu\r\n\r\n%s",
                     method, path, body_len, body ? body : "");
    if (n <= 0 || n >= (int)sizeof(request)) { close(fd); return -1; }
    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t wr = write(fd, request + sent, (size_t)n - sent);
        if (wr < 0 && errno == EINTR) continue;
        if (wr <= 0) { close(fd); return -1; }
        sent += (size_t)wr;
    }

    char response[16384];
    size_t got = 0;
    while (got < sizeof(response) - 1) {
        ssize_t rd = read(fd, response + got, sizeof(response) - 1 - got);
        if (rd < 0 && errno == EINTR) continue;
        if (rd <= 0) break;
        got += (size_t)rd;
    }
    close(fd);
    response[got] = '\0';
    char *body_start = strstr(response, "\r\n\r\n");
    if (!body_start) return -1;
    body_start += 4;
    const char *status = response + 9; /* HTTP/1.x 后的三位状态码 */
    int status_code = atoi(status);
    fputs(body_start, stdout);
    return status_code >= 200 && status_code < 300 ? 0 : -1;
}

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
    /* camctl 与 GUI/HTTP 复用同一状态接口，避免 CLI 显示编译期假值。 */
    if (local_http_request("GET", "/api/status", NULL) == 0) return 0;
    fprintf(stderr, "camctl status: daemon unavailable, showing compile-time defaults\n");
    printf("ipcam STATUS (static info):\n");
    printf("  model        : %s\n", IPCAM_MODEL);
    printf("  swver        : %s\n", IPCAM_VERSION);
    printf("  capture      : %dx%d\n", IPCAM_CAPTURE_WIDTH, IPCAM_CAPTURE_HEIGHT);
    printf("  net_mode     : %s\n", IPCAM_NET_MODE);
    printf("  http_port    : %d\n", IPCAM_HTTP_PORT);
    printf("  jpeg_q       : %d\n", IPCAM_JPEG_QUALITY);
    return 0;
}

static int do_capabilities(int argc, char **argv)
{
    (void)argc; (void)argv;
    return local_http_request("GET", "/api/capabilities", NULL) == 0 ? 0 : 1;
}

static int do_record(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "start") != 0 && strcmp(argv[1], "stop") != 0)) {
        fprintf(stderr, "usage: camctl record <start|stop>\n");
        return 1;
    }
    char body[32];
    snprintf(body, sizeof(body), "action=%s", argv[1]);
    return local_http_request("POST", "/api/record", body) == 0 ? 0 : 1;
}

static int do_photo(int argc, char **argv)
{
    (void)argc; (void)argv;
    return local_http_request("POST", "/api/photo", "") == 0 ? 0 : 1;
}

static int do_control(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: camctl control <preview|view|mirror|light> ...\n");
        return 1;
    }
    char body[256];
    if (strcmp(argv[1], "preview") == 0 && argc >= 3) {
        snprintf(body, sizeof(body), "command=preview&enabled=%d", atoi(argv[2]) ? 1 : 0);
    } else if (strcmp(argv[1], "view") == 0 && argc >= 6) {
        snprintf(body, sizeof(body), "command=view&enabled=%d&zoom=%s&center_x=%s&center_y=%s",
                 atoi(argv[2]) ? 1 : 0, argv[3], argv[4], argv[5]);
    } else if (strcmp(argv[1], "mirror") == 0 && argc >= 4) {
        snprintf(body, sizeof(body), "command=mirror&mirror_horizontal=%d&mirror_vertical=%d",
                 atoi(argv[2]) ? 1 : 0, atoi(argv[3]) ? 1 : 0);
    } else if (strcmp(argv[1], "light") == 0 && argc >= 3) {
        /* 补光是临时板级状态，范围校验由统一控制队列再次执行。 */
        snprintf(body, sizeof(body), "command=light&percent=%d", atoi(argv[2]));
    } else {
        fprintf(stderr, "usage: camctl control preview <0|1>\n"
                        "       camctl control view <0|1> <zoom> <center_x> <center_y>\n"
                        "       camctl control mirror <0|1> <0|1>\n"
                        "       camctl control light <0..100>\n");
        return 1;
    }
    return local_http_request("POST", "/api/control", body) == 0 ? 0 : 1;
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
            fprintf(stderr, "usage: camctl <status|capabilities|record|photo|control|reboot|ota>\n");
            return IPCAM_CLI_EXIT_ERR;
        }
        if (strcmp(argv[1], "status") == 0) {
            do_status(argc - 1, argv + 1);
            return IPCAM_CLI_EXIT_OK;
        }
        if (strcmp(argv[1], "reboot") == 0) {
            return do_reboot(argc - 1, argv + 1) ? IPCAM_CLI_EXIT_ERR : IPCAM_CLI_EXIT_OK;
        }
        if (strcmp(argv[1], "capabilities") == 0) {
            return do_capabilities(argc - 1, argv + 1) ? IPCAM_CLI_EXIT_ERR : IPCAM_CLI_EXIT_OK;
        }
        if (strcmp(argv[1], "record") == 0) {
            return do_record(argc - 1, argv + 1) ? IPCAM_CLI_EXIT_ERR : IPCAM_CLI_EXIT_OK;
        }
        if (strcmp(argv[1], "photo") == 0) {
            return do_photo(argc - 1, argv + 1) ? IPCAM_CLI_EXIT_ERR : IPCAM_CLI_EXIT_OK;
        }
        if (strcmp(argv[1], "control") == 0) {
            return do_control(argc - 1, argv + 1) ? IPCAM_CLI_EXIT_ERR : IPCAM_CLI_EXIT_OK;
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
