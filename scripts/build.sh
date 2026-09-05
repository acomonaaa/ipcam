#!/bin/sh
# scripts/build.sh - 交叉编译 ipcam
#
# 默认使用 ALIENTEK 提供的 gcc-linaro-4.9.4-2017.01。
# 通过环境变量覆盖：
#   CROSS_COMPILE=arm-linux-gnueabihf-  ./scripts/build.sh
#   DEBUG=1                              ./scripts/build.sh  (启用 -O0 -g3)
#
# 先决条件：
#   1) libjpeg-turbo v2.1.x 已交叉编译并安装到 ../thirdparts/libjpeg-turbo/install/
#   2) 交叉编译器已加入 PATH

set -e

# 切到项目根（scripts/build.sh → ../ 是 repo root）
cd "$(dirname "$0")/.."

# 自动探测默认交叉编译器（如果存在）
if [ -z "$CROSS_COMPILE" ] && [ -x /opt/gcc-linaro-4.9.4-2017.01/bin/arm-linux-gnueabihf-gcc ]; then
    export PATH="/opt/gcc-linaro-4.9.4-2017.01/bin:$PATH"
    export CROSS_COMPILE=arm-linux-gnueabihf-
fi

if [ -z "$CROSS_COMPILE" ]; then
    echo "[build.sh] WARN: CROSS_COMPILE 未设置，使用本机 gcc（仅供主机端自测）"
fi

echo "[build.sh] CROSS_COMPILE=$CROSS_COMPILE"
echo "[build.sh] DEBUG=$DEBUG"

# 若 libturbojpeg.a 没装好，构建 display-only 变体（不链 encode）
if [ ! -f "thirdparts/libjpeg-turbo/install/lib/libturbojpeg.a" ] && [ -z "$SKIP_TJ_CHECK" ]; then
    echo "[build.sh] NOTE: libturbojpeg.a 未找到，构建 ipcam-display-only（无 MJPEG 编码）"
    echo "[build.sh]       编译 libjpeg-turbo："
    echo "[build.sh]         cd thirdparts/libjpeg-turbo"
    echo "[build.sh]         ./fetch-and-build.sh"
    echo "[build.sh]       然后重跑本脚本"
    make DEBUG="${DEBUG:-0}" ipcam-display-only "$@"
    exit 0
fi

make DEBUG="${DEBUG:-0}" "$@"

echo "[build.sh] done. 产物: ./ipcam"
file ipcam 2>/dev/null || true