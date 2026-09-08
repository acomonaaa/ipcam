# 排查记录：摄像头 `/dev/video1` 未注册

日期：2026-09-07（UTC+8）
环境：ALIENTEK i.MX6ULL EMMC 14x14（4.3 寸 800x480），内核 4.1.15-ge48931b1，NFS rootfs（busybox 1.29），ipcam 显示版（stub 编码）

---

## 问题现象

板端执行 `/usr/bin/ipcam` 报：

```
ERR [ipcam-imx6ull] ipcam_capture.c:121 /dev/video0 lacks video-capture or streaming capability
ERR [ipcam-imx6ull] main.c:153 capture start failed
```

板端 `ls /dev/video*` 仅有 `/dev/video0`，无摄像头采集节点。

---

## 历史排查链条（旧方案，已废弃）

> 本节保留第一次现场排查证据，但其中把问题归因于 `v4l2_cap_0`/DT 属性并据此
> 修改旧 IPU 驱动链路的结论已经被官方 i.MX6ULL 出厂源码推翻，不得再按本节方案
> 加载模块或修改 DTS。

### 1. `/dev/video0` 不是摄像头

- 日志依据：内核启动打印 `[3.084414] pxp-v4l2 pxp_v4l2: initialized`；板端 `cat /sys/class/video4linux/video0/name` 输出 `PxP`。
- 判断：video0 是 PxP 图像加速器（mem2mem，无 V4L2_CAP_VIDEO_CAPTURE），ipcam 的 capability 检查行为正确，是设备节点不对。

### 2. 摄像头驱动是模块（.ko），新 rootfs 中缺失

- 日志依据：完整启动日志中无任何 `ov5640` / `MXC Video for Linux` 相关行；`modules.tar.bz2` 内含 `mxc_v4l2_capture.ko`、`ov5640_camera_int.ko` 等共 82 个 `.ko`。
- 判断：CSI 采集与传感器驱动均为模块，busybox rootfs 未安装模块 → 驱动从未加载。
- 处置：解压 `modules.tar.bz2` 至 rootfs `/lib/modules/4.1.15-ge48931b1/`（注意包顶层无 `modules/` 一层，直接解到 `/lib` 会报 `modprobe: can't change directory to '/lib/modules'`）。已写入 `rcS` 开机自动加载。

### 3. dtb 与 BSP 驱动不配对（两处差异）

- 日志依据（修复前）：`modprobe mxc_v4l2_capture` + `modprobe ov5640_camera_int` 均静默成功，但 dmesg 无任何探测日志、`/sys/bus/i2c/devices/` 存在 `1-003c`。
- 二进制证据：
  - dtb 字符串：`ov5640@3c` + `compatible = "ovti,ov5640"`；
  - `modinfo ov5640_camera_int` → `alias: i2c:ov564x`；`strings ov5640_camera_int.ko` 显示其 I2C ID 表名为 `ov564x`，且**无 of_match_table**（客户端名取 compatible 后半段，`ov5640 ≠ ov564x`，永不匹配）；
  - `strings mxc_v4l2_capture.ko` → of_match 仅 `fsl,imx6q-v4l2-capture`，对应 NXP BSP 的 `v4l2_cap_0` **虚拟节点**；反编译 dtb 确认其中**不存在**该节点。
- 判断：现用 dtb 与 zImage/模块非同源（另一套 BSP 写法），传感器驱动匹配不上、CSI 主设备无处绑定。
- 处置（已生效）：`dtc` 反编译 → `compatible` 改 `ovti,ov564x` → 根节点补 `v4l2_cap_0`（`fsl,imx6q-v4l2-capture` + `ipu_id/csi_id/mclk_source/is_little_endian`）→ 重编译替换 tftpboot dtb。修复后：`camera ov5640 is found`、`/sys/bus/platform/drivers/mxc_v4l2_capture/` 下 `v4l2_cap_0` 绑定成功。

