# GUI 设计到 LVGL 映射

## 来源与画布

当前设计源为 [`IPCam_LCD_UI_sentry.pen`](IPCam_LCD_UI_sentry.pen)，同步自远端
`dev` 的提交 `b3aa1df`（`fix(ui): 适配 800x480 LCD 设计`）。该文件包含八个
根页面，全部使用 `800×480` 画布；`config/ipcam_config.h` 和
`src/ui/ipcam_ui.h` 的尺寸常量与其保持一致。

## 页面映射

| 设计页面 | LVGL 实现 | 运行时职责 |
| --- | --- | --- |
| P01 本地预览首页 | [`src/ui/ipcam_ui_screen_home.c`](../src/ui/ipcam_ui_screen_home.c) | 本地预览、补光、拍照、录像、视口入口；视频帧显示在 476×268 区域 |
| P02 电脑观看首页 | [`src/ui/ipcam_ui_screen_home.c`](../src/ui/ipcam_ui_screen_home.c) | 访问地址、网络状态、设备信息和页面入口 |
| P03 本地全屏预览 | [`src/ui/ipcam_ui_screen_fullscreen.c`](../src/ui/ipcam_ui_screen_fullscreen.c) | 全屏视频、缩放、录像和退出；复用同一 RGB565 视频帧 |
| P04 视频设置 | [`src/ui/ipcam_ui_screen_video.c`](../src/ui/ipcam_ui_screen_video.c) | 分辨率、帧率、JPEG 质量和水平/垂直翻转；应用按钮提交设置 |
| P05 屏幕设置 | [`src/ui/ipcam_ui_screen_screen.c`](../src/ui/ipcam_ui_screen_screen.c) | 亮度、自动熄屏时间和显示缩放入口 |
| P06 存储状态 | [`src/ui/ipcam_ui_screen_storage.c`](../src/ui/ipcam_ui_screen_storage.c) | SD 卡挂载、容量、使用率和录像策略状态 |
| P07 网络与系统 | [`src/ui/ipcam_ui_screen_network.c`](../src/ui/ipcam_ui_screen_network.c) | 网络信息、MAC/IP/端口及系统信息页签 |
| P08 启动及异常状态 | [`src/ui/ipcam_ui_screen_state.c`](../src/ui/ipcam_ui_screen_state.c) | 启动阶段、采集/LCD/触摸/网络/存储诊断和错误提示 |

页面使用绝对坐标创建控件，坐标直接对应 `.pen` 的页面坐标。通用面板、按钮、
单选行、开关和顶部栏集中在 [`src/ui/ipcam_ui_components.c`](../src/ui/ipcam_ui_components.c)，
颜色、圆角和间距 token 集中在 `ipcam_ui_styles.c`，这样页面布局变化时不会复制
整套 LVGL 样式定义。

## 数据与动作链路

```text
.pen 页面
   │ 800×480 坐标与视觉 token
   ▼
src/ui/ipcam_ui_screen_*.c
   ▼
ipcam_ui 状态快照 ───────────────┐
   ▲                            │
   │                            ▼
ipcam_lvgl.c ← RGB565 帧   LVGL 事件回调
   │                            │
   └── framebuffer flush        ▼
                           动作队列
                                │
                                ▼
                    main.c → ipcam_control
```

`ipcam_lvgl` 线程负责 LVGL 定时器、输入快照、状态刷新和视频帧绑定；事件回调只
写入轻量动作队列，不执行硬件 I/O 或阻塞操作。主循环通过
`ipcam_lvgl_process_actions()` 把动作转换成统一的 `ipcam_control_command_t`，
因此 GUI、HTTP 和 `camctl` 共用同一套控制契约。

P01/P03 的视频对象由 `ipcam_ui_set_video_source()` 绑定完整 RGB565
`lv_image_dsc_t`。没有新帧时保留设计中的占位提示；有效帧到达后只更新 image
source，不重新创建页面对象。

## 字体与移植注意事项

`ipcam_ui_fonts_t` 支持向标题、正文、小字和图标分别注入 `lv_font_t`。项目保留
Montserrat 14 作为默认回退和 `LV_SYMBOL_*` 图标字体；当前页面字符串已经由
`scripts/generate_ui_fonts.sh` 提取，并生成 14/16/20 px 的 CJK 子集字体，文件为
`src/ui/ipcam_ui_font_cjk_14.c`、`16.c` 和 `20.c`。`ipcam_lvgl_start()` 将三种
字号通过 `ipcam_ui_create()` 注入，因此不需要在 NFS 上额外放置 OTF/TTF 文件。
后续 `.pen` 或页面 C 字符串增加中文时，应重新运行该脚本并交叉编译。

主机侧已完成内存显示器的 P01～P08 创建、切屏、状态刷新和销毁 smoke test，
并完成 `ipcam-display-only` 链接验证；真实 `/dev/fb0`、evdev 触摸和板端字体
显示仍需上板验证。

`.pen` 是视觉设计的来源，`src/ui` 是当前 LVGL 的手工实现层，不会把设计文件
直接当作可编译代码。后续设计修改应先更新页面尺寸/坐标映射，再同步状态字段、
动作绑定和板端验收项。
