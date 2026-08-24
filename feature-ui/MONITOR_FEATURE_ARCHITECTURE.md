# 监视器功能实验架构

> 本目录是成功版 `standalone/monitor_demo` 的隔离副本。成功版和板端当前版本不得由本实验修改。

## 分层原则

```text
QML HUD（开关、参数、标尺、构图辅助、分析位图）
    -> LowLatencyVideoPipelineController（保存设置、重启 helper）
        -> v4l2_wayland_explicit_sync（逐帧处理）
            -> V4L2 DQBUF
            -> RGA
            -> Wayland DMA-BUF
            -> DRM video plane
```

- QML 不读取视频 DMA-BUF，只管理交互并绘制 helper 返回的紧凑分析位图。
- 控制器不处理视频帧，只转换设置并校验/转发分析数据帧。
- 只有 helper/RGA 可以修改视频像素。
- 所有新增图像处理默认关闭；关闭时保持成功版参数和单次 RGA 路径。

## 本轮功能归属

| 功能 | 处理位置 | 视频处理成本 | 本轮状态 |
|---|---|---:|---|
| 三分网格、2.39:1 画幅线 | Qt HUD plane | 0 次视频拷贝 | 保留现有实现 |
| 中心标记、90% 安全框 | Qt HUD plane | 0 次视频拷贝 | 新增 |
| 1x/2x/4x 中心放大 | 现有 RGA source crop | 仍为 1 次 RGA | 新增 |
| 伪色 | RGA Y8 + palette LUT | 2 次图像 RGA + 1 次 LUT 更新，中间一次 CPU 等待 | 新增候选 |
| 斑马纹 | 已释放 RGA 亮度面稀疏采样 + Qt HUD | 不增加视频 RGA 作业 | 新增候选 |
| 峰值对焦 | 同一亮度网格相邻梯度 + Qt HUD | 不增加视频 RGA 作业 | 新增候选 |
| 亮度波形图 | 同一亮度网格聚合 + Qt HUD | 不增加视频 RGA 作业 | 新增候选 |

## 低延迟路径

### 标准与中心放大

```text
4K NV16 V4L2 DMA-BUF
    -> 单次异步 RGA：source crop + ROT_90 + scale
    -> 1080x1920 NV16 DMA-BUF
    -> Esmart video plane
```

1x、2x、4x 只改变 RGA 的 source rectangle，不新增缓冲阶段或 CPU 映射。

### 伪色候选

```text
4K NV16 的 Y 平面
    -> 异步 RGA：source crop + ROT_90 + scale
    -> 1080x1920 Y8 中间 DMA-BUF
    -> CPU 等待第一级 completion fence 落地
    -> 同步更新 256 项 RGBA LUT 并执行 RGA palette
    -> 1080x1920 XB24 DMA-BUF
    -> Esmart video plane
```

- 初始化只用 `imcheck_t()` 验证 LUT 参数，不再对空 stage/dst 执行 palette。每帧调用官方 palette API 时携带 LUT；当前 librga 会先同步更新硬件 LUT 表，再执行 palette，不做 CPU 逐像素转换。
- 两级之间不再用 fence 串联异步提交。palette 只能由 RGA2 执行，而 RGA2 不可靠地遵守输入 acquire fence：palette 会在第一级仍在写 stage 时就开始读它，导致每帧内容撕裂、画面持续闪烁。现在先在 CPU 上等第一级 fence，再同步执行 palette。
- palette 返回时目标缓冲已是最终像素，因此伪色帧不再向 compositor 提交 acquire fence。代价是伪色开启时每帧多一次 CPU 等待，约等于一次 RGA 作业时间。
- LUT 使用 `ROCKCHIP_BO_DMA32`；1080x1920 Y8 stage 和 XB24 目标使用 `ROCKCHIP_BO_CONTIG | ROCKCHIP_BO_DMA32`。实机证明仅用 DMA32 时大图被分配为离散页，RGA2 palette 通过自身 MMU 访问会触发 bus error；连续低地址 buffer 可稳定完成同尺寸 palette 探针。
- Qt 继续使用 Cluster3 AB24 plane；伪色视频使用不带 alpha 的 XB24。
- 伪色默认关闭。其新增处理时间必须在有 HDMI 后实测，不预设延迟门禁。
- 当前 LUT 是便于验证链路的候选分区，不宣称符合某一厂商或行业 IRE 色标。
- 伪色 helper 在 V4L2 连接成功后、RGA 初始化前读取 RGA2 core0 的
  runtime power policy。原值为 `auto` 时仅在该 helper 生命周期内保持
  `on`，所有统一退出路径恢复 `auto`；原值已为 `on` 时不改写。标准路径、
  `--probe` 和 `--self-test` 不访问该策略。实机单变量实验确认这可避免
  palette LUT 更新与转换两个 job 之间 runtime suspend 丢失硬件色表；代价
  是伪色开启期间 RGA2 power domain 不再自动挂起，具体功耗尚未测量。