### 4. 【当前卡点】mxc_v4l2_probe 内核 oops

- 日志依据（模块重载后抓到完整现场）：

```
[    8.830695] [<7f01c4f0>] (mxc_v4l2_probe [mxc_v4l2_capture]) from [<80388980>] (platform_drv_probe+0x44/0xac)
[    8.933552] Code: e3473f01 e5853174 e5943944 e3477f01 (e5834034)
[    8.940270] ---[ end trace 54a51f5f76dd525e ]---
```

- 判断：`Code:` 段末尾加括号指令 `str r4, [r3, #52]` 为野指针写。结合源码（NXP `mxc_v4l2.c`）：`mxc_v4l2_probe` 中 `init_camera_struct()` 返回值**未检查**，其内部失败提前返回后 `cam->self->priv = cam` 使用未初始化指针 → oops。probe 崩溃 → `video_register_device` 永不执行 → `/dev/video1` 永不出现。
- 根因推断：现用 dtb 的 `v4l2_cap_0` 节点属性写法仍不满足该 BSP 驱动期望（该 dtb 非本 BSP 同源产物），具体缺失属性需以同源 dtb 为准。

### 5. 佐证信息

- 出厂系统摄像头正常 → 硬件（OV5640/CSI/I2C）与 BSP 驱动本身无问题，问题收敛在 dtb 配对上。
- I2C 链路正常：触摸屏 `1-0014`（goodix）探测成功，摄像头设备 `1-003c` 存在。

---

## 历史错误方案（已废弃）

1. rootfs 安装内核模块（`/lib/modules/4.1.15-ge48931b1/`）本身仍然需要，但旧的
   `mxc_v4l2_capture` / `ov5640_camera_int` 加载方案已废弃；
2. 将 compatible 改为 `ovti,ov564x`、补 `v4l2_cap_0` 的 DT 修改已撤销，原版 DTB
   才是 i.MX6ULL CSI 架构所需写法。

## 历史下一步（已废弃）

1. 板端从 EMMC p1 拷出同源出厂 dtb（`mount /dev/mmcblk1p1` → cp 至 NFS rootfs）；
2. 反编译出厂 dtb，对比 `v4l2_cap_0` / 摄像头节点属性差异，移植正确写法后重编译替换；
3. 板端重启（oops 后模块现场不干净，必须重启），验证 `/dev/video1` 出现；
4. `IPCAM_VIDEO_DEV` 改为实际采集节点的固定路径后重编部署；该做法已由当前源码的
   `mx6s-csi` 自动发现方案替代，固定路径仅保留为显式覆盖方式；
5. LCD 出图验证后，走 libjpeg-turbo 交叉编译解锁 MJPEG 推流（路径 B）。

## 备注

- NFS rootfs 调试链已稳定：Ubuntu 侧改动即时生效，板端仅需重启；本次 `bootargs` 需含 `nfsroot=...,v3`（内核 4.1.15 默认 NFSv2，新版 Ubuntu 服务器已移除 v2）。
- dtb 反编译/重编译会产生大量 phandle 数字引用类 warning，属正常现象，不影响生成。

---

## 根因修正（同日后续，对照正点原子出厂源码）

**此前第 4 节的结论不完整：不是 `v4l2_cap_0` 缺属性，而是整个驱动架构选错了。**

- i.MX6ULL **没有 IPU**，出厂摄像头链路为：
  `csi@021c4000（fsl,imx6ul-csi）→ mx6s_capture.ko → ov5640_camera.ko（I2C ID "ov5640"）→ /dev/videoX`
  源码依据：出厂 dts 无 `v4l2_cap_0`；`imx6ull.dtsi:997` 定义 csi 节点；`subdev/mx6s_capture.c` 为采集主设备；出厂配置 `CONFIG_VIDEO_MXC_CSI_CAMERA=m`。
