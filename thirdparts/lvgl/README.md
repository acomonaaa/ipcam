# LVGL 外部依赖

本项目固定使用 LVGL `v9.5.0`。源码目录被 `.gitignore` 排除，避免把
第三方完整源码复制进应用仓库；在构建前执行：

```sh
git clone --branch v9.5.0 --depth 1 \
  https://github.com/lvgl/lvgl.git src
```

顶层 `Makefile` 会递归编译 `src/` 下的 C 源文件，并将对象和静态库放在
项目根目录的 `build/` 下。项目根目录的 `lv_conf.h` 关闭 LVGL 自带
fbdev/evdev 后端，应用自己的 `src/services/ipcam_lvgl.c` 负责把 LVGL
局部 RGB565 刷新复制到 `/dev/fb0`。
