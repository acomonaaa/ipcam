# 移植 LVGL 9 到 Linux framebuffer 与 evdev

## 元信息

| 项 | 内容 |
| --- | --- |
| 修改时间 | 2026-09-09 17:22:41（UTC+8） |
| 作者 / Agent | Codex |
| 关联提交 | 待提交 |
| 影响模块 | build / display / input / main / config |

## 问题现象

在 i.MX6ULL 开发板上，基础设备已经确认存在：

1. `/dev/fb0` 存在，`virtual_size` 为 `800,480`，`bits_per_pixel` 为 `16`；
2. `/dev/dri/card0` 存在，但当前移植已有稳定的 framebuffer 路径；
3. `/dev/input/event0` 是 `20cc000.snvs:snvs-powerkey`，`event1` 是 `goodix-ts`，
   `event2` 是 `gpio_keys@0`；
4. rootfs 没有 `evtest`，无法直接用该工具验证触摸事件。

原有程序的 LCD 显示由 `ipcam_display` 线程直接把 YUYV 转换成 RGB565
并写入 framebuffer，尚未有 LVGL 9 的显示、输入和线程生命周期。仓库中
已有部分设计稿页面代码，但页面集合尚未完整，且仍按 1024×600 固定坐标，
不能直接作为当前 800×480 板级冒烟验证入口。

## 问题分析

1. 当前板级 framebuffer 已满足 LVGL 的 Linux 软件渲染最低条件，先复用
   `/dev/fb0` 比引入 DRM/KMS 设备管理更符合已验证的运行路径。
2. LVGL 的 `flush_cb` 需要拥有 framebuffer 的唯一写入权；如果保留原
   `ipcam_display` 直接写屏，会出现视频线程和 UI 线程互相覆盖，表现为控件
   消失、撕裂或黑屏。
3. 触摸设备不能让 LVGL 和业务线程各自 `read()` 同一个 evdev，否则事件会
   被两个读取者分走。已有 `ipcam_touch` 解析多点协议，因此应由它读取
   `event1`，再把坐标快照提供给 LVGL。
4. Goodix 的 ABS 坐标范围可能不是 LCD 像素范围；启动时用 `EVIOCGABS`
   读取范围并映射到 `800×480`，比硬编码 event 编号或坐标上限更适合板级
   变体。

相关代码 / 配置：

- [`ipcam_display.c`](../src/services/ipcam_display.c)
- [`ipcam_touch.c`](../src/services/ipcam_touch.c)
- [`main.c`](../src/main.c)
- [`ipcam_config.h`](../config/ipcam_config.h)
- [`lv_conf.h`](../lv_conf.h)
- [`Makefile`](../Makefile)

## 问题根因

工程只有直接 framebuffer 显示线程，没有 LVGL v9 的外部源码、配置、
`lv_display_t`/`lv_indev_t` 初始化和应用级生命周期接入；同时触摸默认配置
没有根据实机枚举结果绑定 Goodix `event1`。因此即使设备节点可用，也无法
形成“摄像头预览帧 → LVGL 合成 → framebuffer flush”和“Goodix 快照 →
LVGL pointer”的完整链路。

## 修改方案

### 变更文件列表

- [`Makefile`](../Makefile) — 固定编译外部 LVGL C 源码并链接 `liblvgl.a`，
  同时支持缺少源码时输出明确的 v9.5.0 获取命令。
- [`lv_conf.h`](../lv_conf.h) — 配置 RGB565、软件绘制、glibc、ASCII 默认字体，
  关闭与自定义适配重复的 fbdev/evdev/DRM 后端。
- [`src/services/ipcam_lvgl.h`](../src/services/ipcam_lvgl.h)、
  [`src/services/ipcam_lvgl.c`](../src/services/ipcam_lvgl.c) — 增加 LVGL
  线程、单调时钟、局部 RGB565 flush、预览图像副本、自定义 pointer 输入和
  `TOUCH TEST` 冒烟页面。
- [`src/services/ipcam_display.h`](../src/services/ipcam_display.h)、
  [`src/services/ipcam_display.c`](../src/services/ipcam_display.c) — 增加
  `ipcam_display_start_ex` 和 framebuffer writer 所有权开关；LVGL 模式下
  display 线程只生产预览副本，初始化失败时可恢复旧的直接写屏。
- [`src/services/ipcam_touch.h`](../src/services/ipcam_touch.h)、
  [`src/services/ipcam_touch.c`](../src/services/ipcam_touch.c) — 增加
  `event1` 多点/单点坐标范围查询、屏幕像素映射和扩展启动接口。
