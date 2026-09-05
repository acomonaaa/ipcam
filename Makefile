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
#    make clean / install / uninstall

ROOT      := $(shell pwd)
PREFIX   ?= $(ROOT)/output

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
            -I$(ROOT)/src \
            -I$(ROOT)/src/core \
            -I$(ROOT)/src/services

CFLAGS  := $(OPT) $(WARN) -std=gnu99 -D_GNU_SOURCE -pthread $(TJ_CFLAGS) $(INCLUDES)
LDFLAGS := -pthread $(TJ_LDFLAGS)

# 源文件：默认从 src/services/*.c 通配，但排除 encode 主源 + stub
# （encode 由 ENCODE_SRC 按 target 选定其中一个）
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

# argv[0] 多调用入口的软软链接
LINKS := camver camctl

.PHONY: all clean install uninstall ipcam-display-only

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "=== build OK: $@ ==="

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ipcam-display-only：保留 stub 编码器，不需要 libjpeg-turbo
ipcam-display-only: CFLAGS := $(OPT) $(WARN) -std=gnu99 -D_GNU_SOURCE -pthread $(INCLUDES)
ipcam-display-only: LDFLAGS := -pthread
ipcam-display-only: $(BIN)

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