- 之前手工补的 `v4l2_cap_0` + `mxc_v4l2_capture.ko` + `ov5640_camera_int.ko` 是 **i.MX6Q IPU 旧架构**：`mxc_v4l2_capture.c` 只匹配 `fsl,imx6q-v4l2-capture`，内部调用 `ipu_get_soc()`，在无 IPU 的 6ULL 上失败 → `init_camera_struct` 提前返回（返回值未检查）→ 野指针 oops。这比"缺 DT 属性"更准确地解释了 oops。
- 原始 dtb 其实是 mx6s 架构的正确写法：`ovti,ov5640` 恰好匹配 `ov5640_camera.ko` 的 I2C ID；**错的不是 dtb，而是加载了旧架构模块**。

**已执行的修正：**

1. dtb 已从备份恢复原版（撤销 `ovti,ov564x` 与 `v4l2_cap_0` 两处改动）；
2. `rcS` 改为 `modprobe mx6s_capture` + `modprobe ov5640_camera`，并注明禁止加载 `mxc_v4l2_capture.ko` / `ov5640_camera_int.ko`。
3. `ipcam` 默认自动扫描 `/dev/videoN`，只选择名称/QUERYCAP 信息匹配 `mx6s-csi` 且
   具备 video-capture、streaming capability 的节点；`IPCAM_VIDEO_DEV` 非空时才固定覆盖。
4. 采集启动接口使用运行时采集宽高，并拒绝奇数宽度及驱动静默调整尺寸，保证环形
   缓冲容量与 V4L2 实际帧尺寸一致。
5. `S90ipcam` 不再硬编码检查 `/dev/video0`，仅报告是否存在任意视频节点。

**板端预期：**

```
video0: PxP
video1: mx6s-csi（编号仅为示例，最终以 name/QUERYCAP 为准）
```

应用日志还应出现类似：

```text
camera device selected automatically: /dev/video1 (...mx6s-csi...)
camera negotiated: /dev/video1 640x480 fmt=YUYV
```

## 启动日志为空与开机自动启动修正（2026-09-08）

板端已经能够正常显示画面，且确认加载的是 `mx6s_capture`、`ov5640_camera`，但执行
`/etc/init.d/S90ipcam start` 后 `/var/log/ipcam.log` 为空。进一步对照发现，NFS
rootfs 中的 `S90ipcam` 仍是旧副本，使用了：

```sh
start-stop-daemon -S -b -m -p /var/run/ipcam.pid -x /usr/bin/ipcam -- > /var/log/ipcam.log 2>&1
```

BusyBox 1.29 的 `start-stop-daemon -b` 内部会将后台进程的标准输入、输出和错误输出
重定向到 `/dev/null`，因此外层 shell 的重定向无法捕获 `ipcam` 的 BCF2 风格日志。
画面正常而日志为空正是该行为的直接结果，并非 `ipcam_log` 没有输出。

同时，NFS `/etc/init.d/rcS` 只执行了两个 CSI 模块的 `modprobe`，没有调用
`S90ipcam`，所以 `run mybootnet` 进入 Linux 后不会自动启动应用。

本次修正如下：

1. [`scripts/rootfs/etc/init.d/S90ipcam`](scripts/rootfs/etc/init.d/S90ipcam) 去掉 `-b`，
   改由 `ash` 在显式日志重定向下后台执行，并轮询验证真实 PID；启动脚本自身的诊断
   同时写入日志和串口。
2. 启动脚本等待名称匹配 `mx6s-csi` 的节点，不等待或选择固定的 `/dev/video0`；
   摄像头缺失时仍启动一次应用，让应用输出候选节点诊断后安全退出。
3. NFS `/home/acomon/linux/nfs/rootfs/etc/init.d/rcS` 在加载
   `mx6s_capture`、`ov5640_camera` 后自动调用 `S90ipcam start`。
4. ARM 版 `ipcam`、启动脚本和 NFS `rcS` 已同步，U-Boot 的 `run mybootnet` 无需改动。

