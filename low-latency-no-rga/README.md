# No-UI, no-RGA direct-display baseline

This directory preserves the source set paired with the helper that ran from
`/opt/rbk` on 2026-08-21 and 2026-08-22.

## Video path

```text
HDMI RX NV16
  -> four V4L2 MMAP/EXPBUF DMA-BUF buffers
  -> Wayland linux-dmabuf direct-display
  -> Weston PLANES_ONLY
  -> Esmart DRM plane
```

The tested invocation used `-v /dev/video80 -f NV16 -q -g`, meaning implicit
sync and direct display. It did not start Qt, link librga, allocate RGA target
buffers, or execute RGA pixel rotation/scaling. It did set
`WL_OUTPUT_TRANSFORM_90` metadata.

## Verified identity

- `v4l2_wayland_explicit_sync.c`:
  `c1597e97481e534ede65938b9eb1b2d1b00c9d350b352a72ca5101f5d6f36a8f`
- Paired ARM64 helper, not included:
  `29dc604f0d6979788cfff8ef3796e34d5fbae59d5895b9e8beeee08752c21a31`
- `weston.ini`:
  `42689d3439f44e1da7988c374f54ac2f31d8ce734039327eb163f30ac68d5adc`

## Build

Use the matching RK3588 Buildroot SDK:

```sh
export SDK_ROOT=/path/to/atk_dlrk3588_linux6.1
BUILD_DIR="$PWD/build" sh ./build.sh
```

The script uses generated protocol sources and headers from the SDK's Weston
14.0.2 build tree. It does not download dependencies.

## Weston sources

`weston/` contains the exact `state-propose.c` and `kms.c` snapshots used for
the working PLANES_ONLY DRM backend, plus the matching isolated
`weston.ini`. These files target the vendor-patched Weston 14.0.2 tree in the
same BSP; do not copy them into an unrelated upstream Weston checkout.

## Limitations

- Verified only for the recorded RK3588 BSP and an NV16 3840x2160p50 input.
- The baseline deliberately omits the Qt HUD and monitor-assist functions.
- The recorded snapshot has a known aspect/crop limitation on the portrait
  1080x1920 panel.
- The runner changes DRM and V4L2 ownership. Use it only with a serial recovery
  path and after reviewing the target paths.
