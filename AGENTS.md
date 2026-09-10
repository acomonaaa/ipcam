# AGENTS.md — ipcam

本文件为 `/home/acomon/project/ipcam` 的 Agent 开发规范。**所有面向用户的说明、注释补充、提交说明、PR 描述、计划与回复均须使用中文**（代码标识符、命令、路径、协议关键字保持英文原文）。

## 项目概览

基于 **正点原子 ALIENTEK i.MX6ULL Mini** 的 C 守护进程：OV5640 采集 → LCD（`/dev/fb0`）+ 可选 MJPEG HTTP 推流。交叉编译工具链 `arm-linux-gnueabihf-`（Linaro 4.9.x），内核 **4.1.15**，BusyBox **1.29**，glibc BSP。

已确认蜂窝模组：**广和通（Fibocom）L610 Cat.1**（AT + PPP）。现有 peers/文档仍有 Quectel EC20 风格 `ttyUSB*`；做 4G 相关改动时**禁止写死** Quectel 专用路径，应将 AT/PPP 设备与 peer 名参数化。

## 板端与 Ubuntu/NFS 调试配置（当前已确认）

以下参数是本项目目前连接正点原子 ALIENTEK i.MX6ULL Mini 开发板时的调试约定。
网络环境或板级介质改变后，先用 Ubuntu 的 `ip addr`、U-Boot 的 `printenv` 和板端
`ip addr` 重新确认，不要把这里的地址当作所有环境都永久不变的配置。

### 网络与 NFS

- 开发板 IP：`192.168.1.50/24`
- Ubuntu 调试网卡 `ens33` / NFS、TFTP 服务器 IP：`192.168.1.253/24`
- 调试网关：`192.168.1.1`
- 开发板 U-Boot 当前约定：`ethact=FEC1`、`netmask=255.255.255.0`；已记录的板卡 MAC 为
  `00:04:9f:01:02:03`
- NFS 根文件系统目录：`/home/acomon/linux/nfs/rootfs`
- 当前调试使用的 NFS 参数：
  `root=/dev/nfs nfsroot=192.168.1.253:/home/acomon/linux/nfs/rootfs,v3,proto=tcp rw`
- 必须显式使用 `v3,proto=tcp`：4.1.15 内核默认可能协商 NFSv2，而当前 Ubuntu
  NFS 服务端通常不提供 NFSv2。

### U-Boot 启动与串口

- 网络启动入口：`run mybootnet`。该命令从 Ubuntu TFTP 目录加载 `zImage` 和
  `imx6ull-14x14-emmc-4.3-800x480-c.dtb`，再执行 `bootz`；当前开发调试默认从网络启动，
  不使用 `run mybootemmc` 作为日常验证入口。
- 常用加载地址：内核 `0x80800000`，设备树 `0x83000000`。
- 串口终端参数：`115200 8N1`。
- Linux 启动链：NFS rootfs 的 `/etc/init.d/rcS` 负责加载 `mx6s_capture`、
  `ov5640_camera`，随后自动调用 `S90ipcam` 启动应用；正常情况下不需要手工再启动一次。

### 板级设备与应用入口

- LCD：`/dev/fb0`，实测 `800×480 RGB565`。
- 触摸：`/dev/input/event1`（Goodix）；`event0` 是电源键，禁止把它误设为触摸设备。
- 摄像头：当前实测 CSI 节点为 `/dev/video1`、sysfs 名称为 `mx6s-csi`；应用默认
  `IPCAM_VIDEO_DEV` 为空并自动扫描，不应把 `video1` 固化为通用前提。
- SD 卡存储默认目录：`/mnt/sdcard`；拍照、录像和状态接口必须沿用挂载检查，不能把
  NFS rootfs 的剩余空间当作 SD 卡可用空间。
- HTTP 服务：`http://192.168.1.50:8080/`；健康检查为
  `http://192.168.1.50:8080/healthz`，状态接口为
  `http://192.168.1.50:8080/api/status`。
- 串口实时日志优先输出到 `/dev/console`；console 不可用时回退到
  `/var/log/ipcam.log`。可用 `IPCAM_LOG_FILE` 覆盖日志位置。

### Ubuntu 构建与部署约定

- 项目目录：`/home/acomon/project/ipcam`。
- 交叉编译器前缀：`arm-linux-gnueabihf-`；完整版本依赖项目内
  `thirdparts/libjpeg-turbo/install`，无 `libturbojpeg` 时使用
  `make ipcam-display-only` 验证 LCD/采集链路。
- LVGL 依赖目录：`thirdparts/lvgl/src`，固定使用 LVGL `v9.5.0`；配置文件为项目根目录
  的 `lv_conf.h`。
- ARM 完整程序部署到 NFS rootfs 的目标：
  `/home/acomon/linux/nfs/rootfs/usr/bin/ipcam`
- 启动脚本部署到 NFS rootfs 的目标：
  `/home/acomon/linux/nfs/rootfs/etc/init.d/S90ipcam`
- 修改后常用部署命令（Ubuntu 执行）：

  ```sh
  cd /home/acomon/project/ipcam
  make -B
  install -m 755 ipcam /home/acomon/linux/nfs/rootfs/usr/bin/ipcam
  install -m 755 scripts/rootfs/etc/init.d/S90ipcam \
      /home/acomon/linux/nfs/rootfs/etc/init.d/S90ipcam
  sync
  ```

- 板端加载新程序：`/etc/init.d/S90ipcam restart`；也可以重新执行
  `run mybootnet`，因为板端会重新挂载同一个 NFS rootfs。
- 可覆盖的板级环境变量包括 `IPCAM_VIDEO_DEV`、`IPCAM_FB_DEV`、`IPCAM_TOUCH_DEV`、
  `IPCAM_STORAGE_ROOT`、`IPCAM_LOG_FILE`、`IPCAM_4G_AT_DEV` 和 `IPCAM_4G_PPP_PEER`；
  覆盖后应在启动日志中确认最终生效值。

## 分层约束（必须遵守）

```
core/  ←  services/  ←  main.c
```

- `src/core/`：通用基础（log / param / ringbuffer / sys / ota）。**禁止**引用 `services/`。
- `src/services/`：业务（V4L2 / LCD / JPEG / HTTP / net / cli）。仅可依赖 `core/`。
- `src/main.c`：只负责子系统启停编排。

