# ARM encoding notes

## Benchmark table

| Codec | Preset | Resolution | %CPU (1 core) | Sender latency |
|---|---|---|---|---|
| H.264 | ultrafast | 1280×720@30 | ~10% | ~7 ms |
| H.264 | veryfast | 1280×720@30| ~23%  | ~14 ms |
| H.264 | medium | 1280×720@30 | ~60% | ~36ms |
| H.265 | ultrafast | 1280×720@30 | 72~100% | ~144ms |

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

### libcamerasrc — YUYV colour conversion bottleneck

The Pi CSI camera outputs `YUYV` (4:2:2 packed). GStreamer's `videoconvert`
must convert every frame to `I420` (planar 4:2:0) before the encoder.  
At 1280×720 this conversion alone saturates the CPU → ~5 fps.  
At 640×480 (4× fewer pixels) it keeps up at 30 fps.

| Resolution | Source | fps |
|---|---|---|
| 1280×720 | videotestsrc | 30 (no conversion needed) |
| 1280×720 | libcamerasrc (YUYV→I420) | ~5 |
| 640×480 | libcamerasrc (YUYV→I420) | 30 |

**Workaround:** use `-W 640 -H 480` with `-s 1`.

**Potential fix for 720p:** offload conversion to the GPU if
`glcolorconvert` is available:

```bash
gst-inspect-1.0 glcolorconvert
# if found, replace videoconvert with:
# glupload ! glcolorconvert ! gldownload
```

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
