# `ipcam` 正确性与线程生命周期修复

## 元信息

| 项 | 内容 |
| --- | --- |
| 修改时间 | 2026-09-07 13:35:10（UTC+8） |
| 作者 / Agent | Codex（ProjectProof / 鼠鼠实习妙妙工具） |
| 关联提交 | `fix(ipcam): 修复缩放与线程生命周期` |
| 影响模块 | scale / capture / ringbuffer / encode / stream / tests |

## 问题现象

1. 输出缩放路径的坐标计算使用 `1<<15` 生成定点值，但后续按 Q16 的 `>>16` 解释；上采样结果可能只覆盖源图部分区域。
2. V4L2 采集线程发生 `STREAMON`、`DQBUF` 或 `QBUF` 致命错误时，没有统一停止全局运行状态；`buf.index` 也在缺少范围检查时直接作为数组下标使用。
3. 采集停止流程在 `pthread_join` 前关闭 V4L2 fd，采集线程退出收尾阶段仍可能访问同一 fd。
4. HTTP 客户端使用 detached thread，停止逻辑只轮询约 5 秒后就销毁 `client_mtx`；慢客户端仍可能在收尾时访问该锁和服务上下文。
5. `/healthz` 固定返回成功，无法区分服务停止、JPEG ring 已关闭和暂时没有帧。

## 问题分析

1. 缩放公式的中心采样坐标要求所有中间值统一使用 Q16。原路径的横向和纵向计算将 Q15 常量与 Q16 右移混用，计算比例不一致；该问题与 JPEG 编码 API 无关，应从缩放逻辑中独立出来测试。
2. V4L2 的 `DQBUF` 返回值包含驱动提供的 buffer index，任何来自驱动的索引都必须在访问 `ctx->bufs` 前校验。采集错误如果只让线程返回而不清零共享运行标志，会留下其他线程继续运行但没有新帧的“假健康”状态。
3. fd 是线程共享资源。主线程 close 后，采集线程可能正处于 ioctl 或日志收尾；此外 fd 数字可能被系统复用，因此必须先 `STREAMOFF`/join，再统一 teardown。
4. detached 只负责线程资源回收，不保证线程已经停止访问 `ctx`。安全条件应是：关闭 socket 和 ring 使阻塞操作返回，等待 `client_cnt` 归零，然后再销毁 mutex/cond。

相关代码 / 配置：

- [`ipcam_scale.c`](../src/core/ipcam_scale.c)
- [`ipcam_encode.c`](../src/services/ipcam_encode.c)
- [`ipcam_capture.c`](../src/services/ipcam_capture.c)
- [`ipcam_ringbuffer.c`](../src/core/ipcam_ringbuffer.c)
- [`ipcam_stream.c`](../src/services/ipcam_stream.c)

## 问题根因

- 缩放实现缺少单一的 Q16 定点约定和独立边界测试。
- 采集线程错误路径和停止路径没有汇聚到同一个资源所有权收尾模型。
- HTTP 服务只维护活动计数，没有保存客户端 fd，也没有用同步原语等待 detached client 完整退出。
- 健康检查只表达“HTTP 路由可访问”，没有读取运行标志和输出 ring 状态。

## 修改方案

### 变更文件列表

- [`src/core/ipcam_scale.h`](../src/core/ipcam_scale.h) — 新增内部 Q16 缩放接口。
- [`src/core/ipcam_scale.c`](../src/core/ipcam_scale.c) — 抽取并修复横向/纵向 Q16 坐标计算，增加尺寸和参数校验。
- [`src/services/ipcam_encode.c`](../src/services/ipcam_encode.c) — 使用缩放模块，增加平面分配溢出检查和编码线程失败收敛。
- [`src/services/ipcam_capture.c`](../src/services/ipcam_capture.c) — 增加 V4L2 能力/index 校验，调整 STREAMOFF、join、teardown 顺序。
- [`src/core/ipcam_ringbuffer.h`](../src/core/ipcam_ringbuffer.h) — 增加 ring 关闭状态查询接口。
- [`src/core/ipcam_ringbuffer.c`](../src/core/ipcam_ringbuffer.c) — 实现带锁的关闭状态查询。
- [`src/services/ipcam_stream.h`](../src/services/ipcam_stream.h) — 增加客户端 fd 和退出条件变量状态。
- [`src/services/ipcam_stream.c`](../src/services/ipcam_stream.c) — 保存客户端 fd，shutdown 后等待计数归零，完善 healthz 和启动失败清理。
- [`tests/test_ipcam_scale.c`](../tests/test_ipcam_scale.c) — 增加 1:1、2×2→4×4、大尺寸、奇数尺寸和非法参数测试。
- [`tests/test_ipcam_stream_lifecycle.c`](../tests/test_ipcam_stream_lifecycle.c) — 主机侧覆盖多客户端等待、stop 唤醒、计数归零和 `/healthz` 状态切换。