保持单二进制多调用：`ipcam` / `camver` / `camctl`（由 `argv[0]` 派发）。

## 编码规范

- 语言：`gnu99`、`_GNU_SOURCE`、pthread；日志沿用现有宏（`MLOGI` / `MLOGE` 等）。
- 改动尽量小而完整，禁止顺手大改无关模块。
- 运行时配置在 `ipcam_param`（`/etc/ipcam.conf`）：magic + version + size + **CRC 只覆盖 `crc` 之后的 payload**（从 `model` 起）。禁止把 `crc` 字段本身算进 CRC。
- 进入 wpa 配置或 AT 命令的字符串（`wifi_ssid` / `wifi_psk` / `apn`）必须拒绝 `"`、`\`、CR/LF 及其他控制字符。
- libjpeg-turbo **2.1.x**：`tjCompressFromYUVPlanes(handle, planes, width, strides, height, …)`；错误串用 `tjGetErrorStr()` / `tjGetErrorStr2(handle)`。
- HTTP POST：首包可能在 `\r\n\r\n` 后已带 body，须「前缀拷贝 + 补读」；禁止无视已缓冲内容再整段 `read(Content-Length)`。
- Init 脚本跑在 **BusyBox ash**（无 bash `/dev/tcp`）。健康检查须用 `wget`（或其他 ash 可用工具）。
- 禁止对 SSID/PSK/APN 做 shell 拼串；保持 `execlp` / argv 传参。

### 中文注释（强制）

**所有代码修改必须同步补充或更新详细的中文注释。** 无中文注释说明意图的改动视为未完成。

适用范围：

- C 源码 / 头文件（[`src/`](src/)、[`config/`](config/)）
- Shell 脚本（如 [`scripts/`](scripts/)）
- Makefile 中非显而易见的逻辑（简短中文注释即可）

必须注释的内容（用中文写清楚「为什么」，避免只复述代码字面意思）：

1. **新增/修改的函数、静态函数**：文件内或函数上方说明职责、关键参数约定、返回值含义、线程/信号安全注意点  
2. **非显而易见的算法、协议、时序、魔数、错误处理分支**：在对应代码块上方或行尾说明原因  
3. **修复类改动**：在修复点用中文注明原问题与修复要点（可与 [`changes/`](changes/README.md) 记录呼应，注释里写摘要即可）  
4. **对外 API（头文件）**：更新中文说明，与实现保持一致  
5. **脚本关键步骤**（拨号、看门狗、安装路径等）：用中文注释标出目的与 BusyBox/板级约束  

风格约定：

- 注释正文使用**中文**；标识符、路径、命令、协议名保留英文  
- 优先块注释说明意图；不要为每一行简单赋值堆砌无信息注释  
- 删除或替换旧逻辑时，同步删除过时注释，禁止留下错误注释  
- 不得用英文注释代替本条要求（既有英文注释可保留，但**本次改动相关处必须有充分中文说明**）

反例 / 正例：

```c
/* 反例：只复述代码 */
/* 把 crc 设为 param_crc 的返回值 */
s_param.crc = param_crc(&s_param);

/* 正例：说明为什么 */
/* CRC 只覆盖 model 起的 payload，避免把 crc 字段自身算进去导致存盘后永校验失败 */
s_param.crc = param_crc(&s_param);
```

### 中文注释（强制）

**所有代码修改必须附带详细的中文注释**，与 [`changes/`](changes/README.md) 变更记录互补：记录写清来龙去脉，注释写清「这段代码当下在做什么、为何这样写」。

#### 适用范围

- C 源码 / 头文件（`src/`、`config/`）
- Shell 脚本（如 [`scripts/`](scripts/build.sh)、[`S90ipcam`](scripts/rootfs/etc/init.d/S90ipcam)）
- Makefile 中非显而易见的规则与变量（用 `#` 中文说明）
- 新增或改动的 PPP peers、关键配置片段

#### 必须注释的内容

1. **文件 / 模块头**（新增文件或大幅改写时）：模块职责、依赖边界、线程/进程模型（若适用）。
2. **函数 / 重要静态函数**：用途、关键参数含义、返回值约定、调用约束（如「须持锁」「异步信号不安全」）。
3. **非显而易见的逻辑块**：协议步骤、字节序/布局、超时与重试、错误分支取舍、与硬件/模组相关的约定。
4. **缺陷修复处**：用简短中文说明「原问题是什么、此次如何避免」；可与变更记录交叉引用文件名，但注释本身须自洽可读。
5. **安全相关**：过滤字符、权限检查、路径约束等，注明防护目的。

#### 写法要求

- **语言：中文**（标识符、API 名、路径、AT 命令等专有名词保留英文）。
- **详细**：写清意图与约束，禁止只写「初始化」「处理一下」等空话。
- **紧贴改动**：只给本次触及的代码补足注释；禁止为凑数给全文件无关旧代码大规模「翻译式」注释，除非用户明确要求全面补注释。
- **风格**：C 用 `/* ... */` 或 `//`；与周边文件已有风格保持一致，同一文件内统一。
- **禁止**：用注释复述无信息量的代码（如 `i++; // i 加一`）；禁止在注释里堆砌过时信息（改逻辑时同步改注释）。

#### 示例

```c
/*
 * 计算配置 payload 的 CRC32。
 * 只覆盖 crc 字段之后的数据（从 model 起），避免把 crc 自身算进校验
 * 导致「保存后永远无法通过加载校验」。
 */
static uint32_t param_crc(const ipcam_param_t *p)
```

```c
/* 拒绝会破坏 wpa 明文 conf 引号或 AT 字符串嵌入的危险字符 */
if (string_has_unsafe_chars(ssid)) return -1;
```

未按本条补齐中文注释的代码修改，视为未完成（须与变更记录一并补全后再结束任务）。

## 构建与部署

- 默认：`./scripts/build.sh` 或 `make`（需交叉编译的 `libturbojpeg`）。
- 无 JPEG：`make ipcam-display-only`（stub 编码，**不得**链接 `-lturbojpeg`）。
- 安装前缀：项目内 `./output/`（不是 `../output/`）。
- 网络模式 / APN / 绑定：编译期默认在 `config/ipcam_config.h`，运行时经 `/api/config` 修改后需重启。**不存在** `camctl net`。

