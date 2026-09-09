# ipcam 顶层 Makefile
#
#  结构（src/core/ + src/services/ + src/main.c）：
#    - src/core/       通用基础（log / ringbuffer / param / sys / ota）
#    - src/services/   业务子模块（capture/display/encode/stream/net4g/netwifi/cli）
#    - src/main.c      应用入口
#
#  用法：
#    make                       # 默认 release 构建（依赖 libjpeg-turbo）
#    make ipcam-display-only    # 不链 libturbojpeg（用 ipcam_encode_stub.c 空壳）
#    make DEBUG=1               # 调试构建
#    make CROSS_COMPILE=arm-linux-gnueabihf-
#    make CROSS_COMPILE=         # 用本机 gcc（仅用于主机端开发自测）
#    make clean / install / uninstall / test-host

ROOT      := $(shell pwd)
PREFIX   ?= $(ROOT)/output
LVGL_DIR  ?= $(ROOT)/thirdparts/lvgl/src
BUILD_DIR ?= $(ROOT)/build
LVGL_BUILD_DIR := $(BUILD_DIR)/lvgl

CROSS_COMPILE ?= arm-linux-gnueabihf-
CC            = $(CROSS_COMPILE)gcc
AR            = $(CROSS_COMPILE)ar
STRIP         = $(CROSS_COMPILE)strip

DEBUG ?= 0
ifeq ($(DEBUG),1)
OPT := -O0 -g3
else
OPT := -O2 -g
endif

WARN := -Wall -Wextra -Wno-unused-parameter -Wno-unused-result

# libjpeg-turbo
TJ_PREFIX    ?= $(ROOT)/thirdparts/libjpeg-turbo/install
TJ_CFLAGS    := -I$(TJ_PREFIX)/include
TJ_LDFLAGS   := -L$(TJ_PREFIX)/lib -lturbojpeg

# 头文件搜索路径
INCLUDES := -I$(ROOT)/config \
            -I$(ROOT) \
            -I$(ROOT)/src \
            -I$(ROOT)/src/core \
            -I$(ROOT)/src/services \
            -I$(LVGL_DIR)

# 32 位 ARM 仍需支持接近 3 GiB 的 AVI 段，启用 glibc 大文件接口。
CFLAGS  := $(OPT) $(WARN) -std=gnu99 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
           -DLV_CONF_INCLUDE_SIMPLE -pthread $(TJ_CFLAGS) $(INCLUDES)
# 触摸双指距离计算使用 libm；display-only 也要保持同一手势实现可链接。
LDFLAGS := -pthread $(TJ_LDFLAGS) -lm

