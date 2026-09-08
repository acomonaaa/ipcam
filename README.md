# ipcam

基于正点原子 ALIENTEK **i.MX6ULL Mini** 开发板的"摄像头采集 → LCD 实时显示 + 网络直播"应用。

## 1. 功能

- OV5640 摄像头 V4L2 采集（默认自动选择 `mx6s-csi`，YUYV 4:2:2）
- LCD 实时显示（YUYV → RGB565 软件转换 → /dev/fb0 双页翻转；不支持时退化为 VSYNC 复制）
- 暗光白光管理（默认自动检测；只使用已暴露的 V4L2 flash 或明确的 camera/flash/white LED class 接口）
- MJPEG-over-HTTP 推流（libjpeg-turbo + multipart/x-mixed-replace）
- HTTP 端点：`/`、`/stream.mjpg`、`/snapshot.jpg`、`/api/status`、`/api/config` (GET/POST)、`/api/version`、`/api/ota` (GET/POST)、`/api/reboot`、`/healthz`
- 网络层可插拔：4G（pppd）/ WiFi（wpa_supplicant）
- argv[0] 多调用二进制：`ipcam`（守护进程）/ `camver`（版本）/ `camctl`（status/reboot/ota/rollback）
- 运行时配置（`/etc/ipcam.conf`，二进制 + CRC 校验，HTTP POST 改写）
- 7 级 BCF2 风格日志（runtime level switching）
- 库版本打印 + SIGSEGV/SIGBUS/SIGILL 崩溃 handler（异步安全，dump 当前配置）
- OTA 升级（无需自有服务器：本地文件 或任意 HTTP URL + SHA256 校验 + 自动 rollback）

## 2. 硬件

- 开发板：ALIENTEK i.MX6ULL Mini（单核 Cortex-A7 @ 996 MHz、512 MB DDR3、2×100M Ethernet）
- 摄像头：OV5640（CSI/DVP 并口、i2c2@0x3c、24 MHz MCLK；模组白光是否可控取决于 BSP 暴露的接口）
- LCD：RGB 并口（默认 7" 1024×600、PWM1 背光）
- 4G 模组（可选）：任意 Quectel EC20/Air720 兼容 USB 串口模组（接 usbotg2）
- WiFi 模组（可选）：RTL8188EUS USB WiFi 模组（接 usbotg2）

## 3. 软件依赖

- Linux 内核 4.1.15（NXP `rel_imx_4.1.15_2.1.0_ga`）+ ALIENTEK 厂商补丁
- 用户态 glibc（ALIENTEK 出厂 BSP 默认）
- BusyBox 1.29.0
- 交叉编译器：`gcc-linaro-4.9.4-2017.01-i686_arm-linux-gnueabihf`
- libjpeg-turbo v2.1.x（可选用 stub 模式跳过）

## 4. 目录结构