### Windows 下修改 Linux 项目（强制）

- 当 Agent 当前运行环境为 Windows，而项目目标环境为 Linux（包括嵌入式 Linux）时，只做代码修改及必要的中文注释、文档和变更记录更新，不在 Windows 上进行编译或依赖编译的测试。
- 不为验证本次修改而安装或配置编译工具链，也不绕道 WSL、容器或远程 Linux 执行编译；除非用户另有明确要求，编译验证留待用户切换到 Linux 环境后进行。
- 修改完成后，检查差异，仅提交本次任务相关文件，并推送到 GitHub；本条视为该场景下执行 `git commit` / `git push` 的持续授权，不得夹带用户已有的无关改动。
- 最终回复必须说明修改与 GitHub 提交结果，明确标注“未进行编译验证”，并提醒用户：“请切换到 Linux 环境进行编译和验证。”不得将未验证的修改描述为已通过测试；提交或推送失败时须如实说明。

## 安全与范围

- 改动危险面（OTA、reboot、远程绑定）须明确说明；触及 OTA 时优先要求 SHA256。
- 禁止提交二进制、`*.o`、`thirdparts/.../install` 构建产物（见 `.gitignore`）。
- 未经用户要求，不要改 `.cursor/plans/` 下的计划文件。
- 仅在用户明确要求或符合上文“Windows 下修改 Linux 项目”的持续授权时执行 `git commit` / `git push`。

## 修改 4G 时

1. 在实机上探测 L610 的 AT 口与 PPP 口（`/dev/ttyUSB*` 或 UART）。
2. 增加独立 `ppp/peers` 配置；`pppd call <peer>` 的 peer 名可配置。
3. APN 继续来自 param；若扩展 AT 流程，可增加驻网等待（`CPIN` / `CSQ` / `CGREG`/`CEREG`）。

---

## 文档与沟通语言

- Agent 对用户的说明、变更总结、审查意见、计划正文：**一律中文**。
- 提交信息的 `subject` / `body` / `footer`：**使用中文**（`type`、`scope` 保持英文关键字）。
- PR 标题与正文：中文；代码、路径、命令、协议名保持英文。

## Git 提交规范（约定式提交 Conventional Commits）

本仓库采用社区事实标准 **Conventional Commits**。格式：

```
<type>(<scope>): <subject>

<body>

<footer>
```

- **Header（必需）**：`type` + 可选 `scope` + `subject`
- **Body（可选）**：说明动机与做法（写「为什么 / 怎么做」，不要复述代码）
- **Footer（可选）**：关联 Issue，或记录破坏性变更

### type（必需）

| 类型 | 说明 |
| :--- | :--- |
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `docs` | 仅文档 |
| `style` | 格式调整，不影响行为 |
| `refactor` | 重构（非功能、非修复） |
| `perf` | 性能优化 |
| `test` | 测试增改 |
| `chore` | 构建/工具/杂项 |
| `ci` | CI/CD 配置 |
| `revert` | 回滚某次提交 |

### scope（可选）

用模块名标明影响范围，例如：`param`、`encode`、`stream`、`net4g`、`netwifi`、`ota`、`build`、`docs`。

示例：`fix(param): 修复配置 CRC 自包含导致无法加载`

### subject（必需）

- 不超过 **50** 个字符（中文按显示长度控制，宜短）
- 祈使、现在时语义（「修复…」「增加…」，不要「已修复…」）
- 不加句号结尾
- 与 `type` 之间：`type(scope): ` 后直接接中文主题

### Body / Footer

- Body 每行建议不超过 **72** 字符
- Footer 可用 `Closes #123` / `Fixes #456`
- 破坏性变更：`type!` 或 Footer 中写 `BREAKING CHANGE: <中文说明>`

### 提交示例

```
feat(stream): 支持组装已缓冲的 HTTP POST body

首包常在头尾分隔符后已含 body，补齐 Content-Length 后再解析，
避免 /api/config 与 /api/ota 空 body 超时。
```

```
fix(netwifi): 去掉 wpa_supplicant -B 导致 PID 误判

-B 会二次守护化使子进程立刻退出，父进程误判启动失败。
改为前台运行并由父进程跟踪真实 PID。
```

```
feat!: 调整 OTA 暂存路径约定

BREAKING CHANGE: 暂存文件固定为 <bin>.new，自定义安装前缀需同步修改。
```

## 协作与分支 / PR

1. **原子化提交**：一次提交只做一件事。
2. **分支命名**：`type/简短描述` 或 `type/issue号-简短描述`，例如 `fix/param-crc`、`feat/l610-ppp`。
3. **PR**：单一职责；标题与描述用中文写清变更、原因、验证方式；建议 **Squash and Merge**。
4. commit / push 需用户明确要求或符合上文“Windows 下修改 Linux 项目”的持续授权；开 PR 仍需用户明确要求。

---

## 变更记录制度（强制，仅针对代码修改）

**仅当修改会改变程序/构建/运行时行为的「代码类」文件时，必须同步撰写变更记录。**  
**纯写文档不需要** `changes/` 记录。

### 需要写变更记录（代码类）

- C 源码与头文件：[`src/`](src/)、[`config/*.h`](config/)
- 构建与脚本：[`Makefile`](Makefile)、[`scripts/`](scripts/) 下会参与编译或板上运行的脚本（含 init、ppp peers 等）
- 其它直接参与编译、打包、启动、拨号、守护进程行为的文件

### 不需要写变更记录（文档类）

- [`AGENTS.md`](AGENTS.md)、[`README.md`](README.md)、[`changes/`](changes/README.md) 内说明/模板/索引
- 仅注释规范说明、计划说明、与运行无关的 Markdown
- 注意：若同一次改动**既改代码又改文档**，仍须为**代码部分**写一条变更记录（文档改动可在同条记录的方案里顺带提及，不必单独再开文档记录）

无对应代码变更记录却改了上述代码类文件，视为修改未完成，不得结束任务；用户要求提交时不得漏带记录文件。

