# ARM encoding notes

## Benchmark table

| Codec | Preset | Resolution | %CPU (1 core) | Sender latency |
| --- | --- | --- | --- | --- |
| H.264 | ultrafast | 1280×720@30 | ~10% | ~7 ms |
| H.264 | veryfast | 1280×720@30 | ~23% | ~14 ms |
| H.264 | medium | 1280×720@30 | ~60% | ~36 ms |
| H.265 | ultrafast | 1280×720@30 | 72~100% | ~144 ms |

## Raspberry Pi 5

**SoC:** BCM2712 — quad-core Cortex-A76 @ up to 2.4 GHz  
**libx264:** compiled with NEON acceleration  
**x265enc:** no `threads` property — thread count cannot be set from the pipeline

### CPU governor

The default `ondemand` governor idles the CPU between frames and cannot ramp
up in time for the next encode. Switch to `performance` before running:

```bash
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

Without this, videotestsrc at 720p30 falls well below 30 fps even at
`ultrafast`. With `performance` the encoder reaches 30 fps at `ultrafast`.

### libcamerasrc with a USB camera — wrong element, YUYV forced

`libcamerasrc` can enumerate USB cameras through libcamera's "simple" pipeline,
but it only exposes `YUYV` regardless of what formats the camera natively
supports. A USB camera that can output MJPEG via V4L2 is forced into YUYV
when accessed through libcamerasrc, throwing away the camera's best format.

The same camera (Logitech BRIO 100) accessed two ways:

| Element | Format seen by pipeline | 720p fps |
| --- | --- | --- |
| `libcamerasrc` (`-s 1`) | YUYV → CPU YUYV→I420 convert | ~5 |
| `v4l2src -F 1` (`-s 2 -F 1`) | MJPEG → jpegdec → I420 | 30 |

**The 5 fps was never a hardware limit — it was the wrong GStreamer element.**

**Investigation steps that confirmed root cause:**

| Test | Result |
| --- | --- |
| Force `format=NV12` via libcamerasrc | `not-negotiated` |
| Force `format=I420` via libcamerasrc | `not-negotiated` |
| No caps (libcamerasrc default) | `YUY2 @ 1920×1080` |
| Display-only (`autovideosink`) | GPU converts YUY2 — no CPU bottleneck for display, only encoding |
| Lower resolution (`-W 640 -H 480`) | 30 fps — confirmed bottleneck is YUYV→I420 pixel count |
| Switch to `v4l2src -F 1` | 30 fps at 720p — MJPEG path bypasses conversion entirely |

**Measured fps (x264enc ultrafast, `performance` governor):**

| Resolution | Element | Format | fps |
| --- | --- | --- | --- |
| 1920×1080 | libcamerasrc (no caps) | YUYV | ~2.5 |
| 1280×720 | libcamerasrc | YUYV→I420 | ~5 |
| 640×480 | libcamerasrc | YUYV→I420 | 30 |
| 1280×720 | videotestsrc | I420 (native) | 30 |
| 1280×720 | v4l2src MJPEG | MJPEG→I420 | 30 |

**Rule:** for USB cameras, always use `v4l2src` (`-s 2`). Only use
`libcamerasrc` (`-s 1`) for CSI cameras connected via MIPI CSI-2.

**Pi 5 has no hardware H.264 encoder.** The Pi 4 had `omxh264enc` / MMAL;
Pi 5 (BCM2712 / VideoCore VII) dropped MMAL entirely and V4L2 M2M encoder
support was not included at launch. Software x264enc is the only option.

### USB camera (v4l2src) — MJPEG path

Most modern USB webcams output MJPEG at 720p/1080p to stay within USB 2.0
bandwidth. MJPEG avoids the YUYV→I420 bottleneck: `jpegdec` decodes each
frame directly to I420, so `videoconvert` becomes a no-op before x264enc.

Pipeline (`-s 2 -F 1`):

```text
v4l2src ! image/jpeg,W×H@fps ! jpegdec ! videoconvert ! x264enc
```

**Logitech BRIO 100 on Pi 5 — measured fps:**

| Resolution | Format | fps |
| --- | --- | --- |
| 1280×720 | MJPEG (`-F 1`) | 30 |
| any | YUYV raw (`-F 0`) | limited by USB bandwidth at high res |

Use `v4l2-ctl --list-formats-ext` to check what formats your camera supports.
If `MJPG` is listed at your target resolution, use `-F 1`.

### H.265 with libcamerasrc

`x265enc` requires `I420`. When using `-s 1 -c 1` the pipeline automatically
inserts `! video/x-raw,format=I420` after `videoconvert` to force an explicit
conversion target; without it caps negotiation may settle on a packed format
that x265enc rejects.

### Analysis

H.264 ultrafast and veryfast are both viable on Cortex-A76 for a shared
flight stack — ultrafast costs ~10% of one core, leaving ample headroom
for ArduPilot, ROS2, and telemetry processes. Medium (60%) begins to
compete with other flight stack processes and should be used only on a
dedicated companion computer with no other CPU-intensive tasks.

H.265 ultrafast consumes 72–100% of one core and introduces ~144 ms of
encoder-side latency alone — 20× that of H.264 ultrafast. The 100% spikes
indicate the Cortex-A76 cannot sustain H.265 software encoding at 720p30
in real-time. This is the expected result: H.265 is not viable in software
on this class of ARM core. The correct solution on production platforms is
hardware encoding — Jetson NVENC or i.MX VPU encodes H.265 at a fraction
of this CPU cost and with sub-10ms latency.
