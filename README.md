# RK3588 Monitor Demo

This private repository preserves two verified development snapshots of an
RK3588 HDMI input monitor. The project name and executable name are
`monitor_demo`.

## Versions

| Directory | Purpose | Per-frame video path |
| --- | --- | --- |
| [`feature-ui`](feature-ui/) | Qt HUD plus false color, zebra, focus peaking, waveform and zoom experiments | HDMI RX -> V4L2 DMA-BUF -> RGA rotate/scale -> Wayland/DRM video plane; Qt HUD uses a separate plane |
| [`low-latency-no-rga`](low-latency-no-rga/) | No-Qt, no-RGA latency baseline used by `/opt/rbk` | HDMI RX -> V4L2 DMA-BUF -> implicit-sync Wayland direct display -> DRM plane |

The baseline contains no RGA pixel rotation or scaling. Its client still sets
`WL_OUTPUT_TRANSFORM_90` metadata, so it should not be described as containing
no orientation setting at all.

## Provenance

- The `feature-ui` sources came from the verified Windows and Ubuntu feature
  candidate. Before publication cleanup, its historical source manifest
  SHA-256 was
  `3c20eb99db5b92eef060bc8c315a6974c78c5a3d08516a301d2ee9f442217c06`.
- The baseline helper source SHA-256 is
  `c1597e97481e534ede65938b9eb1b2d1b00c9d350b352a72ca5101f5d6f36a8f`.
- That source was archived beside an ARM64 helper with SHA-256
  `29dc604f0d6979788cfff8ef3796e34d5fbae59d5895b9e8beeee08752c21a31`,
  which matches the helper that ran from `/opt/rbk`. Binaries are deliberately
  not stored in this repository.
- Each version directory contains a `SOURCE_SHA256.txt` manifest for its
  current publication files.

## Requirements

- The matching Alientek RK3588 Linux 6.1 Buildroot SDK and its cross toolchain.
- Weston 14.0.2 as patched by the same BSP.
- RK3588 target hardware for runtime validation.
- Sony Camera Remote SDK only for the optional Sony control backend. The Sony
  SDK is not included and must remain outside this repository.

Set `SDK_ROOT` to the SDK root before using either helper build script. Each
version has its own README with the remaining build and runtime details.

## Scope

These are development snapshots, not a universal HDMI compatibility claim.
The low-latency baseline was validated with NV16 3840x2160p50 and has known
geometry limitations. The feature version still requires signal-format,
color, latency and monitor-assist validation before product release.

No credentials, board images, generated binaries, Rockchip SDK documents or
Sony SDK files are included.