### 存放位置

- 目录：项目根下 [`changes/`](changes/README.md)
- 模板：[`changes/_TEMPLATE.md`](changes/_TEMPLATE.md)
- 索引：每新增一条记录，必须更新 [`changes/README.md`](changes/README.md) 索引表

### 文件命名

```
changes/YYYY-MM-DD-简短中文标题.md
```

同一天多条时加后缀：`YYYY-MM-DD-标题-02.md`。

### 时间

- **修改时间必须使用 UTC+8（北京时间）**
- 格式建议：`YYYY-MM-DD HH:MM:SS（UTC+8）`
- 写入前用 `TZ=Asia/Shanghai date '+%Y-%m-%d %H:%M:%S %z'` 取当前时间

### 文档必须包含的章节（须写详细）

1. **元信息**：修改时间、作者、关联提交、影响模块  
2. **问题现象**：环境、操作步骤、可观察结果（可复现）  
3. **问题分析**：排查过程、对照了哪些代码/日志、排除了什么  
4. **问题根因**：明确根因，不只写表象  
5. **修改方案**：改动点、取舍理由、**变更文件列表**、**中文注释说明**  
6. **预期结果**：修改后行为 + 建议验证步骤  
7. **备注（可选）**：风险、回滚、后续待办  

### 相对路径可点击链接（强制）

记录正文中凡提到源码、脚本、配置，须使用 **Markdown 相对路径链接**，便于在 GitHub / IDE 跳转。

示例（写在 `changes/xxx.md` 内时）：

```markdown
- 修复函数见 [`param_crc()`](../src/core/ipcam_param.c)
- 启动脚本见 [`S90ipcam`](../scripts/rootfs/etc/init.d/S90ipcam)
```

禁止只写无法跳转的裸路径而不加链接；禁止使用仅本机有效的绝对路径（如 `/home/acomon/...`）作为跳转链接。

### 工作流

1. 分析并修改**代码**，**同步写好详细中文注释**（见上文「中文注释」）  
2. **立即**按模板新增/更新 `changes/` 记录，并更新索引（纯文档改动跳过本步）  
3. 用户要求提交时：代码提交须纳入对应记录文件；commit message 遵循上文约定式提交（中文 subject）  


---

# 附录：pen.dev → LVGL 设计转码规范

## 1. Purpose

This repository uses an **AI-first embedded GUI development workflow** for:

- NXP i.MX6ULL
- Embedded Linux
- LVGL 9
- Windows development host
- Ubuntu virtual machine accessed through SSH
- pen.dev as the primary GUI design tool
- AI Agent as the primary design/code assistant

The standard workflow is:

```text
Requirements
    ↓
AI discussion
    ↓
pen.dev design through MCP
    ↓
Human design review
    ↓
Approved .pen design
    ↓
AI converts design to LVGL 9 C code
    ↓
PC / Ubuntu LVGL Simulator
    ↓
Simulator validation
    ↓
ARM cross compilation
    ↓
i.MX6ULL deployment
    ↓
Board validation
    ↓
Performance optimization
```

All AI agents working in this repository MUST follow the rules in this document.

---

# 2. Target Platform

Default target:

```text
SoC:            NXP i.MX6ULL
CPU:            ARM Cortex-A7
GPU:            None
OS:             Embedded Linux
GUI Framework:  LVGL 9
Display:        LCD
Typical Size:   800x480
Typical Format: RGB565
Input:          Linux evdev
Display Backend:
                framebuffer and/or DRM/KMS
```

The i.MX6ULL has limited CPU and graphics performance.

Therefore all GUI implementations MUST prioritize:

- low CPU usage;
- low memory usage;
- low redraw cost;
- fast startup;
- predictable performance;
- long-term stability;
- touch responsiveness.

The agent MUST NOT design the UI as if the target were a desktop GPU system.

---

# 3. Development Environment

Expected development topology:

```text
Windows
│
├── AI Agent
├── pen.dev
├── MCP
├── IDE
└── SSH
     │
     ▼
Ubuntu Virtual Machine
│
├── Git repository
├── LVGL
├── LVGL Simulator
├── CMake / Make
├── ARM cross compiler
├── Buildroot / Yocto if required
└── deployment scripts
     │
     ▼
i.MX6ULL
│
├── Embedded Linux
├── LCD
├── Touchscreen
├── framebuffer / DRM
└── evdev
```

The Ubuntu VM SHOULD be treated as the primary compilation environment.

The Windows host SHOULD primarily be used for:

- pen.dev;
- AI interaction;
- MCP;
- editing;
- design review;
- SSH access.

---

# 4. Design Source of Truth

The approved `.pen` file is the **single source of truth for GUI visual design**.

Recommended location:

```text
design/
└── hmi.pen
```

The `.pen` file owns:

- screen structure;
- widget hierarchy;
- layout;
- dimensions;
- spacing;
- colors;
- typography;
- visual states;
- navigation;
- icons;
- component appearance.

The LVGL C implementation MUST reflect the approved `.pen` design.

---

# 5. Design Drift Is Not Allowed

The following changes MUST normally be made in the `.pen` design first:

- widget position;
- widget size;
- spacing;
- colors;
- font sizes;
- component structure;
- navigation structure;
- visual states;
- icon placement;
- visual hierarchy.

The agent MUST NOT permanently make a design change only in C code.

Bad workflow:

```text
pen.dev design
      ↓
LVGL C

Developer moves button manually in C

      ↓

.pen and C no longer match
```

Correct workflow:

```text
Change requested
      ↓
Modify .pen
      ↓
Review
      ↓
Update LVGL implementation
```

Temporary board-side experiments are allowed when debugging.

If a temporary implementation change becomes permanent, the design MUST be updated accordingly.

---

# 6. Mandatory GUI Development Pipeline

Every new screen or major visual feature MUST follow:

```text
REQUIREMENTS
    ↓
DESIGN SPEC
    ↓
PEN DESIGN
    ↓
HUMAN REVIEW
    ↓
DESIGN APPROVED
    ↓
LVGL IMPLEMENTATION
    ↓
SIMULATOR BUILD
    ↓
SIMULATOR VALIDATION
    ↓
CROSS BUILD
    ↓
BOARD DEPLOYMENT
    ↓
BOARD VALIDATION
```

