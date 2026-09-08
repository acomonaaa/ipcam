# 兼容出厂 mx6s-csi 的空 FourCC 返回值

## 元信息

| 项 | 内容 |
| --- | --- |
| 修改时间 | 2026-09-08 16:58:58（UTC+8） |
| 作者 / Agent | Codex |
| 关联提交 | 待提交 |
| 影响模块 | capture / 文档 |

## 问题现象

板端已经正确加载 `mx6s_capture`、`ov5640_camera`，并注册出
`/dev/video1: mx6s-csi`。ipcam 也成功跳过了 `/dev/video0: PxP` 并选择了
`/dev/video1`，但启动在格式检查处失败：

```text
/dev/video1 did not honor YUYV (negotiated )
```

## 问题分析

对照当前出厂 BSP 的 `mx6s_capture.c` 可见，`VIDIOC_S_FMT` 的实现通过
`format_by_fourcc()` 选择了请求格式，并调用传感器的 `s_mbus_fmt`；但保存到
`csi_dev->pix` 时只复制了宽度、高度、帧大小和场类型，没有复制
`pixelformat`。随后 `VIDIOC_G_FMT` 直接返回 `csi_dev->pix`，因此
`G_FMT.fmt.pix.pixelformat` 为 0。

这与板端日志完全一致：S_FMT 并未报错，传感器已在 dmesg 中注册，只有应用打印的
G_FMT FourCC 为空。故障不是 DTS、I2C、OV5640 探测或 CSI 节点选择问题。

## 问题根因

出厂 Linux 4.1.15 的 `mx6s-csi` V4L2 驱动存在历史实现缺陷：S_FMT 内部已经按
请求的 YUYV 配置 CSI，但 G_FMT 没有回填对应的 `pixelformat`。应用原先把这个
“空 FourCC”当成格式不匹配，导致正常的摄像头链路被误判为失败。

## 修改方案

- 在 [`../src/services/ipcam_capture.c`](../src/services/ipcam_capture.c) 中保存并
  校验 `S_FMT` 回写的 FourCC；只有 S_FMT 确认请求的 YUYV 后，才允许继续检查 G_FMT。
- 对已经通过自动发现确认是 `mx6s-csi` 的节点，兼容 `G_FMT.pixelformat == 0`，
  使用 S_FMT 的已确认结果作为格式依据。
- 显式覆盖路径只有在其 driver/card/sysfs 名称能够识别为 `mx6s-csi` 时才启用该
  兼容行为；其它 V4L2 设备返回 0 或错误 FourCC 仍安全失败。
- 继续严格校验 S_FMT/G_FMT 的宽高，以及 `sizeimage == width * height * 2`，防止
  环形缓冲容量与驱动实际帧大小不一致。
- 增加固定长度 FourCC 文本和十六进制日志；返回 0 时显示为
  `NONE(0x00000000)`，避免空字符串掩盖驱动行为。
- 在 [`../src/services/ipcam_capture.h`](../src/services/ipcam_capture.h) 增加兼容标记
  的中文说明，明确其只适用于已识别的出厂 CSI 驱动。
- 在 [`../README.md`](../README.md) 补充该 BSP 兼容约束，避免后续把空 G_FMT 误判为
  摄像头硬件故障。

## 预期结果

板端再次启动时应出现类似日志：

```text
camera device selected automatically: /dev/video1 (...)
/dev/video1 G_FMT returned pixelformat=NONE; using S_FMT=YUYV for legacy mx6s-csi
camera negotiated: /dev/video1 640x480 fmt=YUYV
```

随后采集线程和 LCD 显示线程继续运行；若只有 PxP、没有 CSI 节点，或 CSI 节点
确实拒绝 S_FMT/尺寸/帧大小，程序仍会退出并打印诊断，不触发内核 oops。

## 验证结果

- 主机版 `make -B CROSS_COMPILE= ipcam-display-only` 通过。
- ARM 版 `make -B ipcam-display-only` 通过，产物为 ARM EABI5 ELF。
- `ipcam_capture.c` 严格编译（`-Wall -Wextra -Wformat=2 -Werror`）通过。
- 两个 init 脚本 `sh -n` 检查通过。
- 新 ARM 版 [`../ipcam`](../ipcam) 已更新到 NFS rootfs 的
  `/home/acomon/linux/nfs/rootfs/usr/bin/ipcam`，待板端重启后复验。
- 本次补充的中文注释覆盖 FourCC 诊断、旧版驱动兼容边界和尺寸/帧大小安全约束。

## 备注

- 未修改官方 DTS、内核模块或 NFS `rcS`；仍使用 `mx6s_capture + ov5640_camera`。
- 当前工作区已有未提交修改，本次继续保留；未提交、未推送。
