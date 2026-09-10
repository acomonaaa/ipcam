# ipcam

基于正点原子 ALIENTEK **i.MX6ULL Mini** 开发板的"摄像头采集 → LCD 实时显示 + 网络直播"应用。

## 1. 功能

- OV5640 摄像头 V4L2 采集（YUYV 4:2:2）
- LCD 实时显示（YUYV → RGB565 软件转换 → LVGL 9 局部合成 → /dev/fb0 mmap）
- MJPEG-over-HTTP 推流（libjpeg-turbo + multipart/x-mixed-replace）
- HTTP 端点：`/`、`/stream.mjpg`、`/snapshot.jpg`、`/api/status`（含 target/capture/output fps）、`/api/capabilities`、`/api/control`、`/api/control/result`、`/api/record`、`/api/photo`、`/api/config` (GET/POST)、`/api/version`、`/api/ota` (GET/POST)、`/api/reboot`、`/healthz`
- 网络层可插拔：4G（pppd）/ WiFi（wpa_supplicant）
- argv[0] 多调用二进制：`ipcam`（守护进程）/ `camver`（版本）/ `camctl`（状态、控制、录像、拍照、reboot、OTA）
- 运行时配置（`/etc/ipcam.conf`，二进制 + CRC 校验，HTTP POST 改写）
- 7 级 BCF2 风格日志（runtime level switching）
- 固件侧独立拍照与 MJPEG AVI 录像（5 分钟分段、SD 卡挂载校验、缺帧重复统计）
- 本地预览开关、1×～4×视口、水平/垂直翻转、熄屏唤醒和 Linux evdev 多点触摸契约
- 板级补光临时控制（只接受显式配置的亮度节点，开机默认关闭）
- 统一控制队列：GUI、HTTP 与 `camctl` 共用能力、状态、命令和结果接口
- 库版本打印 + SIGSEGV/SIGBUS/SIGILL 崩溃 handler（异步安全，dump 当前配置）
- OTA 升级（无需自有服务器：本地文件 或任意 HTTP URL + SHA256 校验 + 自动 rollback）

## 2. 硬件

- 开发板：ALIENTEK i.MX6ULL Mini（单核 Cortex-A7 @ 996 MHz、512 MB DDR3、2×100M Ethernet）
- 摄像头：OV5640（CSI/DVP 并口、i2c2@0x3c、24 MHz MCLK）
- LCD：RGB 并口（当前板实测 /dev/fb0 为 800×480、RGB565、PWM1 背光）
- 4G 模组（可选）：任意 Quectel EC20/Air720 兼容 USB 串口模组（接 usbotg2）
- WiFi 模组（可选）：RTL8188EUS USB WiFi 模组（接 usbotg2）

## 3. 软件依赖

- Linux 内核 4.1.15（NXP `rel_imx_4.1.15_2.1.0_ga`）+ ALIENTEK 厂商补丁
- 用户态 glibc（ALIENTEK 出厂 BSP 默认）
- BusyBox 1.29.0
- 交叉编译器：`gcc-linaro-4.9.4-2017.01-i686_arm-linux-gnueabihf`
- libjpeg-turbo v2.1.x（可选用 stub 模式跳过）
- LVGL v9.5.0（源码依赖，使用项目根目录的 `lv_conf.h` 配置）

## 4. 目录结构