```
ipcam/
├── Makefile                  # 顶层单一 Makefile（wildcard 自动收集 src/）
├── README.md
├── config/
│   └── ipcam_config.h        # 编译期默认值
├── scripts/
│   ├── build.sh              # 交叉编译入口
│   └── rootfs/
│       ├── etc/init.d/S90ipcam   # 启动脚本（healthz watchdog + auto rollback）
│       └── etc/ppp/peers/quectel # pppd 拨号配置
├── thirdparts/libjpeg-turbo/ # 外部库（自行交叉编译）
└── src/
    ├── core/                 # LEVEL 1：通用基础（无业务依赖）
    │   ├── ipcam_log.{h,c}            # 7 级日志（BCF2 mo_log 风格）
    │   ├── ipcam_ringbuffer.{h,c}     # 环形缓冲（SPSC + try_append）
    │   ├── ipcam_frame_diag.{h,c}     # 帧四象限指纹与跨层证据
    │   ├── ipcam_param.{h,c}          # 运行时配置持久化（BCF2 mo_param 风格）
    │   ├── ipcam_sys.{h,c}            # 版本 + 崩溃 handler
    │   └── ipcam_ota.{h,c}            # OTA（含内嵌 SHA256）
    ├── services/             # LEVEL 2：业务子模块（依赖 core）
    │   ├── ipcam_capture.{h,c}        # V4L2 采集
    │   ├── ipcam_display.{h,c}        # LCD 显示（YUYV→RGB565）
    │   ├── ipcam_light.{h,c}          # 暗光检测与安全白光控制
    │   ├── ipcam_encode.{h,c}         # MJPEG 编码（libjpeg-turbo）
    │   ├── ipcam_encode_stub.c        # 编码空壳（display-only fallback）
    │   ├── ipcam_stream.{h,c}         # HTTP MJPEG 推流服务
    │   ├── ipcam_net4g.{h,c}          # 4G 拨号（AT + pppd）
    │   ├── ipcam_netwifi.{h,c}        # WiFi 连接（wpa_supplicant）
    │   └── ipcam_cli.{h,c}            # argv[0] 多调用派发
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

### 5.2 编译 ipcam

```sh
./scripts/build.sh                          # 自动探测工具链 + 检查 libturbojpeg
# 或者
CROSS_COMPILE=arm-linux-gnueabihf- DEBUG=1 ./scripts/build.sh
```

产物：项目根目录 `./ipcam`（ELF32 ARM EABI5）。

### 5.3 仅采集 + LCD（无 libjpeg-turbo 时）

```sh
make ipcam-display-only
# 或者
SKIP_TJ_CHECK=1 ./scripts/build.sh ipcam-display-only
```

`src/services/ipcam_encode_stub.c` 提供空壳 `ipcam_encode_start/stop`，让 daemon 链接通过但无 MJPEG 流。

### 5.4 编译选项

```sh
make DEBUG=1                       # -O0 -g3 调试构建
make CROSS_COMPILE=arm-linux-gnueabihf-   # 显式工具链
make CROSS_COMPILE=                # 用本机 gcc（仅用于主机端开发自测）
make clean                        # 清理所有产物
make install                      # 安装到项目内 ./output/
```

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

使用 NFS rootfs 调试时，除同步 ARM 版 `ipcam` 和 `S90ipcam` 外，还必须确认
`/etc/init.d/rcS` 在 `modprobe mx6s_capture`、`modprobe ov5640_camera` 后调用
`/etc/init.d/S90ipcam start`。当前正点原子 i.MX6ULL 调试 rootfs 已按此方式配置，
因此每次 `run mybootnet` 进入 Linux 后会自动加载摄像头并启动应用。

### 6.2 启动

- 开发板上电后登录串口 shell（115200 8N1）
- 内核启动脚本需加载 i.MX6ULL CSI 链路模块：`mx6s_capture`、`ov5640_camera`；不要加载 i.MX6Q IPU 旧模块 `mxc_v4l2_capture`、`ov5640_camera_int`
- 执行 U-Boot 的 `run mybootnet` 后，Linux `rcS` 会自动加载 CSI 模块并启动 `ipcam`，无需再次手工执行启动命令
- 手动重启或排查时可执行：`/etc/init.d/S90ipcam restart`
- 查看版本：`camver`
- 查看状态：`camctl status`
- `run mybootnet` 后应用日志会自动从串口控制台输出，无需手工执行 `tail -f`；若串口不可用，启动脚本会将应用输出回退到 `/var/log/ipcam.log`
- 查看历史/回退日志：`tail -f /var/log/ipcam.log`（主要包含启动脚本诊断）
- 浏览器预览：`http://<board_ip>:8080/`（默认绑定 `0.0.0.0`）

### 6.3 日志与媒体链路运行统计

日志格式沿用 BCF2 `mo_log` 的核心习惯：时间戳、级别、模块、源文件/行号和线程号，
例如 `INF [CAP ] ... (tid=...)`。媒体链路使用固定模块标签，便于串口或日志文件中
快速筛选：`MAIN`（主流程）、`CAP `（V4L2/CSI 采集）、`DISP`（LCD 显示）、
`LIGHT`（白光状态）、`ENC `（JPEG 编码）和 `HTTP`（推流会话）。

采集、显示和编码线程启动后会各打印一次 `first frame`；正常运行期间每 5 秒打印一次
统计，包含区间帧率、累计帧数、环形缓冲深度以及采集丢帧、编码跳帧、白光后端状态或 JPEG 平均大小。
这样可以区分“设备没有出帧”“LCD 消费跟不上”“编码耗时过高”和“网络客户端未连接”。
HTTP 客户端还会记录流会话的首个 JPEG 和会话帧率。