Do not skip stages without a technical reason.

---

# 7. Stage 1 — Requirements Discussion

Before modifying pen.dev or LVGL code, the agent SHOULD understand:

- target resolution;
- required screens;
- navigation model;
- displayed values;
- user actions;
- touch behavior;
- alarm states;
- error states;
- offline states;
- loading states;
- disabled states;
- data update frequency;
- real-time chart requirements;
- hardware limitations.

For substantial GUI features, the agent SHOULD first produce a short design specification.

Example:

```text
Display: 800x480

Header:
height 48px

Sidebar:
width 128px

Footer:
height 32px

Content:
remaining area

Spacing:
8 / 12 / 16 / 24px

Touch target:
minimum approximately 44-48px
```

---

# 8. Stage 2 — pen.dev Design

When pen.dev MCP access is available, the AI SHOULD use it to create or modify the GUI.

The AI MAY:

- create screens;
- create components;
- adjust layout;
- modify text;
- modify colors;
- modify spacing;
- add icons;
- create navigation structures;
- create visual states.

The agent MUST NOT claim that pen.dev was modified unless the MCP/tool operation actually succeeded.

If MCP is unavailable, the agent MUST NOT fabricate a design result.

---

# 9. Stage 3 — Human Review

The human developer reviews the design before implementation.

Possible outcomes:

```text
Rejected
    ↓
AI modifies .pen
    ↓
Review again
```

or:

```text
Approved
    ↓
LVGL implementation begins
```

For a newly designed screen, the agent SHOULD NOT treat it as final until approval is given.

Small implementation fixes that do not change visual behavior do not require renewed approval.

---

# 10. Stage 4 — Convert Design to LVGL

After design approval, the AI converts the `.pen` design into maintainable LVGL 9 C code.

The conversion MUST NOT simply generate one giant source file.

The implementation SHOULD preserve:

- component hierarchy;
- spacing;
- colors;
- sizes;
- alignment;
- typography;
- interaction states.

The agent SHOULD translate design concepts into appropriate LVGL concepts.

Examples:

```text
Design flex layout
      ↓
LVGL Flex

Design grid layout
      ↓
LVGL Grid

Reusable card
      ↓
Reusable LVGL component

Shared visual rules
      ↓
Reusable lv_style_t
```

---

# 11. Recommended Repository Structure

Preferred structure:

```text
project/
│
├── AGENTS.md
├── CMakeLists.txt
│
├── design/
│   ├── hmi.pen
│   └── design-notes.md
│
├── src/
│   │
│   ├── main.c
│   │
│   ├── ui/
│   │   │
│   │   ├── ui.c
│   │   ├── ui.h
│   │   │
│   │   ├── screens/
│   │   │   ├── screen_home.c
│   │   │   ├── screen_home.h
│   │   │   ├── screen_control.c
│   │   │   ├── screen_control.h
│   │   │   ├── screen_alarm.c
│   │   │   ├── screen_alarm.h
│   │   │   ├── screen_settings.c
│   │   │   └── screen_settings.h
│   │   │
│   │   ├── components/
│   │   │   ├── status_card.c
│   │   │   ├── status_card.h
│   │   │   ├── nav_button.c
│   │   │   ├── nav_button.h
│   │   │   ├── status_bar.c
│   │   │   └── status_bar.h
│   │   │
│   │   ├── styles/
│   │   │   ├── ui_styles.c
│   │   │   └── ui_styles.h
│   │   │
│   │   └── assets/
│   │       ├── ui_images.c
│   │       ├── ui_images.h
│   │       ├── ui_fonts.c
│   │       └── ui_fonts.h
│   │
│   ├── ui_logic/
│   │   ├── ui_actions.c
│   │   ├── ui_actions.h
│   │   ├── ui_data.c
│   │   └── ui_data.h
│   │
│   ├── app/
│   │   ├── device.c
│   │   ├── device.h
│   │   ├── sensor.c
│   │   ├── sensor.h
│   │   ├── uart.c
│   │   ├── uart.h
│   │   ├── network.c
│   │   ├── network.h
│   │   ├── database.c
│   │   └── database.h
│   │
│   └── platform/
│       ├── lv_port_disp.c
│       ├── lv_port_disp.h
│       ├── lv_port_indev.c
│       └── lv_port_indev.h
│
├── simulator/
│
├── scripts/
│
└── tests/
```

If an existing repository already uses another sensible structure, preserve it.

Do not perform unnecessary large-scale restructuring.

---

# 12. Never Generate a Giant `ui.c`

The agent MUST NOT create one monolithic file containing:

- every screen;
- every widget;
- every style;
- all callbacks;
- UART code;
- network code;
- database code;
- threading code.

Bad:

```text
ui.c
12000 lines
```

Preferred:

```text
screen_home.c
screen_alarm.c
status_card.c
nav_button.c
ui_styles.c
ui_actions.c
```

Each file SHOULD have one clear responsibility.

---

# 13. UI Layer Responsibilities

`src/ui/` SHOULD contain only GUI-related concerns.

Examples:

- widget creation;
- layout;
- style;
- visual states;
- screen creation;
- reusable components;
- image assets;
- fonts.

The UI layer MUST NOT directly implement:

- serial protocol;
- TCP protocol;
- SQLite queries;
- blocking file I/O;
- device discovery;
- sensor drivers.

---

# 14. UI Logic Layer

`src/ui_logic/` acts as the bridge between UI and application logic.

Example:

```text
LVGL button
    ↓
ui_action_start_device()
    ↓
device_request_start()
    ↓
UART subsystem
```

Preferred callback:

```c
static void on_start_clicked(lv_event_t *e)
{
    (void)e;

    ui_action_start_device();
}
```

Then:

```c
void ui_action_start_device(void)
{
    device_request_start();
}
```

Do NOT write this directly inside the LVGL callback:

```c
int fd = open("/dev/ttymxc2", O_RDWR);

write(fd, command, sizeof(command));
```

---

# 15. Business Logic Layer

The `app/` layer owns:

- device state;
- sensors;
- UART;
- networking;
- database;
- protocol handling;
- application state machines.

Example:

