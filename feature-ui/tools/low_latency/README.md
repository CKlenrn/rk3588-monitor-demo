# HDMI RX low-latency experiment

This tool is an isolated low-latency experiment. It does not replace the
`monitor_demo` GStreamer pipeline.

The monitor-feature copy adds optional helper arguments:

```text
--zoom 1|2|4     centered RGA source crop; no extra RGA stage
--false-color    NV16 luma -> rotated/scaled Y8 -> RGA palette -> XB24
--zebra 0..100   sampled luma zebra threshold
--peaking 1..100 sampled luma-gradient sensitivity
--waveform       sampled luma waveform for the Qt HUD
--self-test      validate options, LUT, bit packing, and nonblocking output
```

The default remains `--zoom 1` with false color disabled. That path keeps the
single asynchronous NV16 RGA operation used by the validated product build.
False color adds one fence-chained RGA palette operation and must be measured
on the RK3588 before it can be accepted as a low-latency product mode.

Zebra, peaking, and waveform share one 240x135 luma analysis after the video
surface commit, once every five frames. They read the completed RGA output,
add no video RGA operation, and never delay the current frame waiting for an
explicit fence. Their sampled accuracy and runtime cost still require RK3588
measurement.

The target explicit-sync data path is:

```text
rk_hdmirx V4L2 MMAP/EXPBUF
  -> acquire fence from v4l2_buffer.timecode.userbits
  -> Wayland linux-dmabuf + explicit synchronization
  -> Weston atomic DRM backend + linear Esmart plane
  -> Wayland release fence
  -> wait, then requeue the V4L2 buffer
```

The current HDMI input must be locked as NV16 or NV24. Inputs at 59 fps or
faster use the HDMI RX driver's line-10 release point. Slower inputs still use
the explicit-sync path, but the driver releases the buffer at roughly two
thirds of the frame and therefore has a higher latency floor.

Run the same verifier twice without changing the input timing, format, or
binary:

```sh
/tmp/run-isolated-low-latency.sh /tmp/v4l2_wayland_explicit_sync baseline
/tmp/run-isolated-low-latency.sh /tmp/v4l2_wayland_explicit_sync explicit
```

Each mode runs for eight seconds by default. `baseline` keeps `low_latency=N`
and uses implicit sync. It must prove that the current input reaches a
linear Esmart hardware plane before writing a boot-specific marker. `explicit`
requires that marker, probes Wayland explicit sync while the parameter is
still `N`, then enables `low_latency=Y` before the client's first QBUF.

Both modes save the V4L2 preflight, client synchronization trace, DRM atomic
log and DRM state under `/tmp/monitor-demo-low-latency-MODE/`. They validate
the per-buffer release-before-QBUF order and the matching DRM plane. Explicit
mode additionally requires a positive `IN_FENCE_FD` on that plane. Every exit
path restores `low_latency=N`, releases `/dev/video80`, removes the isolated
socket, and restores the original Weston, SystemUI and control center.

The older `run-on-board.sh` is an unisolated GL-composition experiment and does
not provide these recovery or evidence checks. The explicit-sync test is
allowed only after the matching hardware-plane baseline passes.