日志级别可通过启动环境变量 `IPCAM_LOG_LEVEL=5` 打开 DEBUG，也可通过
`POST /api/config` 设置 `log_level`。启动脚本不会使用 BusyBox
`start-stop-daemon -b`，因为该选项会把后台进程的标准输出和错误输出丢到
`/dev/null`；现在保留 `-m` 管理真实 PID，由 shell 将应用的 stdout/stderr 直接接到
`/dev/console`，所以 `run mybootnet` 后串口会自动持续显示 `MAIN/CAP/DISP/LIGHT/ENC` 日志。
仅当 `/dev/console` 不可用时才回退到 `/var/log/ipcam.log`；启动脚本自己的诊断仍同时
写串口和该文件。

LCD 显示优先申请 `yres_virtual >= 2*yres`，把完整 RGB565 帧写入后台页后用
`FBIOPAN_DISPLAY` 在帧边界翻页，避免 LCD 扫描到半帧造成撕裂、绿屏和左右错位。
若 framebuffer 不支持双页，则使用完整临时帧并尝试 `FBIO_WAITFORVSYNC` 后一次复制，
启动日志会明确显示退化模式及 VSYNC 不可用警告。

为定位长时间运行后的半幅错位，`CAP` 会记录 V4L2 sequence、DMA buffer、时间戳、
`bytesperline`、`bytesused`、亮度和四象限 `probe`；同一帧元数据随 payload 进入 display
和 encode ring。`DISP` 会重新计算 ring 指纹，再记录 RGB565 目标页指纹、页地址、请求/实际
`yoffset` 和翻页耗时。每 5 秒的 `diag={...}` 可按下面的顺序判断问题层次：

- `source_probe != ring_probe` 或 `ring_mismatch` 增加：优先检查 ring 槽位复用、内存覆盖或消费者越界。
- source/ring 一致但 `rgb_probe` 异常：优先检查 YUYV stride、转换和目标页写入。
- `fb_page_corrupt` 增加：后台页在复用前被 LCD 控制器、console 或其它线程改写。
- `fb_layout`、`bad_yoffset`、`pan_error`、`pan_timeout` 增加：检查 framebuffer 参数、翻页地址和 `dmesg` 中的 mxsfb underflow/overflow。
- 各项都为 0 但画面仍错位：保留 CAP/DISP 日志并结合内核 `dmesg`、中断和 LCD 控制器状态继续定位；本轮不修改内核。

默认只打印首帧和每 5 秒汇总。现场需要逐帧关联时设置 `IPCAM_LOG_LEVEL=5`，此时会额外
看到采样指纹、每次 `pan` 的目标页/地址/实际偏移/耗时以及每个 V4L2 DMA buffer 的使用次数。
若希望 `run mybootnet` 后直接进入该详细模式，可在 NFS
`/etc/init.d/rcS` 调用 `/etc/init.d/S90ipcam start` 前加入
`export IPCAM_LOG_LEVEL=5`；仅临时重启应用时可执行
`IPCAM_LOG_LEVEL=5 /etc/init.d/S90ipcam restart`。默认 INFO 模式无需任何额外命令。
采集约 30 fps 而 LCD 约 16.5 fps 时，`drop_disp` 持续增加是实时 ring 满导致的正常丢帧，
不等同于画面损坏；display-only 构建的 `drop_enc` 也会因编码空壳不消费而增加。

### 6.4 暗光白光控制

白光状态机默认处于 `auto`：从 YUYV 8×8 均匀采样点计算平均 Y 值，平均亮度不高于 35
连续 15 帧请求开启，高于 55 连续 30 帧请求关闭，并且两次状态变化至少间隔 3 秒。
状态变化会输出 `LIGHT` 日志，包括平均亮度、阈值、持续帧数、后端和执行结果。

- `IPCAM_WHITE_LIGHT=off`：禁用自动控制，并在已发现安全后端时先请求关灯。
- `IPCAM_WHITE_LIGHT=auto`：显式使用默认自动模式。
- `IPCAM_WHITE_LIGHT_SYSFS=/sys/class/leds/<camera|flash|white>/brightness`：多个安全 LED 候选时明确指定；程序拒绝 `sys-led`、`led0` 和非 LED class 路径。

