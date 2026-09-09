# AGENTS.md — ipcam

本文件为 `/home/acomon/project/ipcam` 的 Agent 开发规范。**所有面向用户的说明、注释补充、提交说明、PR 描述、计划与回复均须使用中文**（代码标识符、命令、路径、协议关键字保持英文原文）。

## 项目概览

基于 **正点原子 ALIENTEK i.MX6ULL Mini** 的 C 守护进程：OV5640 采集 → LCD（`/dev/fb0`）+ 可选 MJPEG HTTP 推流。交叉编译工具链 `arm-linux-gnueabihf-`（Linaro 4.9.x），内核 **4.1.15**，BusyBox **1.29**，glibc BSP。

已确认蜂窝模组：**广和通（Fibocom）L610 Cat.1**（AT + PPP）。现有 peers/文档仍有 Quectel EC20 风格 `ttyUSB*`；做 4G 相关改动时**禁止写死** Quectel 专用路径，应将 AT/PPP 设备与 peer 名参数化。

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
