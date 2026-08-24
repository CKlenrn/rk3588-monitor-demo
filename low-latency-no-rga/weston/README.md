# Weston snapshot: 20260821-planes-only-working

This directory contains the exact text sources recorded with the first working
low-latency video path:

- NV16 DMA-BUF direct display through the Esmart primary plane.
- PLANES_ONLY mode with kiosk-shell.
- DSI-2 at approximately 1080x1920@54 Hz.

Recorded SHA-256 values:

```text
623ecf5a30bf6486b6bdd3edeb6edfecd12de1a43dbcbb44053588ad8d4c1131  state-propose.c
d4c3bae1ab221dcb7e019ef465aad88728f11a04335d318f6c5fafd6fd866dcc  kms.c
42689d3439f44e1da7988c374f54ac2f31d8ce734039327eb163f30ac68d5adc  weston.ini
```

The C files are complete snapshots from the vendor-patched Weston 14.0.2 tree,
not a portable patch for upstream Weston. The known baseline issue is incorrect
4K-to-portrait-panel crop/aspect geometry.