当前 display-only 版本的正常日志应包含 `MAIN`、`CAP`、`DISP` 和 `ENC stub active`；
完整 libjpeg-turbo 版本才会在客户端连接后出现 JPEG 编码及 `HTTP` 会话日志。

---

### 源码级验证（最终结果）

出厂内核源码已拷贝至 Ubuntu `/home/acomon/linux/IMX6ULL/`（`linux-imx-4.1.15-2.1.0-e48931b1-v2.8`），验证如下：

- `imx6ull-alientek-emmc.dts:205` → `&csi { status = "okay"; port { csi1_ep { remote-endpoint = <&ov5640_ep>; }; } }`；
- 同文件 `:320` → `ov5640: ov5640@3c { compatible = "ovti,ov5640"; ... }`；**全树无 `v4l2_cap_0`**（仅 `imx6qdl-*.dtsi` 有，属 6Q 板）；
- `drivers/media/platform/mxc/subdev/ov5640.c` 的 `i2c_device_id` = `{"ov5640"}`；
- 源码重编 dtb 与在用 dtb **md5 完全一致**（`b36d0f28500683278768bcea8afcf5d4`）→ **dtb 正确，无需改动**。

最终结论：根因 100% 是加载了 i.MX6Q IPU 旧架构模块，与 dtb 无关。

### 内核源码树使用备注

- 配置：`make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- imx_alientek_emmc_defconfig`
- 编译 dtb：`make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- HOSTCFLAGS="-fcommon" imx6ull-14x14-emmc-4.3-800x480-c.dtb`
- 注：本机 gcc 较新，编译内核自带 dtc 会因 `-fno-common` 报 `multiple definition of yylloc`，**必须加 `HOSTCFLAGS="-fcommon"`**。

## 串口日志与 LCD 画面异常修正（2026-09-08）

在 CSI 链路恢复后，板端已经可以出图，但仍观察到以下现象：

1. `S90ipcam` 启动后串口没有持续的 BCF2 风格运行日志，手工 `tail -f /var/log/ipcam.log`
   也可能看不到应用输出；
2. 画面变化时出现撕裂，偶尔刷新为绿屏；
3. 长时间运行后画面发生左右错位或分成两半；
4. `CAP` 约 30 fps，而 `DISP` 约 16.5 fps，不能用“采集帧率正常”推断 LCD 写入是安全的。

### 根因分析

- `S90ipcam` 使用 BusyBox `start-stop-daemon -b` 时，`-b` 会把真正后台进程的标准输出和
  标准错误重定向到 `/dev/null`。外层 shell 的重定向因此只能接住启动器，无法接住
  `ipcam_log` 写入的 `stderr`；这解释了画面正常而日志为空。
- 旧显示线程把 YUYV→RGB565 的结果直接写入 `/dev/fb0` 当前扫描的单一 framebuffer。
  当软件转换和 LCD 扫描同时访问同一页时，LCD 可能读到半帧，于是出现撕裂、半帧绿屏和
  跨行/左右错位。环形缓冲本身已经在生产者写入时复制 payload，不是本次画面错位的主要
  竞态来源。
- 采集端原先只按总帧大小判断，未把 `bytesperline`、`sizeimage` 和 `bytesused` 作为一组
  完整布局校验。若驱动返回短帧、带 padding 的帧或错误帧，显示转换按紧凑 YUYV 行读取，
  会把后续内容误当成当前行。

### 修改方案

1. [`scripts/rootfs/etc/init.d/S90ipcam`](scripts/rootfs/etc/init.d/S90ipcam) 保留
   `start-stop-daemon -m` 的 PID 管理，移除 `-b`；优先将应用 stdout/stderr 直接接到
   `/dev/console`，console 不可用时才回退到 `/var/log/ipcam.log`。因此执行
   `run mybootnet` 后，Linux `rcS` 自动启动的应用日志会直接出现在串口，无需手工
   `tail -f`。