程序先查询 V4L2 `V4L2_CID_FLASH_LED_MODE`，再查找名称明确包含 camera/flash/white 的
LED class。当前出厂 `ov5640_camera` 若没有暴露这些接口，会打印
`white light control unavailable` 并保持关闭；不会直接操作 `/dev/i2c-*`，也不会把开发板
状态灯当作摄像头白光。

display-only 版本没有真实 JPEG 编码器和 HTTP 服务，日志中应看到
`CAP`、`DISP` 以及 `ENC stub active`；只有链接 libjpeg-turbo 的完整版本在有 HTTP
客户端连接时才会出现 JPEG 编码和 `HTTP` 会话日志。

## 7. 验证清单

| 里程碑 | 验证手段 | 通过标准 |
|---|---|---|
| 编译通过 | `make` 无 error | 产出 `./ipcam` ELF32 ARM EABI5 |
| 自动启动 | `run mybootnet` 后查看进程和日志 | `rcS` 自动加载 CSI 模块并启动 `ipcam`，无需手工输入启动命令 |
| 启动日志 | `run mybootnet` 后观察串口 | 日志自动出现 `MAIN/CAP/DISP/LIGHT/ENC` 启动信息，并显示自动选中的 `mx6s-csi` 节点，无需手工 `tail -f` |
| 运行日志 | 继续观察串口 | 看到 `CAP/DISP/LIGHT/ENC` 各自的启动/首帧信息，随后每约 5 秒出现 `stats`；日志包含 framebuffer 模式和异常帧计数 |
| LCD 显示 | 插入 OV5640 到 CSI 插座 | LCD 上看到实时画面（双页翻转或 VSYNC 退化模式，无明显撕裂/绿屏/左右错位）|
| 错位诊断 | `IPCAM_LOG_LEVEL=5` 后运行至少 30 分钟 | 能关联 source/ring/RGB565/page/yoffset；异常计数增加时按日志层次定位 |
| 白光控制 | 暗光环境运行，观察 `LIGHT` | 有安全控制节点时按滞回阈值切换；无节点时明确 unavailable 且不误点亮 sys-led |
| 局域网推流 | 浏览器访问 `http://<board_ip>:8080/` | 看到实时 MJPEG 流，延迟 < 1 秒 |
| 4G 推流 | 接 4G 模组到 usbotg2；`curl -d 'net_mode=1' http://127.0.0.1:8080/api/config` 后重启 daemon，浏览器通过 4G IP 访问 | 同上，但出网走 ppp0 |
| API 状态 | `curl http://<board_ip>:8080/api/status` | 返回 JSON：model/swver/capture/ring_count 等 |
| 配置修改 | `curl -X POST -d 'jpeg_quality=80' http://<board_ip>:8080/api/config` | 立即生效 |
| 拍照 | `curl -o snap.jpg http://<board_ip>:8080/snapshot.jpg` | 下载得到单张 JPEG |
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