# 源文件：默认从 src/services/*.c 通配，但排除 encode 主源 + stub
# （encode 由 ENCODE_SRC 按 target 选定其中一个）
# 录像、控制、熄屏和触摸服务随 wildcard 自动纳入，只有编码真源/空壳需要互斥选择。
SRCS_SERVICES_WILDCARD := $(wildcard src/services/*.c)
SRCS_SERVICES := $(filter-out src/services/ipcam_encode.c src/services/ipcam_encode_stub.c, $(SRCS_SERVICES_WILDCARD))

# display-only 用 stub；其他 target 用真 encode
# findstring：支持 `make clean ipcam-display-only` 等多目标
ENCODE_SRC := src/services/ipcam_encode.c
ifneq (,$(findstring ipcam-display-only,$(MAKECMDGOALS)))
    ENCODE_SRC := src/services/ipcam_encode_stub.c
endif

SRCS_CORE     := $(wildcard src/core/*.c)
SRCS_APP      := src/main.c
SRCS          := $(SRCS_CORE) $(SRCS_SERVICES) $(ENCODE_SRC) $(SRCS_APP)

OBJS := $(SRCS:.c=.o)
BIN  := ipcam

# LVGL 的 CMake 也按 src 下的 C 文件递归编译；这里使用同一边界，关闭的
# 平台后端会由 lv_conf.h 的宏裁剪，生成的对象放到 build/，不污染外部源码树。
LVGL_SRCS := $(shell if [ -f "$(LVGL_DIR)/src/lv_init.c" ]; then \
                    find "$(LVGL_DIR)/src" -type f -name '*.c' -print; \
                fi)
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(LVGL_BUILD_DIR)/%.o,$(LVGL_SRCS))
LVGL_STATIC_LIB := $(BUILD_DIR)/liblvgl.a

# argv[0] 多调用入口的软软链接
LINKS := camver camctl

.PHONY: all clean install uninstall ipcam-display-only test-host

all: $(BIN)

$(BIN): $(OBJS) $(LVGL_STATIC_LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "=== build OK: $@ ==="

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(LVGL_BUILD_DIR)/%.o: $(LVGL_DIR)/%.c
	@mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -I$(LVGL_DIR)/src -c $< -o $@

ifneq ($(strip $(LVGL_SRCS)),)
$(LVGL_STATIC_LIB): $(LVGL_OBJS)
	@mkdir -p "$(@D)"
	$(AR) rcs $@ $^
else
$(LVGL_STATIC_LIB):
	@echo "缺少 LVGL v9.5.0 源码：$(LVGL_DIR)" >&2
	@echo "请执行：git clone --branch v9.5.0 --depth 1 https://github.com/lvgl/lvgl.git $(LVGL_DIR)" >&2
	@false
endif

# ipcam-display-only：保留 stub 编码器，不需要 libjpeg-turbo
ipcam-display-only: CFLAGS := $(OPT) $(WARN) -std=gnu99 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
                              -DLV_CONF_INCLUDE_SIMPLE -pthread $(INCLUDES)
ipcam-display-only: LDFLAGS := -pthread -lm
ipcam-display-only: $(BIN)

# 主机测试不依赖 ARM 工具链或板端设备；产物放到 /tmp，避免污染工作树。
HOST_CC ?= gcc
TEST_BUILD_DIR ?= /tmp/ipcam-test-build
TEST_SANITIZE_FLAGS :=
ifeq ($(SANITIZE),1)
TEST_SANITIZE_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
endif
TEST_CFLAGS := -std=gnu99 -Wall -Wextra -Werror -Werror=implicit-function-declaration \
               -pthread -I$(ROOT)/config -I$(ROOT)/src -I$(ROOT)/src/core \
               -I$(ROOT)/src/services $(TEST_SANITIZE_FLAGS)
TEST_LDFLAGS := -pthread $(TEST_SANITIZE_FLAGS)

TEST_SCALE_BIN := $(TEST_BUILD_DIR)/test_ipcam_scale
TEST_FRAME_BIN := $(TEST_BUILD_DIR)/test_ipcam_frame_diag
TEST_STREAM_BIN := $(TEST_BUILD_DIR)/test_ipcam_stream_lifecycle

test-host: $(TEST_SCALE_BIN) $(TEST_FRAME_BIN) $(TEST_STREAM_BIN)
	@$(TEST_SCALE_BIN)
	@$(TEST_FRAME_BIN)
	@$(TEST_STREAM_BIN)

$(TEST_BUILD_DIR):
	mkdir -p $@

$(TEST_SCALE_BIN): tests/test_ipcam_scale.c src/core/ipcam_scale.c | $(TEST_BUILD_DIR)
	$(HOST_CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

$(TEST_FRAME_BIN): tests/test_ipcam_frame_diag.c src/core/ipcam_frame_diag.c \
                   src/core/ipcam_ringbuffer.c | $(TEST_BUILD_DIR)
	$(HOST_CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

$(TEST_STREAM_BIN): tests/test_ipcam_stream_lifecycle.c tests/test_ipcam_stream_stubs.c \
                    src/services/ipcam_stream.c src/core/ipcam_ringbuffer.c | $(TEST_BUILD_DIR)
	$(HOST_CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

install: $(BIN)
	install -d $(PREFIX)/usr/bin
	install -d $(PREFIX)/etc/init.d
	install -d $(PREFIX)/etc/ppp/peers
	install $(BIN) $(PREFIX)/usr/bin/$(BIN)
	@for link in $(LINKS); do ln -sf $(BIN) $(PREFIX)/usr/bin/$$link; done
	install -m 755 $(ROOT)/scripts/rootfs/etc/init.d/S90ipcam $(PREFIX)/etc/init.d/
	install -m 644 $(ROOT)/scripts/rootfs/etc/ppp/peers/quectel $(PREFIX)/etc/ppp/peers/

uninstall:
	rm -rf $(PREFIX)

clean:
	rm -f $(BIN) $(OBJS)
	rm -rf $(LVGL_BUILD_DIR) $(LVGL_STATIC_LIB)