- [`src/main.c`](../src/main.c) — 接入 LVGL 启停顺序、触摸快照转发、
  `IPCAM_LVGL` 回退开关和默认 `IPCAM_TOUCH_DEV`。
- [`config/ipcam_config.h`](../config/ipcam_config.h) — 将实测 Goodix
  `/dev/input/event1` 作为默认触摸节点，增加 LVGL 启用及局部缓冲配置。
- [`thirdparts/lvgl/README.md`](../thirdparts/lvgl/README.md) — 说明固定的
  LVGL `v9.5.0` 外部依赖和获取方式。
- [`README.md`](../README.md) — 补充依赖、构建、部署、800×480 设备事实和
  LVGL 冒烟验证步骤。

### 关键改动说明

1. LVGL 源码固定为 `v9.5.0`，构建产物放在 `build/`；第三方源码目录由
   `.gitignore` 排除，避免把完整外部源码复制进应用仓库。
2. `ipcam_display_start_ex(..., framebuffer_writer=0, ...)` 让 LVGL 成为
   唯一 framebuffer 写入者；LVGL flush 使用 `fb_line_length`、x/y offset
   写入局部区域，适配 padding 和虚拟 framebuffer。
3. display 线程继续按摄像头帧生成独立 RGB565 预览副本，LVGL 线程在自身
   上下文内复制该副本并刷新 `lv_image`，不跨线程调用 LVGL API。
4. `ipcam_touch` 只保留一个 evdev reader，使用 `EVIOCGABS` 做坐标映射；
   `ipcam_lvgl_touch_report` 只写互斥保护的触摸快照，LVGL `read_cb` 再读取。
5. 默认开启 LVGL；如果板上初始化失败，自动重新开启 direct framebuffer
   writer。也可显式设置 `IPCAM_LVGL=0` 进行回退诊断。
6. 第一阶段 UI 使用 ASCII 标题、状态和 `TOUCH TEST` 按钮，先排除中文字库
   缺失对显示链路的干扰；现有完整设计稿页面待本阶段实机通过后按 800×480
   重新适配。

### 中文注释说明（代码类变更必填）

**中文注释**：在 [`ipcam_lvgl.c`](../src/services/ipcam_lvgl.c) 中补充了
LVGL 线程模型、单调时钟、局部 flush、RGB565 图像副本、输入互斥和失败清理
说明；在 [`ipcam_display.c`](../src/services/ipcam_display.c) 中说明了
framebuffer 所有权切换和偏移写入约束；在 [`ipcam_touch.c`](../src/services/ipcam_touch.c)
中说明了 ABS 范围映射、evdev 多点解析和非阻塞停止；在 [`main.c`](../src/main.c)
中说明了启动/停止时序、LVGL 回退和触摸快照转发；头文件、配置文件和
`Makefile` 同步补充了公开 API、板级默认值和构建规则的中文意图。

## 预期结果

1. 构建前执行 `git clone --branch v9.5.0 --depth 1
   https://github.com/lvgl/lvgl.git thirdparts/lvgl/src` 后，`make` 或
   `make ipcam-display-only` 能自动编译并链接 LVGL。
2. 开发板启动时，`ipcam_display` 仅生产预览副本，LVGL 线程独占 LCD
   framebuffer，屏幕显示 800×480 冒烟页面和摄像头画面。
3. 点击 `TOUCH TEST` 后，按钮文字变为 `TOUCH OK`；触摸日志使用
   `/dev/input/event1`，event0 电源键不会被误当作触摸。
4. 若 LVGL 初始化失败，日志出现回退提示且旧的直接 framebuffer 预览仍可用；
   `IPCAM_LVGL=0` 可手动复现回退路径。

## 备注

- 本次已完成主机端 `make test-host`（3 项测试均 PASS）、主机端
  `make -B ipcam-display-only CROSS_COMPILE= -j2`，以及目标工具链
  `make -B CROSS_COMPILE=arm-linux-gnueabihf- -j2` 编译验证；后者产出
  ELF32 ARM EABI5。尚未在用户提供的开发板上替换二进制并观察真实
  LCD/触摸效果。
- 主机端完整编码构建未作为通过标准：当前安装的 `libturbojpeg.a` 是 ARM
  静态库，不能由 x86 主机 `gcc` 链接；目标工具链完整构建已经通过。
- `evtest` 不属于本次必须依赖；实机可先观察 `LVGL ready`、`touch started`
  和 `TOUCH OK`，若触摸坐标方向相反，再根据 Goodix 轴方向增加板级变换配置。