```
ipcam/
├── Makefile                  # 顶层单一 Makefile（wildcard 自动收集 src/）
├── README.md
├── design/
│   ├── IPCam_LCD_UI_sentry.pen       # 最新 800×480 GUI 设计源文件
│   └── LVGL_MAPPING.md               # 设计页面与 LVGL 实现映射
├── config/
│   └── ipcam_config.h        # 编译期默认值
├── lv_conf.h                 # LVGL 9 的 i.MX6ULL 最小配置
├── scripts/
│   ├── build.sh              # 交叉编译入口
│   └── rootfs/
│       ├── etc/init.d/S90ipcam   # 启动脚本（healthz watchdog + auto rollback）
│       └── etc/ppp/peers/quectel # pppd 拨号配置
├── thirdparts/libjpeg-turbo/ # 外部库（自行交叉编译）
├── thirdparts/lvgl/          # LVGL v9.5.0 外部源码依赖说明
└── src/
    ├── core/                 # LEVEL 1：通用基础（无业务依赖）
    │   ├── ipcam_log.{h,c}            # 7 级日志（BCF2 mo_log 风格）
    │   ├── ipcam_ringbuffer.{h,c}     # 环形缓冲（SPSC + try_append）
    │   ├── ipcam_param.{h,c}          # 运行时配置持久化（BCF2 mo_param 风格）
    │   ├── ipcam_sys.{h,c}            # 版本 + 崩溃 handler
    │   └── ipcam_ota.{h,c}            # OTA（含内嵌 SHA256）
    ├── services/             # LEVEL 2：业务子模块（依赖 core）
    │   ├── ipcam_capture.{h,c}        # V4L2 采集
    │   ├── ipcam_display.{h,c}        # LCD 预览帧生产与 framebuffer 映射
    │   ├── ipcam_lvgl.{h,c}           # LVGL 9 framebuffer/evdev 适配与 UI 服务
    │   ├── ipcam_encode.{h,c}         # MJPEG 编码（libjpeg-turbo）
    │   ├── ipcam_encode_stub.c        # 编码空壳（display-only fallback）
    │   ├── ipcam_record.{h,c}         # SD 卡照片与 MJPEG AVI 分段录像
    │   ├── ipcam_control.{h,c}        # GUI/HTTP/camctl 统一控制契约
    │   ├── ipcam_screen.{h,c}         # 自动熄屏、背光和唤醒状态机
    │   ├── ipcam_touch.{h,c}          # Linux evdev 多点触摸输入适配
    │   ├── ipcam_light.{h,c}          # 板级补光节点适配
    │   ├── ipcam_stream.{h,c}         # HTTP MJPEG 推流服务
    │   ├── ipcam_net4g.{h,c}          # 4G 拨号（AT + pppd）
    │   ├── ipcam_netwifi.{h,c}        # WiFi 连接（wpa_supplicant）
    │   └── ipcam_cli.{h,c}            # argv[0] 多调用派发
    ├── ui/                   # LVGL 9 的 P01～P08 固定坐标界面与组件
    │   ├── ipcam_ui.{h,c}              # 页面生命周期、状态同步和动作分发
    │   ├── ipcam_ui_components.{h,c}   # 与 .pen token 对齐的通用控件
    │   └── ipcam_ui_screen_*.c         # 八个设计页面的具体布局
    └── main.c                # LEVEL 3：应用入口（编排所有子系统）
```

**为什么这种分层**：

- `core/` 是通用工具，理论上可被其他 C 项目复用
- `services/` 是 ipcam 专属业务逻辑（V4L2/LCD/JPEG/HTTP/PPP 是平台特定的）
- `src/main.c` 编排所有 `services` 启动顺序
- 依赖方向严格：**core ← services ← main**，core 不引用 services，services 不引用 main

## 5. 编译

### 5.1 准备交叉编译 libjpeg-turbo

```sh
cd thirdparts/libjpeg-turbo
git clone --depth 1 -b 2.1.x https://github.com/libjpeg-turbo/libjpeg-turbo.git src
cd src
mkdir -p ../install
export CC=arm-linux-gnueabihf-gcc
export AR=arm-linux-gnueabihf-ar
cmake -DCMAKE_INSTALL_PREFIX=$(pwd)/../install \
      -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
      -DWITH_JAVA=OFF -DWITH_SIMD=OFF \
      ..
make -j$(nproc)
make install
```

### 5.2 准备 LVGL 9

LVGL 源码不复制进本仓库，构建时由 Makefile 直接递归编译；固定使用
`v9.5.0`，避免板端适配随上游分支漂移：

```sh
git clone --branch v9.5.0 --depth 1 \
  https://github.com/lvgl/lvgl.git thirdparts/lvgl/src
```

`lv_conf.h` 已关闭 LVGL 自带的 fbdev/evdev 后端，应用复用现有
`ipcam_display` 的 `/dev/fb0` 映射，并由 `ipcam_touch` 的唯一 evdev 线程
把触点快照交给 LVGL。最新 `.pen` 已按 800×480 画布同步，P01～P08
分别落到 `src/ui/ipcam_ui_screen_*.c`，P01/P03 的视频区域直接绑定
`ipcam_display` 输出的 RGB565 帧，按钮动作通过队列交给统一控制层。