```text
UI
 ↓
ui_actions
 ↓
device
 ↓
uart
 ↓
hardware
```

The business layer SHOULD be testable independently of LVGL whenever practical.

---

# 16. Platform Layer

`platform/` owns platform-specific integration such as:

- LVGL display backend;
- framebuffer;
- DRM/KMS;
- evdev;
- touchscreen;
- Linux input devices.

Examples:

```text
lv_port_disp.c
lv_port_indev.c
```

The screen implementation MUST NOT depend directly on `/dev/fb0` or `/dev/input/eventX`.

---

# 17. LVGL Version

This repository targets:

```text
LVGL 9
```

The agent MUST:

- use LVGL 9 APIs;
- inspect the actual LVGL version when uncertain;
- compile after API changes;
- avoid mixing LVGL 8 and LVGL 9 code.

The agent MUST NOT invent LVGL APIs based only on memory.

If API behavior is uncertain, inspect:

- local LVGL headers;
- repository examples;
- installed documentation.

---

# 18. Naming Convention

Use semantic names.

Good:

```text
screen_home
screen_alarm
screen_settings

btn_start
btn_stop
btn_reset

label_temperature
label_pressure
label_flow

chart_realtime

card_temperature

status_network
status_uart
```

Bad:

```text
obj1
obj2
button4
label7
screen3
container5
```

Preferred C function naming:

```c
ui_screen_home_create();
ui_screen_home_destroy();

ui_status_card_create();

ui_update_temperature();

ui_action_start_device();

device_start();

uart_send_command();
```

---

# 19. Component Reuse

Repeated UI elements MUST be implemented as reusable components.

Examples:

```text
status card
navigation button
alarm row
parameter row
status indicator
dialog
numeric value card
```

Do not duplicate nearly identical LVGL widget construction across screens.

For example:

```c
ui_status_card_create(parent, ...);
```

is preferred over copying the same 30 lines four times.

---

# 20. Style Reuse

Shared styles SHOULD use reusable `lv_style_t`.

Avoid configuring identical style properties individually on every object.

Preferred:

```text
style_card
style_nav_button
style_title
style_value
style_alarm
```

Benefits:

- smaller code;
- easier maintenance;
- consistent design;
- easier AI modifications;
- less risk of visual drift.

---

# 21. i.MX6ULL Performance Rules

The target has no GPU.

Therefore the agent MUST optimize for software rendering.

## Avoid

The agent SHOULD avoid or minimize:

- blur;
- complex shadows;
- large shadows;
- large transparent overlays;
- unnecessary alpha blending;
- large gradients;
- full-screen animations;
- continuous full-screen transitions;
- frequent image scaling;
- frequent image rotation;
- oversized image resources;
- unnecessary redraws;
- excessive widget count;
- excessive nested containers.

## Prefer

The agent SHOULD prefer:

- flat visual design;
- solid colors;
- simple borders;
- small-radius corners;
- reusable styles;
- Flex layout;
- Grid layout;
- partial redraw;
- moderate animation;
- simple state transitions;
- static assets sized close to final display size.

---

# 22. Forbidden GPU Assumptions

The agent MUST NOT assume hardware OpenGL ES acceleration is available on i.MX6ULL.

Do not introduce GPU-specific rendering requirements unless the hardware target changes.

The design SHOULD remain functional with LVGL software rendering.

---

# 23. Animation Rules

Animations are allowed but MUST be conservative.

Acceptable examples:

```text
button press feedback
small progress animation
short page fade
small indicator movement
temporary notification
```

Avoid:

```text
continuous background animation
full-screen 60 FPS transitions
large transparency animation
large-scale zoom
continuous rotating graphics
```

Animation MUST NOT interfere with touch responsiveness.

---

# 24. Touch UI Rules

The UI is intended for physical touchscreens.

Interactive elements SHOULD have adequate touch targets.

Typical recommendation:

```text
minimum height:
approximately 44-48px
```

Avoid:

- tiny text buttons;
- tightly packed controls;
- small icons as the only click target.

Touch targets MAY be larger than their visible graphics.

---

# 25. Font Rules

Fonts can consume significant memory.

The agent SHOULD:

- use as few font sizes as practical;
- avoid embedding unnecessary characters;
- subset Chinese fonts where practical;
- avoid embedding multiple complete CJK font sets;
- reuse font resources.

Suggested UI hierarchy:

```text
small:
14px

normal:
16px

section:
18-20px

large value:
24-32px
```

Exact values MUST follow the approved design.

---

# 26. Image Resource Rules

Images SHOULD be prepared specifically for the target.

Prefer:

- correct target dimensions;
- appropriate LVGL-compatible formats;
- compressed resources where supported;
- RGB565-compatible assets when appropriate.

Avoid relying on runtime scaling for large images.

The agent SHOULD consider:

```text
flash/storage cost
RAM cost
decode cost
rendering cost
```

before adding large assets.

---

# 27. Chart Rules

Real-time charts can be expensive.

The agent SHOULD:

- limit visible points;
- limit update frequency;
- avoid full-screen charts unless required;
- avoid unnecessarily high sampling rates;
- update only when new data exists.

For example:

```text
sensor collection:
100 Hz
```

does NOT imply:

```text
GUI redraw:
100 Hz
```

A GUI update of approximately:

```text
5-20 Hz
```

may be sufficient depending on the product.

---

# 28. LVGL Thread Safety

LVGL MUST be treated as single-thread-owned unless the project explicitly implements a safe LVGL synchronization model.

Worker threads MUST NOT casually call LVGL APIs.

Bad:

```text
UART thread
   ↓
lv_label_set_text()
```

Preferred:

```text
UART thread
   ↓
update application data
   ↓
send message/event
   ↓
GUI thread
   ↓
lv_label_set_text()
```

The agent MUST understand the project's existing threading model before updating UI from worker threads.

---

# 29. Blocking Operations

LVGL event callbacks MUST NOT perform long blocking operations.

Do not block the UI thread with:

- long serial reads;
- network requests;
- database scans;
- file copies;
- sleep();
- hardware timeout waits.

Instead:

```text
UI event
   ↓
submit request
   ↓
worker thread
   ↓
result
   ↓
GUI update
```

---

# 30. UI Data Model