http://<board_ip>:8080/                   # 浏览器实时预览
http://<board_ip>:8080/snapshot.jpg      # 拍照
http://<board_ip>:8080/stream.mjpg        # 流媒体
```

## 9. 借鉴的设计思想（参考 `mc-lib + mc-mid + mc-app` 分层）

| BCF2 模块 | ipcam 对应 | 借鉴点 |
|---|---|---|
| `libmo/log` | `src/core/ipcam_log.{h,c}` | 7 级日志 + runtime 切换 + 模块名 |
| `libmo/param` | `src/core/ipcam_param.{h,c}` | 二进制 + magic + CRC + getter/setter |
| `libmo/ringbuffer` | `src/core/ipcam_ringbuffer.{h,c}` | SPSC + CLOCK_MONOTONIC |
| 帧诊断 | `src/core/ipcam_frame_diag.{h,c}` | 四象限采样指纹，关联采集/ring/显示页 |
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

## 10. 采集像素格式选型（架构决策）

**最终方案：单路 packed 4:2:2（默认 YUYV，保留 UYVY 编译期兼容选项），不采用官方例程的 RGB565 直采路径。**

| 对比项 | 单路 YUV422（采用） | 单路 RGB565（官方例程） |
|---|---|---|
| 编码路径（产品主功能） | 直通 TurboJPEG YUV 接口，近零转换 | libjpeg 内部做 RGB→YCbCr，转换落在每个推流消费者 |
| 本地预览路径 | 软件转 RGB565（次要路径，可查表/NEON 优化或降帧） | 按行 memcpy（最优） |
| 后续 H.264 | i.MX6ULL 无 VPU，软编 x264 同样只吃 YUV，管线无需变更 | RGB→YUV420 又是一层转换 |

决策依据：产品主功能是网络推流，格式转换成本应落在次要路径（本地预览）上；
编码器生态（JPEG/MJPEG/软编 H.264）全部位于 YUV 侧；单传感器同一时刻只能输出
一种格式，"LCD 用 RGB、编码用 YUV"的双格式并行在单摄下不成立。

字节序配置：[`config/ipcam_config.h`](config/ipcam_config.h) 的 `IPCAM_CAP_PIXFMT`
（0 = YUYV，1 = UYVY）。正点原子出厂 `ov5640_camera` 驱动明确支持 YUYV；当前
i.MX6ULL 出厂链路默认使用 YUYV，UYVY 选项只保留给其它 sensor/BSP。程序会在
`VIDIOC_S_FMT` 后通过 `VIDIOC_G_FMT` 验证实际格式，协商不一致时拒绝启动并打印
期望/实际四字符码。正点原子 4.1.15 出厂 `mx6s-csi` 驱动存在一个兼容性缺陷：
`G_FMT` 可能把 `pixelformat` 返回为 0，但 `S_FMT` 已回显请求的 YUYV 且驱动内部
已经按 YUYV 配置 CSI；程序只对已识别的 `mx6s-csi` 节点兼容这一情况，尺寸和
`bytesperline`、`sizeimage` 仍必须严格匹配：当前官方链路要求
`bytesperline == width*2`、`sizeimage == bytesperline*height`，并且每个 V4L2 帧的
`bytesused == sizeimage`。不完整帧会在进入环形缓冲前丢弃，避免显示半帧造成绿屏或
逐行错位。其它设备返回 0 或其它格式仍会拒绝启动。

设备节点配置：[`config/ipcam_config.h`](config/ipcam_config.h) 的
`IPCAM_VIDEO_DEV` 默认为空，程序扫描 `/dev/videoN`，只选择名称/驱动信息匹配
`mx6s-csi` 且同时具备 video-capture、streaming capability 的节点。设置为具体路径
时可显式覆盖自动发现，但仍会执行 capability 和格式检查；不能根据 `video0`/`video1`
编号推断摄像头节点。

官方 RGB565 例程（`ZDYZexample/.../25_v4l2_camera`）的定位是 **bring-up 硬件验证基线**：
若板上 RGB565 通而 YUV 不通，先用官方例程确认 sensor 节点/时钟/LCD 通路，
再回到本方案排查驱动格式配置，而不是推翻选型。

### 10.1 输出分辨率上采样（满足客户分辨率规格）

传感器有效分辨率由硬件决定、不可更改；当客户/产品规格要求更高的**输出**分辨率
时，编码前在 planar 域做插值缩放满足规格（实际画质不变，这是行业常规做法，
同数码变焦/超分输出）。

- 配置：`out_w`/`out_h`（编译期默认 [`config/ipcam_config.h`](config/ipcam_config.h)，
  运行时 `/api/config` 可改，**重启生效**）；`0 = 跟随采集分辨率（旁路缩放）`
- 算法：Y 平面双线性（Q16 定点），色度平面最临近（4:2:2 色度为低频半分辨率信号）
- 位置：编码线程内、`unpack → planar` 之后、`tjCompressFromYUVPlanes` 之前；
  显示链路不受影响（LCD 本来就按全屏插值）
- 限制：`out_w` 必须为偶数（4:2:2 色度半宽取整）；输出越大编码 CPU 越贵，
  i.MX6ULL 单核建议 720p@15fps 起步实测
- 口径：对外表述用「输出分辨率」，不要写成「有效像素/采集分辨率」