`lv_conf.h` 保留 `LV_FONT_DEFAULT`（Mont14）作为图标和回退字体；当前 UI 文案所需
的中文已经裁剪为 `src/ui/ipcam_ui_font_cjk_14.c`、`16.c`、`20.c`，并在
`ipcam_lvgl_start()` 中通过 `ipcam_ui_fonts_t` 注入。若后续页面增加中文，应在
Ubuntu 上运行 `bash scripts/generate_ui_fonts.sh` 重新生成字库，再交叉编译部署。
字体注入接口和页面映射详见 [`design/LVGL_MAPPING.md`](design/LVGL_MAPPING.md)。
页面坐标按设计源文件手工维护，后续 `.pen` 发生布局变化时应同步更新映射。

### 5.3 编译 ipcam

```sh
./scripts/build.sh                          # 自动探测工具链 + 检查 libturbojpeg
# 或者
CROSS_COMPILE=arm-linux-gnueabihf- DEBUG=1 ./scripts/build.sh
```

产物：项目根目录 `./ipcam`（ELF32 ARM EABI5）。

### 5.4 仅采集 + LCD（无 libjpeg-turbo 时）

```sh
make ipcam-display-only
# 或者
SKIP_TJ_CHECK=1 ./scripts/build.sh ipcam-display-only
```

`src/services/ipcam_encode_stub.c` 提供空壳 `ipcam_encode_start(_ex)/stop`，让 daemon 链接通过但无 MJPEG 流。

### 5.5 编译选项

```sh
make DEBUG=1                       # -O0 -g3 调试构建
make CROSS_COMPILE=arm-linux-gnueabihf-   # 显式工具链
make CROSS_COMPILE=                # 用本机 gcc（仅用于主机端开发自测）
make test-host                     # 编译并运行主机侧 ring/媒体生命周期测试
make -B test-host SANITIZE=1       # 使用 ASan/UBSan 强制重编并运行测试
make clean                        # 清理所有产物
make install                      # 安装到项目内 ./output/
```

LVGL 默认接管 LCD 的 UI 合成；首次部署遇到 UI 初始化问题时可设置
`IPCAM_LVGL=0` 回退到旧的 display 线程直接写屏。无论哪种模式，
`IPCAM_FB_DEV` 仍指定 framebuffer，触摸节点由 `IPCAM_TOUCH_DEV` 指定，
本板默认值分别为 `/dev/fb0` 和 `/dev/input/event1`。

## 6. 部署到开发板

### 6.1 准备 rootfs

```sh
mount /dev/sdX1 /mnt/rootfs
install -m 755 ./ipcam /mnt/rootfs/usr/bin/
ln -sf ipcam /mnt/rootfs/usr/bin/camver
ln -sf ipcam /mnt/rootfs/usr/bin/camctl
install -m 755 scripts/rootfs/etc/init.d/S90ipcam /mnt/rootfs/etc/init.d/
install -m 644 scripts/rootfs/etc/ppp/peers/quectel /mnt/rootfs/etc/ppp/peers/
mkdir -p /mnt/rootfs/var/log
umount /mnt/rootfs
```

OTA 看门狗（`S90ipcam`）依赖 BusyBox **`wget`** 探测 `http://127.0.0.1:8080/healthz`；成功后会删除 `ipcam.prev`。

### 6.2 启动

- 开发板上电后登录串口 shell（115200 8N1）
- 手动启动：`/etc/init.d/S90ipcam start`
- 查看版本：`camver`
- 查看状态：`camctl status`
- 查看日志：`tail -f /tmp/ipcam/ipcam.log`（可用 `IPCAM_LOG_FILE` 覆盖）
- 浏览器预览：`http://<board_ip>:8080/`（默认绑定 `0.0.0.0`）

板级探测后可通过环境变量绑定实际节点和挂载点：`IPCAM_VIDEO_DEV`、`IPCAM_FB_DEV`、
`IPCAM_TOUCH_DEV`、`IPCAM_LVGL`、`IPCAM_BACKLIGHT_PATH`、`IPCAM_LIGHT_PATH`、
`IPCAM_STORAGE_ROOT`。本板的 `/proc/bus/input/devices` 已确认 `event0` 是
电源键、`event1` 是 `goodix-ts`，因此不能把触摸默认写成 `event0`。
应用不会猜测 GPIO 编号，也不会在 SD 未挂载时把照片写入根文件系统。

## 7. 验证清单

