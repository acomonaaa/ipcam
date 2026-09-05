#ifndef IPCAM_CLI_H
#define IPCAM_CLI_H

/*
 * argv[0] 多调用入口（参考 BCF2 mc-app-moobox/src/main.c:39-132 的 dispatch）。
 *
 * 通过软链接 / BusyBox applet 把同一二进制当作多个命令启动：
 *   ipcam       → 默认（运行守护进程）
 *   camver      → 打印版本
 *   camctl      → 控制子命令（status / reboot）
 *
 * ipcam_cli_dispatch 返回 IPCAM_CLI_RUN_DAEMON 表示需要 main 进入 daemon 流程，
 * 其他返回值（>=0）是进程退出码。
 */
typedef enum {
    IPCAM_CLI_RUN_DAEMON = -1,   /* main 应当调用 run_daemon() */
    IPCAM_CLI_EXIT_OK    = 0,
    IPCAM_CLI_EXIT_ERR   = 1,
} ipcam_cli_action_t;

ipcam_cli_action_t ipcam_cli_dispatch(int argc, char **argv);

#endif /* IPCAM_CLI_H */