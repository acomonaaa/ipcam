# AGENTS.md — ipcam（项目专属）

本文件**跟随本仓库提交**，只描述 **ipcam** 专属约定。

**通用开发规范**（中文沟通、约定式提交、`main`/`dev` 分支、中文注释原则、代码才写变更记录等）见：

→ [`/home/acomon/AGENTS.md`](/home/acomon/AGENTS.md)

在本仓库工作时须**同时遵守**通用规范与下文；下文更具体的路径与模块约束优先。

---

## 项目概览

基于 **正点原子 ALIENTEK i.MX6ULL Mini** 的 C 守护进程：OV5640 采集 → LCD（`/dev/fb0`）+ 可选 MJPEG HTTP 推流。交叉编译工具链 `arm-linux-gnueabihf-`（Linaro 4.9.x），内核 **4.1.15**，BusyBox **1.29**，glibc BSP。

已确认蜂窝模组：**广和通（Fibocom）L610 Cat.1**（AT + PPP）。现有 peers/文档仍有 Quectel EC20 风格 `ttyUSB*`；做 4G 相关改动时**禁止写死** Quectel 专用路径，应将 AT/PPP 设备与 peer 名参数化。

## 官方例程参考（必须）

硬件/驱动约定以正点原子 **I.MX6ULL** 配套例程为准，根目录：

→ [`/home/acomon/ZDYZexample/01、例程源码`](/home/acomon/ZDYZexample/01、例程源码)

改采集、LCD、背光、V4L2、fb、摄像头相关逻辑前，**先对照官方例程再改 ipcam**，不要只凭通用 V4L2 经验硬写。

与「采集 → LCD」最直接相关：

| 例程 | 路径 | 用途 |
|---|---|---|
| V4L2 摄像头上屏 | `11、Linux C应用编程例程源码/25_v4l2_camera/v4l2_camera.c` | **主参考**：OV5640 → `/dev/fb0` |
| LCD / fb | `11、…/19_lcd/`（`lcd_test.c` / `lcd_info.c` / `bmp_show.c`） | fb 打开、RGB565、分辨率 |
| 竖屏 | `11、…/22_lcd_vertical_display/` | 旋转场景才需要 |
| 背光 PWM | `11、…/24_pwm/` 与裸机 `01、裸机例程/20_pwm_lcdbacklight` | 屏黑时查背光 |
| JPEG | `11、…/20_libjpeg/` | 后续 MJPEG 可对照 |

### 官方 `25_v4l2_camera` 要点（与当前 ipcam 差异）

官方已验证路径：

1. 设备：默认自动扫描并按名称/capability 选择 `mx6s-csi`；`IPCAM_VIDEO_DEV` 非空时可显式指定节点，但仍需通过 V4L2 检查；LCD 固定 `/dev/fb0`
2. 像素格式：**`V4L2_PIX_FMT_RGB565`**，与 fb 一致后 **按行 `memcpy`** 上屏（无 YUYV 软件转色）
3. 分辨率：`S_FMT` 宽高直接取 **LCD 的 `xres/yres`**（全屏采集）
4. 帧率：能设则设 **30 fps**；缓冲 **3** 路 MMAP
5. 显示：取 `min(lcd, cam)` 宽高裁剪拷贝；LCD 行步进用 `xres`（官方未用 `line_length`）

当前 ipcam 默认采集 **YUYV**（可为其它 BSP 编译选择 UYVY），再在 `ipcam_display` 里软件转 RGB565 并缩放——**与官方主路径不一致**。  
做「仅 LCD 出图」联调时：优先用官方例程确认硬件/节点/格式可用；若官方 RGB565 通、ipcam YUYV 不通，应**向官方路径对齐**（或双路径：LCD 用 RGB565，编码再另取 YUYV/JPEG），禁止在未对照例程前判定硬件坏。

## 分层约束（必须遵守）

```
core/  ←  services/  ←  main.c
```

- [`src/core/`](src/core/)：通用基础（log / param / ringbuffer / sys / ota）。**禁止**引用 `services/`。
- [`src/services/`](src/services/)：业务（V4L2 / LCD / JPEG / HTTP / net / cli）。仅可依赖 `core/`。
- [`src/main.c`](src/main.c)：只负责子系统启停编排。

保持单二进制多调用：`ipcam` / `camver` / `camctl`（由 `argv[0]` 派发）。

## 本仓库编码要点

