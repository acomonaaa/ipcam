#define _GNU_SOURCE

#include "ipcam_log.h"
#include "ipcam_storage.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* 主机测试只验证路径保护，不接触真实块设备或执行 mkfs。 */
void ipcam_log_printf(ipcam_log_level_t level, const char *module,
                      const char *file, uint32_t line, const char *fmt, ...)
{
    (void)level;
    (void)module;
    (void)file;
    (void)line;
    (void)fmt;
}

static void test_root_and_unmounted_are_rejected(void)
{
    ipcam_storage_info_t info;
    memset(&info, 0, sizeof(info));
    assert(ipcam_storage_get_info("/", &info) != 0);
    assert(info.mounted == 0);
    assert(ipcam_storage_format("/", (char[128]){0}, 128) != 0);

    char path[] = "/tmp/ipcam-storage-test-XXXXXX";
    assert(mkdtemp(path) != NULL);
    memset(&info, 0, sizeof(info));
    assert(ipcam_storage_get_info(path, &info) != 0);
    assert(info.mounted == 0);
    assert(info.format_supported == 0);
    assert(rmdir(path) == 0);
}

int main(void)
{
    test_root_and_unmounted_are_rejected();
    puts("ipcam storage safety tests: PASS");
    return 0;
}