2. [`src/services/ipcam_display.c`](src/services/ipcam_display.c) 启动时申请
   `yres_virtual >= 2*yres`，按 `line_length*yres` 计算页容量。每一帧先完整写后台 RGB565
   页，再通过 `FBIOPAN_DISPLAY` 让 `mxsfb` 在帧边界翻页；不支持双页时改用完整 staging
   帧，尝试 `FBIO_WAITFORVSYNC` 后再复制，并记录退化警告、翻页次数和失败次数。
3. [`src/services/ipcam_capture.c`](src/services/ipcam_capture.c) 对官方 OV5640 YUYV
   链路要求 `bytesperline == width*2`、`sizeimage == bytesperline*height`、
   `bytesused == sizeimage`。异常帧在进入 display/encode 环形缓冲前丢弃，并在统计中
   累计 `drop_size`，避免半帧进入显示路径。
4. NFS [`rcS`](/home/acomon/linux/nfs/rootfs/etc/init.d/rcS) 继续只加载
   `mx6s_capture`、`ov5640_camera`，随后自动调用 `S90ipcam start`；不恢复任何
   `mxc_v4l2_capture` 或 `ov5640_camera_int` 旧 IPU 模块。

### 中文注释说明

本次在 [`ipcam_display.c`](src/services/ipcam_display.c) 中补充了双页容量计算、
VSYNC 翻页、单页退化和 ring 释放时机的中文意图注释；在
[`ipcam_capture.c`](src/services/ipcam_capture.c) 中补充了异常帧节流日志和完整布局校验
约束的中文注释；在 [`S90ipcam`](scripts/rootfs/etc/init.d/S90ipcam) 中补充了
`-b` 丢日志、console 回退和 PID 校验原因说明。

### 预期板端日志

重启并执行 `run mybootnet` 后，串口应自动看到类似：

```text
INF [CAP ] ... camera device selected automatically: /dev/video1 (...mx6s-csi...)
INF [CAP ] ... camera negotiated: /dev/video1 640x480 fmt=YUYV bytesperline=1280 sizeimage=614400
INF [DISP] ... fb ready: 800x480 ... virtual_y=960 pages=2 mode=pageflip-vsync ...
INF [DISP] ... first frame: ... presented=pageflip ...
INF [CAP ] ... stats: ... drop_size=0 ...
INF [DISP] ... stats: ... present_errors=0 flips=... ...
```

若目标 framebuffer 不接受双页，日志应明确出现 `single-buffer-vsync`；若 VSYNC ioctl
也不可用，还会出现 `FBIO_WAITFORVSYNC unavailable` 警告，此时只能作为最后退化模式运行。
摄像头缺失或布局异常时，应用应安全退出并打印原因，不触发内核 oops。

## 长时间运行画面错位诊断与白光控制（2026-09-08）

### 现象与已有证据

板端已经能够稳定选择 `video1: mx6s-csi`，采集约 30 fps、显示约 16.5 fps，已有日志中
`drop_size=0`、`present_errors=0`。但运行一段时间后仍可能观察到：

1. 画面动态变化时撕裂或短暂绿屏；
2. 画面左右分成两半，原本左侧内容跑到右侧；
3. 仅凭帧率、帧长度和 `FBIOPAN_DISPLAY` 返回值，无法判断异常是在采集、ring、颜色转换、
   framebuffer 页复用还是 LCD 控制器侧发生。

### 本轮诊断改动

应用现在为每个可接受的 V4L2 帧建立同一条证据链：

- `CAP` 记录 V4L2 sequence、DMA buffer index、时间戳、`bytesused`、`bytesperline`、
  平均亮度和四象限 `probe`；同时统计 sequence 跳变/回退、时间戳异常、QBUF 错误和每个
  DMA buffer 的使用次数。