The UI SHOULD consume structured application state.

Example:

```c
typedef struct {
    float temperature;
    float pressure;
    float flow;

    bool network_online;
    bool uart_online;
    bool device_running;
} app_status_t;
```

Then:

```text
hardware
   ↓
app_status_t
   ↓
ui_data
   ↓
LVGL widgets
```

Avoid scattering unrelated global variables throughout screen source files.

---

# 31. Simulator Is Mandatory

UI code SHOULD be tested on PC before board deployment.

Recommended:

```text
Ubuntu VM
   ↓
LVGL
   ↓
SDL2
   ↓
800x480 simulator window
```

The simulator is used to check:

- build success;
- crashes;
- page layout;
- navigation;
- label rendering;
- component reuse;
- callback behavior;
- mock sensor data;
- chart behavior.

---

# 32. Simulator Validation Checklist

Before cross-compiling, verify:

- [ ] Simulator builds successfully
- [ ] Application starts
- [ ] Correct resolution is used
- [ ] Home screen renders
- [ ] No obvious overlap
- [ ] No major clipping
- [ ] Navigation works
- [ ] Buttons trigger expected callbacks
- [ ] Dynamic labels update
- [ ] Charts update
- [ ] Error states render
- [ ] Offline states render
- [ ] No immediate crash
- [ ] No obvious memory corruption
- [ ] Design closely matches approved `.pen`

If the simulator fails, the normal workflow MUST return to implementation/debugging before board deployment.

---

# 33. Cross Compilation

After simulator validation passes:

```text
native simulator build
        ↓
cross compilation
        ↓
ARM executable
```

The agent MUST use the repository-defined toolchain when available.

Do not replace an existing toolchain casually.

Possible toolchains include:

```text
arm-linux-gnueabihf-gcc
Yocto SDK
Buildroot SDK
vendor SDK
```

The agent MUST distinguish:

```text
host compiler
```

from:

```text
target compiler
```

---

# 34. Board Deployment

Typical deployment may use:

```text
scp
ssh
rsync
NFS
```

The agent SHOULD use existing project scripts if available.

Do not invent a new deployment method when a working deployment workflow already exists.

---

# 35. Board Validation

Board testing MUST validate what the simulator cannot.

Check:

- [ ] LCD output
- [ ] color correctness
- [ ] RGB565 appearance
- [ ] touchscreen input
- [ ] touch coordinate mapping
- [ ] button hit areas
- [ ] scrolling
- [ ] page transitions
- [ ] CPU usage
- [ ] RAM usage
- [ ] UI responsiveness
- [ ] chart performance
- [ ] startup time
- [ ] UART interaction
- [ ] network interaction
- [ ] long-running stability

---

# 36. Performance Validation

The agent SHOULD measure instead of guessing.

Useful Linux tools may include:

```text
top
htop
ps
free
time
pidstat
strace
perf
```

Availability depends on the target root filesystem.

The agent SHOULD look for:

- CPU saturation;
- memory growth;
- blocking syscalls;
- excessive wakeups;
- excessive redraw;
- runaway threads.

---

# 37. Performance Optimization Rule

If the board performs poorly:

```text
Board issue
    ↓
Measure
    ↓
Identify cause
    ↓
Optimize implementation
```

Do NOT immediately degrade the entire design without evidence.

Examples of implementation-level optimization:

- reduce redraw frequency;
- cache formatted strings;
- lower chart refresh rate;
- reduce chart points;
- reduce image size;
- remove unnecessary transparency;
- avoid recreating widgets;
- reuse styles;
- avoid repeated allocation.

---

# 38. When Performance Changes the Design

If a performance optimization changes:

- layout;
- appearance;
- animation;
- spacing;
- visual hierarchy;

then the `.pen` design MUST be updated.

The implementation and design MUST converge again.

---

# 39. Error Handling

Hardware and communication failures are normal.

The GUI SHOULD represent states such as:

```text
Disconnected
Connecting
Connected
Timeout
Error
Unavailable
```

The application MUST NOT assume:

- UART is always present;
- network is always available;
- sensor data is always valid;
- database always succeeds.

GUI error presentation SHOULD be meaningful to the user.

---

# 40. Logging

Business and hardware layers SHOULD provide useful logging.

Example:

```text
[UART] opened /dev/ttymxc2
[DEVICE] start request sent
[NET] disconnected
[DB] failed to open database
```

Avoid flooding logs from high-frequency GUI events.

Logging SHOULD help diagnose board behavior without significantly affecting performance.

---

# 41. Generated Code Policy

If the AI generates LVGL files from `.pen`, generated and manually maintained code SHOULD remain clearly separated.

Example:

```text
src/ui/
    generated visual implementation

src/ui_logic/
    manually maintained behavior
```

The agent SHOULD avoid regenerating files that contain hand-written business logic.

When regeneration is necessary, preserve manual extensions through clear interfaces.

---

# 42. AI Modification Boundaries

The agent MAY autonomously modify:

```text
src/ui/
src/ui_logic/
src/app/
simulator/
tests/
build files
```

when required by the task.

The agent SHOULD be conservative when modifying:

```text
bootloader
kernel
device tree
Buildroot configuration
Yocto layers
production deployment
```

unless the task explicitly requires those areas.

Do not modify unrelated subsystems merely to make a GUI task easier.

---

# 43. AI Must Inspect Before Editing

Before making significant changes, inspect:

- repository structure;
- build system;
- LVGL version;
- existing coding conventions;
- simulator configuration;
- cross-toolchain configuration;
- current screen implementation;
- design files.

Do not assume the project matches this template exactly.

---

# 44. AI Must Build After Changes

After meaningful code changes, the agent SHOULD run the smallest relevant build.

Preferred progression:

```text
affected target
    ↓
simulator
    ↓
full native build
    ↓
cross build
```

Compilation errors SHOULD be fixed before declaring completion.

---

# 45. Do Not Hide Build Failures

If a build fails, report the actual failure.

Do NOT claim:

```text
Build successful
```

unless it actually succeeded.

If an environmental dependency prevents compilation, clearly state the limitation.

---

# 46. Git Workflow

The `.pen` design SHOULD be committed to Git with the source code.

Recommended:

```text
design/hmi.pen
src/ui/
src/ui_logic/
src/app/
```

Commit design and implementation changes together when they belong to the same feature.

Example:

```text
feat(ui): add device status dashboard
```

A commit SHOULD ideally contain:

```text
approved design change
+
LVGL implementation
+
associated logic
```

---

# 47. Do Not Commit Build Artifacts

Unless the repository explicitly requires them, do not commit:

```text
build/
*.o
temporary simulator binaries
core dumps
temporary screenshots
IDE caches
```

Generated production assets MAY be committed when required by the build architecture.

---

# 48. Design Notes

For important UI decisions, maintain:

```text
design/design-notes.md
```

Useful information includes:

- resolution;
- color system;
- spacing system;
- font hierarchy;
- reusable components;
- performance limitations;
- navigation rules.

This provides context to AI agents without requiring visual inference every time.

---

# 49. Recommended Design Constraints for i.MX6ULL

Default design guidance:

```text
Resolution:
800x480

Grid:
8px

Spacing:
8 / 12 / 16 / 24px

Button height:
44-52px

Corner radius:
4-8px

Visual style:
mostly flat

Animations:
short and limited

Transparency:
minimal

Shadows:
minimal or none
```

These are guidelines, not mandatory values if the approved design specifies otherwise.

---

# 50. Recommended Prompt Context for Design-to-LVGL

When converting `.pen` design to LVGL, use constraints equivalent to:

```text
Target platform:
NXP i.MX6ULL

CPU:
ARM Cortex-A7

GPU:
None

Operating system:
Embedded Linux

GUI:
LVGL 9

Display:
800x480

Typical color format:
RGB565

Implementation requirements:

- use LVGL 9 APIs;
- prefer Flex and Grid;
- reuse styles;
- reuse components;
- minimize dynamic allocation;
- avoid blur;
- avoid expensive shadows;
- minimize alpha blending;
- avoid GPU-specific APIs;
- minimize redraw area;
- avoid unnecessary full-screen animations;
- keep event callbacks lightweight;
- separate UI and business logic;
- keep hardware access outside screen files.
```

---

# 51. Interaction State Requirements

Important controls SHOULD consider:

```text
normal
pressed
disabled
active
error
offline
loading
```

AI agents SHOULD not design only the ideal normal state.

Industrial interfaces must clearly represent abnormal conditions.

---

# 52. Screen Lifecycle

Avoid continuously recreating expensive screens without reason.

The chosen lifecycle strategy SHOULD be explicit.

Possible strategies:

```text
create once and reuse
```

or:

```text
create on entry
destroy on exit
```

Choose based on memory and performance constraints.

Do not mix lifecycle models unpredictably.

---

# 53. Memory Discipline

i.MX6ULL resources are limited.

Avoid:

- unnecessary large global buffers;
- repeated heap allocation in high-frequency paths;
- memory leaks during screen navigation;
- repeated creation without deletion;
- unnecessarily large image buffers.

The agent SHOULD consider memory ownership when creating custom LVGL components.

---

# 54. String Formatting

Avoid repeated expensive formatting when unnecessary.

Prefer bounded APIs such as:

```c
snprintf()
```

Do not use unsafe unbounded string operations.

Sensor values SHOULD be validated before presentation.

---

# 55. Real-Time Data Updates

GUI refresh rate MUST be decoupled from hardware sampling rate.

Example:

```text
sensor:
100 samples/sec

application:
100 updates/sec

GUI:
10 updates/sec
```

This is usually preferable to repainting at sensor frequency.

---

# 56. Hardware Mocking

The simulator SHOULD support mock data when practical.

For example:

```text
SIMULATOR
   ↓
mock temperature
mock pressure
mock device state
```

instead of requiring real hardware for every GUI test.

The same UI data interface SHOULD ideally support:

```text
mock backend
```

and:

```text
real backend
```

---

# 57. Build Configuration

Host simulator and target build SHOULD share as much application/UI source as practical.

Preferred:

```text
                   shared UI
                      │
         ┌────────────┴────────────┐
         │                         │
    simulator backend        Linux target backend
         │                         │
        SDL2                framebuffer / DRM
```

Avoid maintaining two different GUI implementations.

---

# 58. Completion Criteria

A GUI feature is considered complete only when applicable items pass:

## Design

- [ ] requirements understood
- [ ] design created/updated in pen.dev
- [ ] `.pen` stored correctly
- [ ] design reviewed
- [ ] design approved

## Code

- [ ] LVGL 9 code implemented
- [ ] code is componentized
- [ ] no giant UI file
- [ ] styles reused
- [ ] UI/business layers separated
- [ ] callbacks remain lightweight

## Simulator

- [ ] native build succeeds
- [ ] simulator starts
- [ ] screen renders correctly
- [ ] navigation works
- [ ] interactions work
- [ ] representative data displays

## Target

- [ ] cross build succeeds
- [ ] board application starts
- [ ] LCD output works
- [ ] touch works
- [ ] no serious visual mismatch
- [ ] CPU usage acceptable
- [ ] memory usage acceptable
- [ ] UI remains responsive

---

# 59. Agent Response Format

After completing substantial work, the AI SHOULD report:

```text
Changed:
- ...

Validated:
- ...

Not validated:
- ...

Design impact:
- none / .pen updated

Target impact:
- ...

Remaining issues:
- ...
```

Do not produce long generic explanations when a concise engineering summary is sufficient.

---

# 60. Core Principles

All agents MUST follow these principles:

## Principle 1

```text
.pen is the visual source of truth.
```

## Principle 2

```text
Design first.
Implementation second.
```

## Principle 3

```text
LVGL UI and business logic remain separated.
```

## Principle 4

```text
Simulator before board.
```

## Principle 5

```text
Measure performance on the real i.MX6ULL.
```

## Principle 6

```text
Do not optimize for desktop hardware.
```

## Principle 7

```text
Do not allow C implementation and .pen design to silently diverge.
```

## Principle 8

```text
Prefer maintainable components over generated monolithic code.
```

## Principle 9

```text
AI may automate implementation, but hardware validation remains mandatory.
```

## Principle 10

```text
The final product must remain stable, responsive, and maintainable on i.MX6ULL.
```