- 语言：`gnu99`、`_GNU_SOURCE`、pthread；日志沿用现有宏（`MLOGI` / `MLOGE` 等）。
- 改动尽量小而完整，禁止顺手大改无关模块。
- 运行时配置在 `ipcam_param`（`/etc/ipcam.conf`）：magic + version + size + **CRC 只覆盖 `crc` 之后的 payload**（从 `model` 起）。禁止把 `crc` 字段本身算进 CRC。
- 进入 wpa 配置或 AT 命令的字符串（`wifi_ssid` / `wifi_psk` / `apn`）必须拒绝 `"`、`\`、CR/LF 及其他控制字符。
- libjpeg-turbo **2.1.x**：`tjCompressFromYUVPlanes(handle, planes, width, strides, height, …)`；错误串用 `tjGetErrorStr()` / `tjGetErrorStr2(handle)`。
- HTTP POST：首包可能在 `\r\n\r\n` 后已带 body，须「前缀拷贝 + 补读」；禁止无视已缓冲内容再整段 `read(Content-Length)`。
- Init 脚本跑在 **BusyBox ash**（无 bash `/dev/tcp`）。健康检查须用 `wget`（或其他 ash 可用工具）。
- 禁止对 SSID/PSK/APN 做 shell 拼串；保持 `execlp` / argv 传参。

### 本仓库中文注释落点

除遵守通用「中文注释」外，本仓库改动时重点覆盖：

- C / 头文件：[`src/`](src/)、[`config/`](config/)
- Shell：[`scripts/`](scripts/)（含 [`S90ipcam`](scripts/rootfs/etc/init.d/S90ipcam)）
- Makefile 中非显而易见的规则
- 缺陷修复处注明原问题与避坑要点（可与 [`changes/`](changes/README.md) 呼应）

示例：

```c
/*
 * 只对 crc 之后的 payload 做 CRC（从 model 起），
 * 避免把 crc 自身算进去导致存盘后永校验失败。
 */
static uint32_t param_crc(const ipcam_param_t *p)
```

## 构建与部署

- 默认：`./scripts/build.sh` 或 `make`（需交叉编译的 `libturbojpeg`）。
- 无 JPEG：`make ipcam-display-only`（stub 编码，**不得**链接 `-lturbojpeg`）。
- 安装前缀：项目内 `./output/`（不是 `../output/`）。
- 网络模式 / APN / 绑定：编译期默认在 [`config/ipcam_config.h`](config/ipcam_config.h)，运行时经 `/api/config` 修改后需重启。**不存在** `camctl net`。

## 安全与范围（本仓库）

- 改动危险面（OTA、reboot、远程绑定）须明确说明；触及 OTA 时优先要求 SHA256。
- 禁止提交二进制、`*.o`、`thirdparts/.../install` 构建产物（见 [`.gitignore`](.gitignore)）。

## 修改 4G 时

1. 在实机上探测 L610 的 AT 口与 PPP 口（`/dev/ttyUSB*` 或 UART）。
2. 增加独立 `ppp/peers` 配置；`pppd call <peer>` 的 peer 名可配置。
3. APN 继续来自 param；若扩展 AT 流程，可增加驻网等待（`CPIN` / `CSQ` / `CGREG`/`CEREG`）。

---

## 本仓库变更记录目录

通用规则见 [`/home/acomon/AGENTS.md`](/home/acomon/AGENTS.md)。本仓库落地为：

- 目录：[`changes/`](changes/README.md)
- 模板：[`changes/_TEMPLATE.md`](changes/_TEMPLATE.md)
- 索引：每新增一条须更新 [`changes/README.md`](changes/README.md)
- 命名：`changes/YYYY-MM-DD-简短中文标题.md`
- 时间：**UTC+8**；源码链接用**相对路径**（如 [`../src/core/ipcam_param.c`](src/core/ipcam_param.c) 写在 changes 文件内时用 `../src/...`）

### 本仓库「代码类 / 文档类」划分

**需要记录**：[`src/`](src/)、[`config/`](config/)、[`Makefile`](Makefile)、[`scripts/`](scripts/) 下会编译或板上运行的脚本等。  

**不需要记录**：本文件、[`README.md`](README.md)、[`changes/`](changes/) 内说明/模板/索引等纯文档。

### 本仓库推荐工作流（结合通用分支模型）

1. 在 **`dev`** 上开发（从 `main` 签出）  
2. 改代码 + 中文注释 +（若属代码类）写 `changes/` 并更新索引  
3. 用户要求提交 → 推到 **`dev`**  
4. 审核通过后 PR 合入 **`main`**  