- `ipcam_ringbuffer` 在保留旧 `ipcam_ring_try_append()` / `ipcam_ring_append()` 接口的同时，
  增加带元数据的兼容接口；display/encode 两条 ring 保存同一份来源元数据。
- `DISP` 从 ring 重新计算源帧指纹，比较 `source_probe` 与 `ring_probe`；转换后再计算
  RGB565 目标页指纹，并关联 ring sequence、V4L2 sequence、页号、页地址、请求/实际
  `yoffset` 和 `FBIOPAN_DISPLAY` 耗时。
- framebuffer 每 5 秒重新读取布局；`fb_layout`、`bad_yoffset`、`pan_error`、
  `pan_timeout`、`fb_page_corrupt` 等异常会报警并停止继续写屏，避免把错误扩大。
- `IPCAM_LOG_LEVEL=5` 时额外输出每次翻页和四象限采样；默认仍采用首帧 + 5 秒摘要，
  防止串口 I/O 反过来影响 i.MX6ULL 调度。

日志判断顺序：`source_probe != ring_probe` 指向 ring 拷贝或内存覆盖；source/ring 一致而
RGB565 指纹异常指向转换或目标页；后台页复用前指纹变化指向 framebuffer 被覆盖；布局、
`yoffset` 或 pan 计数异常指向 framebuffer/LCD 提交；应用计数全部正常仍错位时，继续结合
`dmesg` 中的 `mxsfb` underflow/overflow 和中断状态排查内核侧。本轮不修改内核、DTS 或 U-Boot。

### 白光控制边界

OV5640 模组可能带白光灯，但当前 `ov5640_camera` 驱动不一定暴露可控的 V4L2 flash 控件，
也不应把开发板 `sys-led` 当成摄像头灯。应用默认启用暗光检测：YUYV 8×8 样本平均值不高于
35 连续 15 帧请求开启，高于 55 连续 30 帧请求关闭，状态变化至少间隔 3 秒。

控制顺序是 V4L2 `V4L2_CID_FLASH_LED_MODE`，其次是名称明确含 `camera`、`flash` 或 `white`
的 LED class `brightness`；拒绝直接访问 `/dev/i2c-*`、`sys-led`、`led0` 和不安全路径。
`IPCAM_WHITE_LIGHT=off` 可禁用自动控制；若板端没有安全控制节点，串口会出现
`white light control unavailable`，状态保持关闭，不能仅凭芯片支持闪光灯推断本板一定能点亮。

### 相关源码

- [`src/core/ipcam_frame_diag.h`](src/core/ipcam_frame_diag.h)
- [`src/core/ipcam_frame_diag.c`](src/core/ipcam_frame_diag.c)
- [`src/core/ipcam_ringbuffer.h`](src/core/ipcam_ringbuffer.h)
- [`src/core/ipcam_ringbuffer.c`](src/core/ipcam_ringbuffer.c)
- [`src/services/ipcam_capture.c`](src/services/ipcam_capture.c)
- [`src/services/ipcam_display.c`](src/services/ipcam_display.c)
- [`src/services/ipcam_light.c`](src/services/ipcam_light.c)
- [`src/services/ipcam_light.h`](src/services/ipcam_light.h)
- [`tests/test_ipcam_frame_diag.c`](tests/test_ipcam_frame_diag.c)

### 当前验证状态

- 主机版和 ARM 版 `ipcam-display-only` 编译通过；帧诊断、scale、stream 生命周期测试通过。
- 项目与 NFS 的启动脚本语法和模块加载方式保持正确；ARM 程序和启动脚本已同步并完成
  SHA256 复核，项目/NFS 二进制 SHA256 为
  `04dc7855cbb471ef5f921b16bd82d7ab6ab8c432c532a622f1422d4e20b090e3`。
- 板端至少 30 分钟静态、运动、暗光场景验证待更新程序后执行；重点记录第一次异常前后的
  `CAP/DISP/LIGHT` 摘要和 `IPCAM_LOG_LEVEL=5` 详细日志。
