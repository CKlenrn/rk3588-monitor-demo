# monitor_demo feature candidate

> This directory preserves the feature candidate as an independent source
> tree. It must be built and deployed separately from any known-good release.

`monitor_demo` 是一个与原 SystemUI、官方 `hdmirx` 和旧学习工程隔离的
RK3588 HDMI 输入监视器应用。应用是独立 Wayland 全屏客户端，不使用
SystemUI Remote Objects。

## 当前实现

- 启动时把 HDMI RX EDID 设置为 `2` 并回读确认；helper 接管前释放
  `/dev/video80`，explicit-sync 运行时才临时启用 `low_latency=Y`，退出后
  恢复并回读 `N`。
- 读取 `/dev/video80` 的 DV timings、V4L2 多平面格式和源变化事件。
- 产品视频链路为同 release 的
  `v4l2_wayland_explicit_sync -> DMA-BUF -> Wayland explicit-sync -> DRM plane`；
  原 `videopipelinecontroller.h/.cpp` 及其 GStreamer 路径仍完整保留。
- 设置 `MONITOR_DEMO_LOW_LATENCY=0` 可运行 `low_latency=N` 的产品共存
  baseline；正常产品启动默认运行 explicit-sync。
- 支持无信号、热插拔、格式变化、管线重建、错误重试和退出释放。
- Qt Quick 提供透明 HUD、HDMI 状态、网格、2.39:1 画幅线和显示设置。
- 实验副本新增中心标记、90% 安全框、1x/2x/4x 中心放大、候选伪色、
  斑马纹、峰值与亮度波形图。
  构图辅助只使用 Qt HUD plane；中心放大复用现有 RGA source crop；伪色
  默认关闭，启用后使用两个由 fence 串联的 RGA 作业，不做 CPU 像素转换。
  其余三项在视频提交后共用一次 240x135 亮度采样，由 Qt HUD 绘制，
  不增加视频 RGA 作业。
- `ICameraControlBackend` 与 Sony Camera Remote SDK 2.02.00 后端已建立；
  Sony 后端在独立线程中完成 USB FX30 枚举、重连、录制、曝光、白平衡、
  AF 和触控对焦，控件按相机返回能力动态显示。
- 未提供 Sony SDK 时仍可构建；该版本只报告控制不可用，不会模拟连接、
  录制或命令成功。

该功能候选已经完成交叉构建并在 RK3588 上运行，但伪色新增处理时间，
以及斑马纹、峰值和波形图的精度、刷新开销与端到端延迟仍未形成产品级
兼容矩阵；当前实现边界见 `MONITOR_FEATURE_ARCHITECTURE.md`。

## 目录约定

- `SOURCE_ROOT`：本目录的绝对路径。
- `SDK_ROOT`：RK3588 Linux SDK 根目录。
- `BUILD_ROOT`：仓库之外的可再生构建目录。
- `SONY_CRSDK_ROOT`：可选的 Sony Camera Remote SDK ARMv8 根目录，必须位于
  仓库之外。
- 板端版本：`/opt/monitor_demo/releases/<version>/monitor_demo`
- 同版本低延迟 helper：
  `/opt/monitor_demo/releases/<version>/v4l2_wayland_explicit_sync`
- 当前版本：`/opt/monitor_demo/current`
- 上一版本：`/opt/monitor_demo/previous`

Sony SDK 必须放在本仓库之外，不提交 SDK 头文件、库或示例。

## 构建

在 Ubuntu 使用 SDK 的 qmake，从独立构建目录执行。Sony 启用版先设置
Git 外的 ARMv8 SDK 根目录：

```sh
export SOURCE_ROOT=/path/to/rk3588-monitor-demo/feature-ui
export SDK_ROOT=/path/to/atk_dlrk3588_linux6.1
export BUILD_ROOT=/path/to/build/monitor_demo

mkdir -p "$BUILD_ROOT/feature-ui"
cd "$BUILD_ROOT/feature-ui"
"$SDK_ROOT/buildroot/output/alientek_rk3588/host/bin/qmake" \
  "$SOURCE_ROOT/monitor_demo.pro"
make -j4

SDK_ROOT="$SDK_ROOT" BUILD_DIR="$BUILD_ROOT/feature-helper" \
  sh "$SOURCE_ROOT/tools/low_latency/build.sh"
```

启用 Sony 后端时，额外把 `SONY_CRSDK_ROOT` 设置为仓库外 SDK 目录。不设置
时仍可构建不可用后端，HDMI 监看功能不依赖 Sony SDK。
`tools/sony_sdk_init_probe.cpp` 只调用 `SCRSDK::Init()` 和 `Release()`，用于
板端初始化诊断，不枚举或控制相机。

构建完成后必须用 `file`、`readelf -h` 和 `readelf -d` 检查 AArch64 架构
及动态依赖。

## 安全门禁

`packaging` 中的文件只是交付材料，不会自行安装或启用。板端顺序固定为：

1. 串口控制台可用后，暂存新的平台文件。
2. 从串口运行 `monitor-demo-preflight --serial-rescue-confirmed`，生成旧文件
   SHA-256 与原脚本副本。
3. 安装一个新 release，`current` 指向它。
4. 运行 `/usr/libexec/monitor-demo-validation/test-current-weston.sh`；退出时脚本
   自动恢复原 SystemUI。
5. 先运行 `/usr/libexec/monitor-demo-validation/test-isolated-weston.sh baseline`
   验证动态视频、原 HUD 和触控共存；退出时脚本自动恢复原 Weston 和
   SystemUI。
6. baseline 通过后再运行同一脚本的 `explicit` 模式，验收 acquire/release
   fence、画面、交互及 `low_latency=N` 恢复。
7. 实际观察通过后分别记录 `manual-ui` 与 `isolated-weston` 验证标记。
8. 只有全部门禁通过，才运行 `monitor-demo-enable-boot` 做可逆改名。

启用脚本不会自动重启。应留在串口控制台中重启并完成冷启动验证。

一键恢复命令为 `monitor-demo-restore-ui`。它停止新进程、把新脚本改为
`K` 前缀、把原脚本恢复为 `S` 前缀、核对基线副本并重启。只回退应用版本
使用 `monitor-demo-rollback-release`，不改变启动链。

## 官方资料

- [Linux V4L2 Userspace API](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2.html)
- [V4L2 DV timings](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/vidioc-g-dv-timings.html)
- [GStreamer v4l2src](https://gstreamer.freedesktop.org/documentation/video4linux2/v4l2src.html)
- [GStreamer queue](https://gstreamer.freedesktop.org/documentation/coreelements/queue.html)
- [GStreamer waylandsink](https://gstreamer.freedesktop.org/documentation/waylandsink/index.html)
- [Qt 5 QML Applications](https://doc.qt.io/qt-5/qmlapplications.html)
- [Qt 5 C++ and QML Integration](https://doc.qt.io/qt-5/qtqml-cppintegration-topic.html)
- [Sony Camera Remote SDK](https://support.d-imaging.sony.co.jp/app/sdk/en/index.html)