| 里程碑 | 验证手段 | 通过标准 |
|---|---|---|
| 编译通过 | `make` 无 error | 产出 `./ipcam` ELF32 ARM EABI5 |
| 启动 | `/etc/init.d/S90ipcam start` | 日志显示 capture/display/LVGL/stream 线程 start |
| LCD 显示 | 插入 OV5640 到 CSI 插座 | 800×480 LCD 显示 P01 本地预览首页，P01/P03 视频区域出现实时画面 |
| LCD 触摸 | 点击 P01～P08 的按钮、单选项和开关 | 页面切换和控制动作生效，日志显示 `touch started: /dev/input/event1` |
| 局域网推流 | 浏览器访问 `http://<board_ip>:8080/` | 看到实时 MJPEG 流，延迟 < 1 秒 |
| 4G 推流 | 接 4G 模组到 usbotg2；可用 `IPCAM_4G_AT_DEV` 和 `IPCAM_4G_PPP_PEER` 覆盖 AT 设备与 PPP profile，再通过 `curl -d 'net_mode=1' http://127.0.0.1:8080/api/config` 重启 daemon | 同上，但出网走 ppp0 |
| API 状态 | `curl http://<board_ip>:8080/api/status` | 返回 JSON：model/swver/capture/ring_count 等 |
| 配置修改 | `curl -X POST -d 'jpeg_quality=80' http://<board_ip>:8080/api/config` | 立即生效 |
| 拍照 | `curl -X POST http://<board_ip>:8080/api/photo` | 返回 SD 卡正式文件路径；`/snapshot.jpg` 仍是在线快照 |
| 录像 | `curl -X POST -d action=start http://<board_ip>:8080/api/record` | 返回请求编号，查询 `/api/record` 状态 |
| 控制 | `curl -X POST -d 'command=light&percent=50' http://<board_ip>:8080/api/control` | 统一控制队列设置临时补光，能力不可用时返回失败 |
| OTA 升级 | `camctl ota /mnt/sd/ipcam-new <sha256> && camctl ota commit` | 重启后自动应用新版本 |

## 8. 关键命令速查

```sh
camver                                    # 版本
camctl status                             # 状态
camctl reboot                             # 重启（需 root）
camctl ota <file> [sha256]                # 本地文件 OTA
camctl ota url <url> [sha256]             # HTTP URL OTA
camctl ota commit                         # 切换 + reboot
camctl ota rollback                       # 回滚
camctl capabilities                       # 已验证能力
camctl record start|stop                  # 手动录像
camctl photo                              # 独立拍照
camctl control preview <0|1>              # 本地预览开关
camctl control view <0|1> <zoom> <cx> <cy> # 本地视口
camctl control mirror <0|1> <0|1>         # 水平/垂直翻转
camctl control light <0..100>             # 临时补光

http://<board_ip>:8080/                   # 浏览器实时预览
http://<board_ip>:8080/snapshot.jpg      # 拍照
http://<board_ip>:8080/stream.mjpg        # 流媒体
```

固件首版只对外公布 `640×480、目标 15 fps` 档位；JPEG 质量可在运行时调整。
分辨率和帧率的其它档位必须完成实机压力测试后再加入能力清单。

## 9. 借鉴的设计思想（参考 `mc-lib + mc-mid + mc-app` 分层）

| BCF2 模块 | ipcam 对应 | 借鉴点 |
|---|---|---|
| `libmo/log` | `src/core/ipcam_log.{h,c}` | 7 级日志 + runtime 切换 + 模块名 |
| `libmo/param` | `src/core/ipcam_param.{h,c}` | 二进制 + magic + CRC + getter/setter |
| `libmo/ringbuffer` | `src/core/ipcam_ringbuffer.{h,c}` | SPSC + CLOCK_MONOTONIC |
| `libmo/sys` | `src/core/ipcam_sys.{h,c}` | 崩溃 handler（异步安全 + snapshot）|
| `libmo/ota` | `src/core/ipcam_ota.{h,c}` | SHA256 + ELF + atomic + rollback |
| `mo_cli_tool` | `src/services/ipcam_cli.{h,c}` | argv[0] dispatch + uid==0 |
| `mo_sys` 启动顺序 | `src/main.c` | sys_init → crash handler → banner → lib_versions → param_init → 子系统 |
| `mo_log.h` 7 级 enum | 同样 7 级 enum | 完整对齐 |

**未借鉴**（BCF2 历史包袱，不适合 greenfield）：
- 27 个独立 libmo 目录 + 每个独立 Makefile（14 个文件不需要）
- 多 binary 产物（我们是 single-binary daemon）
- MTD 分区持久化（ipcam 用普通文件即可）
- 跨型号 param.cfg 配置（（单一型号）
