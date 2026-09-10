#!/usr/bin/env bash

#
# 为 IPCam 当前 LVGL 页面生成静态 CJK 子集字库。
#
# 该脚本只在 Ubuntu 主机上运行，生成的 .c 文件会被 Makefile 的 src/ui/*.c
# 通配规则自动编译；开发板运行时不需要 npx、字体文件或任何动态加载器。
#

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FONT_SOURCE="${IPCAM_UI_FONT_SOURCE:-$ROOT/thirdparts/lvgl/src/scripts/built_in_font/SourceHanSansSC-Normal.otf}"
FONT_CONV_PACKAGE="${IPCAM_LV_FONT_CONV_PACKAGE:-lv_font_conv@1.5.3}"

if ! command -v npx >/dev/null 2>&1; then
    echo "缺少 npx：请在 Ubuntu 安装 Node.js 后再生成 LVGL 字库" >&2
    exit 1
fi
if ! command -v rg >/dev/null 2>&1 || ! command -v perl >/dev/null 2>&1; then
    echo "缺少 rg 或 perl：无法从 UI 文案提取字形集合" >&2
    exit 1
fi
if [[ ! -f "$FONT_SOURCE" ]]; then
    echo "找不到 CJK 字体源文件：$FONT_SOURCE" >&2
    echo "可通过 IPCAM_UI_FONT_SOURCE=/path/to/font.otf 覆盖字体源" >&2
    exit 1
fi

#
# 只从 UI 与控制/存储/录像服务的 C 字符串字面量提取非 ASCII 字符；注释中的中文
# 不应进入固件字库。左下角反馈文本由服务层返回，因此服务源文件必须一起扫描，
# 否则页面静态文案正常而运行时错误提示会出现缺字。动态状态值使用 ASCII 占位符，
# 这里再统一补入 ASCII 可打印字符范围。
#
UI_SOURCES=(
    "$ROOT/src/ui/ipcam_ui.c"
    "$ROOT/src/ui/ipcam_ui_components.c"
    "$ROOT/src/ui/ipcam_ui_sleep.c"
    "$ROOT"/src/ui/ipcam_ui_screen_*.c
    "$ROOT/src/services/ipcam_lvgl.c"
    "$ROOT/src/services/ipcam_control.c"
    "$ROOT/src/services/ipcam_storage.c"
    "$ROOT/src/services/ipcam_record.c"
)
symbols="$(
    rg -o --no-filename -P '"[^"]*"' "${UI_SOURCES[@]}" |
    perl -CSD -Mutf8 -ne '
        while (/[^\x00-\x7f]/g) {
            $symbols{$&} = 1;
        }
        END {
            print join("", sort keys %symbols);
        }
    '
)"
if [[ -z "$symbols" ]]; then
    echo "未从 UI 文案提取到非 ASCII 字符，拒绝生成空的 CJK 字库" >&2
    exit 1
fi

#
# 14/16/20 px 分别对应辅助文字、正文和标题；4 bpp 在 RGB565 LCD 上保留平滑边缘，
# --no-kerning 删除当前固定坐标界面不需要的字偶距表，--no-compress 则避免依赖
# LVGL 的运行时压缩字库解码器。原来生成的 bitmap_format=1 在本项目关闭
# LV_USE_FONT_COMPRESSED 时会直接返回空 bitmap，表现为中文、数字和英文全部消失。
# --no-prefilter 与 --no-compress 配套，保证生成的字库使用普通 bitmap 格式。
#
for size in 14 16 20; do
    output="$ROOT/src/ui/ipcam_ui_font_cjk_${size}.c"
    npx --yes "$FONT_CONV_PACKAGE" \
        --size "$size" \
        --bpp 4 \
        --format lvgl \
        --font "$FONT_SOURCE" \
        -r 0x20-0x7f \
        --symbols "$symbols" \
        --no-kerning \
        --no-compress \
        --no-prefilter \
        --output "$output" \
        --lv-include lvgl.h \
        --lv-font-name "ipcam_font_cjk_${size}"
    echo "已生成：$output"
done