### 斑马纹、峰值与波形图候选

```text
当前帧 wl_surface_commit（视频先提交，并标记本帧待分析）
    -> compositor 释放该 buffer
    -> 释放路径已等待 RGA completion fence
    -> 只读映射 1080x1920 NV16 Y 或伪色 Y8 stage
    -> 每 5 帧抽取一次 240x135 亮度网格
    -> 同一次采样生成 zebra / peaking / waveform 三张 1-bit 位图
    -> Qt AB24 HUD plane 绘制
```

- 采样点在 buffer 释放路径上，不在 commit 之后。此前在 commit 后立即用非阻塞 poll 检查刚提交的 RGA fence，该 job 只提交了几微秒而需要约 1ms，fence 几乎永不就绪，导致分析帧被全部丢弃：波形图看似只在触摸屏幕时才刷新，斑马纹和峰值完全不出现。释放路径上 fence 已经等待完成，像素即显示内容，且 buffer 尚未重新入队 V4L2。
- 不读取 4K HDMI 源帧，不复制完整视频帧，不增加 RGA 作业或显示前等待。
- 斑马纹默认阈值 95%，可调 0~100%；百分比映射到 limited range Y 的 16~235。此前按 0~255 映射，95% 得到 242，高于 HDMI RX 实际最大白 235，因此永不触发。仍不宣称已完成精确 IRE 标定。
- 峰值默认灵敏度 50，可调 1~100%；基于亮度梯度，颜色为候选红色。梯度跨 2 个目标像素而非相邻像素：4K 缩到 1080 后相邻像素已被插值平滑，几乎不携带边缘能量。阈值同时下调到 `110 - 灵敏度`（下限 12）。
- 波形图是 240x135 采样亮度波形，不是全分辨率测量仪器。分析约每 5 帧更新一次。纵轴同样按 limited range 16~235 归一化。
- DMA-BUF 映射、cache sync 或协议解析失败时只丢弃分析帧，视频 helper 继续运行。
- 分析输出按整帧检查 pipe 容量并非阻塞写入；容量不足时丢弃该分析帧。Qt 接收端只绘制本次读取到的最新完整帧，避免积压反向拖慢视频循环。
- 仅开启波形图时不再绘制全屏透明 zebra/peaking 图层，只绘制波形面板；三者的位图协议和采样算法不变。
- Qt 收到完整分析帧后同时请求 `QQuickPaintedItem` 和所属 `QQuickWindow` 更新，使 HUD surface commit 不依赖触摸事件。

## 设置生效方式

- 无 HDMI 时只保存本进程内的选择，UI 可正常操作。
- HDMI 到来时按当前选择启动 helper。
- streaming 时改变伪色、放大倍率或分析设置，会停止并重新启动 helper。这样避免新增控制 IPC 和逐帧锁；代价是切换瞬间会短暂重建视频链路。
- 伪色关闭后，helper 重新使用成功版 `-d NV16` 路径。

## 验收边界

- 无 HDMI：应用启动、HUD 操作、开关状态、面板布局和无信号状态。
- 有 HDMI：标准路径回归、伪色颜色顺序、中心放大区域、三种分析图层的方向/阈值/刷新、DRM plane、触摸、热切换和延迟。
- 三种分析功能目前是低分辨率候选；全分辨率精度、精确 IRE LUT 和最终刷新率仍需实机证据后决定。

## 参考依据

- SDK RGA 中文开发指南：`docs/cn/Common/RGA/Rockchip_Developer_Guide_RGA_CN.pdf`
- SDK 官方 palette 示例：`external/linux-rga/samples/palette_demo/src/rga_palette_demo.cpp`
- [Linux V4L2 Userspace API](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2.html)
- [Wayland linux-dmabuf protocol](https://wayland.app/protocols/linux-dmabuf-v1)
- [Qt Quick Controls 2](https://doc.qt.io/qt-5/qtquickcontrols2-index.html)