### 关键改动说明

1. `ipcam_scale` 统一以 `1<<16` 表示 Q16 的 1，并限制尺寸上限使坐标乘法保持在 `int64_t` 安全范围内；编码线程在缩放参数非法时停止而不是无限跳过坏帧。
2. V4L2 停止时不再在 join 前 close fd；致命错误会把共享运行状态清零，主线程随后按统一 cleanup 流程回收。
3. HTTP 客户端仍保持 detached，避免扩大线程接口改造；新增客户端 fd 槽位、`shutdown` 和条件变量等待，最终 `close` 由客户端线程执行，确保销毁同步对象前 `client_cnt` 已归零且避免双重 close/fd 复用竞态。
4. `/healthz` 返回运行标志、JPEG ring 关闭状态和当前队列数量；它只表示进程/输出链路最小状态，不宣称完整子系统监控。

### 中文注释说明（代码类变更必填）

本次在以下位置补充了说明约束、资源所有权和失败收敛原因的中文注释：

- [`src/core/ipcam_scale.c`](../src/core/ipcam_scale.c) — Q16 约定、溢出边界和像素坐标校验。
- [`src/core/ipcam_scale.h`](../src/core/ipcam_scale.h) — 缩放接口的参数/返回值语义。
- [`src/services/ipcam_encode.c`](../src/services/ipcam_encode.c) — 编码线程失败和缩放缓冲生命周期。
- [`src/services/ipcam_capture.c`](../src/services/ipcam_capture.c) — V4L2 buffer 所有权、错误索引和 stop 顺序。
- [`src/core/ipcam_ringbuffer.c`](../src/core/ipcam_ringbuffer.c) — ring 状态读取的加锁原因。
- [`src/services/ipcam_stream.c`](../src/services/ipcam_stream.c) — detached client 的 fd 关闭、条件变量等待和锁销毁前提。
- [`tests/test_ipcam_scale.c`](../tests/test_ipcam_scale.c) — 每组测试覆盖的边界和目的。
- [`tests/test_ipcam_stream_lifecycle.c`](../tests/test_ipcam_stream_lifecycle.c) — 用最小桩隔离板端依赖，说明多客户端等待、stop 唤醒和 healthz 状态断言的原因。

## 验证结果

- 缩放单测：主机 `cc -std=gnu99 -Wall -Wextra -Werror` 构建并运行通过。
- 缩放 Sanitizer：ASan/UBSan 构建并运行通过。
- HTTP 生命周期：ASan/UBSan 构建真实 `ipcam_stream.c + ipcam_ringbuffer.c` 测试夹具并连续运行 10 次通过，覆盖三个等待中的 stream 客户端、`ring_close`/`shutdown` 唤醒、`client_cnt == 0` 和 `/healthz` 的 200/503。
- 主机 `ipcam-display-only`：构建通过；仅有已有的 `_GNU_SOURCE` 重定义和 OTA `strncpy` 截断警告。
- ARM `ipcam-display-only`：使用仓库现有 Linaro `arm-linux-gnueabihf` 工具链构建通过。
- 完整 MJPEG：未验证。当前仓库缺少 `thirdparts/libjpeg-turbo/install/include/turbojpeg.h` 和对应库，因此没有把 TurboJPEG 编码、链接或目标板视频输出标记为通过。
- 真实设备场景：未验证。当前环境没有可用的 V4L2/Framebuffer 设备，慢客户端、SIGTERM、真实多客户端视频和编码初始化失败仍需在目标环境执行。

## 预期结果

1. 缩放模块在 1:1、上采样、下采样、奇数尺寸和非法参数下具有可重复的边界行为。
2. V4L2 错误会让整个服务进入停止路径，不能继续报告没有产出的视频服务为健康。
3. 采集线程 join 完成前不关闭 fd；HTTP client 线程退出前不销毁相关锁和上下文。
4. `/healthz` 在运行和 ring 关闭时返回不同 HTTP 状态和 JSON 状态。
5. 本次不改变 OTA 认证、HTTPS、签名、严格广播、4G 重连或 crash handler 的完整改造范围。

## 备注

- 当前工作树在 `dev` 分支，原有未提交修改保留；本记录对应的代码尚未提交。
- 主机 display-only 构建、ARM display-only 构建、缩放单测（含 ASan/UBSan）和主机 HTTP 生命周期测试已通过；目标板 V4L2/Framebuffer、完整 TurboJPEG 和真实设备场景仍需按环境可用性继续验证。
- 若条件变量等待因外部 fd/驱动异常长期不返回，应先修复阻塞路径，不得恢复“超时后直接销毁锁”的旧行为。
