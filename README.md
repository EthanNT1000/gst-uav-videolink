# gst-uav-videolink

Low-latency H.264/H.265 video link for UAV systems, built with GStreamer.
Demonstrates RTP/UDP streaming, an RTSP server, and MAVLink video-stream
announcement compatible with QGroundControl.

```text
Air vehicle                          Ground station
──────────────────                   ──────────────────────────────
videotestsrc / libcamerasrc / v4l2src
  │
x264enc / x265enc                    rtph264depay / rtph265depay
  │   tune=zerolatency               rtpjitterbuffer
rtph264pay / rtph265pay                │
  │   config-interval=1             avdec_h264 / avdec_h265
udpsink ──── RTP/UDP ──────────────► autovideosink / filesink
  │
  └── MAVLink UDP ─────────────────► QGroundControl (auto-discovers stream)
```

## Components

| Binary | Role |
| --- | --- |
| `sender` | Encodes and sends an RTP/UDP video stream; optionally announces via MAVLink |
| `receiver` | Receives, decodes, and displays or saves the stream |
| `rtsp_server` | Exposes the stream over RTSP for on-demand clients; optionally announces via MAVLink |

MAVLink announcement is built into `sender` and `rtsp_server` — no separate
script is needed. Pass `-g GCS_HOST` to enable it.

## Dependencies

```bash
# Ubuntu / Debian
sudo apt install \
    cmake pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    libgstrtspserver-1.0-dev \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Usage

### RTP/UDP — point-to-point (lowest latency)

Terminal 1 — sender:

```bash
./build/sender -h 127.0.0.1 -p 5600 -b 2000 -c 0 -W 1280 -H 720 -f 30
```

Terminal 2 — receiver:

```bash
./build/receiver -p 5600 -c h264 -l 200
```

Save to file instead of displaying:

```bash
./build/receiver -p 5600 -c h264 -s recording.mp4
```

### Codec

Both `sender` and `rtsp_server` accept `-c CODEC`:

| `-c` | Codec |
| --- | --- |
| `0` (default) | H.264 — widest client compatibility |
| `1` | H.265 — ~30% lower bitrate at equivalent quality |

```bash
./build/sender   -c 1 -b 1200   # H.265
./build/receiver -c h265
```

### Source selection

Both `sender` and `rtsp_server` accept `-s SOURCE`:

| `-s` | Source |
| --- | --- |
| `0` (default) | `videotestsrc` — animated test pattern, no hardware needed |
| `1` | `libcamerasrc` — Raspberry Pi CSI camera (libcamera stack) |
| `2` | `v4l2src` — USB or V4L2 camera (`/dev/video0`) |

```bash
./build/sender -s 1 -h 192.168.1.1 -p 5600    # CSI camera
./build/sender -s 2 -h 192.168.1.1 -p 5600    # USB camera
```

### Speed preset

Both `sender` and `rtsp_server` accept `-P PRESET` to trade encoding CPU for quality:

| `-P` | Preset | Use case |
| --- | --- | --- |
| `0` (default) | `ultrafast` | Embedded ARM, minimum latency |
| `1` | `superfast` | |
| `2` | `veryfast` | |
| `3` | `faster` | |
| `4` | `fast` | |
| `5` | `medium` | Desktop, better quality |
| `6` | `slow` | |
| `7` | `slower` | |
| `8` | `veryslow` | Offline / bench testing only |

### Encoder threads (H.264 only)

Pass `-T THREADS` to set the x264enc thread count. Has no effect with `-c 1`
(H.265) because x265enc does not expose a threads property.

```bash
./build/sender -c 0 -T 4    # x264enc with 4 threads
```

`0` (default) lets x264enc choose automatically based on CPU count.

### Local preview with FPS overlay

Pass `-d` to open a local display window showing the pre-encode frame rate via
`fpsdisplaysink`. The stream is tee'd so the network output is unaffected.

```bash
./build/sender      -d -s 1 -h 192.168.1.1 -p 5600
./build/rtsp_server -d -s 1 -p 8554
```

### RTSP — on-demand, multi-client

```bash
./build/rtsp_server -p 8554 -c 0 -b 2000

# Connect from any RTSP client
vlc rtsp://127.0.0.1:8554/uav
ffplay rtsp://127.0.0.1:8554/uav
gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/uav latency=200 \
  ! rtph264depay ! h264parse ! avdec_h264 ! autovideosink
```

### MAVLink auto-discovery

Pass `-g GCS_HOST` to either binary and it will send `VIDEO_STREAM_INFORMATION`
so QGroundControl discovers the feed automatically:

```bash
# RTP sender with MAVLink announce
./build/sender -h 192.168.1.1 -p 5600 -g 192.168.1.1 -G 14550

# RTSP server with MAVLink announce
./build/rtsp_server -p 8554 -g 192.168.1.1 -G 14550
```

## Tuning

### CPU governor (Raspberry Pi)

The default `ondemand` governor keeps the CPU at idle frequency between frames,
preventing it from reaching full speed for encoding. Switch to `performance`
before running:

```bash
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

To make it permanent across reboots:

```bash
sudo apt install cpufrequtils
echo 'GOVERNOR="performance"' | sudo tee /etc/default/cpufrequtils
sudo systemctl enable cpufrequtils
```

Confirmed on Pi 5: `ondemand` prevents the CPU reaching full clock, causing
the encoder to fall well below the target fps. `performance` → 30 fps with
`x264enc speed-preset=ultrafast` at 720p30.

### Known issue — libcamerasrc bottleneck at 1280×720 (Pi 5)

`sender -s 1` (libcamerasrc) at 1280×720 delivers ~5 fps regardless of
CPU governor or encoder thread count. At 640×480 it reaches 30 fps.

**Root cause identified:** the camera outputs `YUYV` (4:2:2 packed).
`videoconvert` must convert every frame to `I420` (planar) before x264enc.
At 720p that conversion saturates the CPU. At 480p (4× fewer pixels) it
keeps up at 30 fps.

**What was tried and ruled out:**

| Fix attempted | Result |
| --- | --- |
| `performance` CPU governor | No effect on camera path |
| `sudo apt install libcamera-ipa` | Removed IPA warning, no fps change |
| `threads=4` on x264enc (`-T 4`) | No effect |
| Force `format=NV12` in caps | Camera does not support NV12; pipeline error |
| `queue` after libcamerasrc (now in code) | Queue warning gone, still 5 fps |
| Lower resolution: `-W 640 -H 480` | **30 fps — confirmed bottleneck is YUYV→I420** |

**Workaround:** use `-W 640 -H 480` for 30 fps with the CSI camera.

**Next step for 720p:** check if the Pi 5 GPU can accelerate the colour
conversion:

```bash
gst-inspect-1.0 glcolorconvert
```

If available, replace `videoconvert` with
`glupload ! glcolorconvert ! gldownload` to offload YUYV→I420 to the GPU.

### Measuring pipeline latency

Use the built-in GStreamer latency tracer — no code changes needed:

```bash
GST_TRACERS="latency" GST_DEBUG="GST_TRACER:7" ./build/sender -s 0 2>&1 \
  | grep latency
```

This prints per-element and end-to-end latency measurements to stderr as each
frame passes through the pipeline. Useful for comparing presets and sources
without a separate receiver.

To compare two presets back-to-back:

```bash
# ultrafast (default)
GST_TRACERS="latency" GST_DEBUG="GST_TRACER:7" ./build/sender -P 0 2>&1 \
  | grep "latency, src"

# medium
GST_TRACERS="latency" GST_DEBUG="GST_TRACER:7" ./build/sender -P 5 2>&1 \
  | grep "latency, src"
```

## Design notes

### Low-latency encoding parameters

| Parameter | Value | Reason |
| --- | --- | --- |
| `tune=zerolatency` | enabled | Disables B-frames and encoder look-ahead so every frame is emitted immediately |
| `speed-preset` | ultrafast | Minimal CPU for encoding; important on embedded ARM (Jetson, RPI) |
| `key-int-max=30` | 30 frames (~1 s at 30 fps) | Forces an IDR regularly; a receiver after link loss can decode within 1 s |
| `config-interval=1` | every IDR | Repeats SPS/PPS in-band so no SDP exchange is required for recovery |

### RTP vs RTSP

**RTP/UDP** is preferred for fixed UAV links: no connection overhead, minimal
stack latency, and the receiver can start with a pre-configured port.
**RTSP** is preferred when the ground station network has multiple clients,
when bandwidth probing (`SETUP`/`PLAY`) is needed, or when clients need to
auto-discover codec parameters via `DESCRIBE`.

### MAVLink integration

MAVLink does not carry video — it carries *metadata about* the video.
`VIDEO_STREAM_INFORMATION` (message ID 269) tells the GCS:

- where to find the stream (URI)
- codec, resolution, bitrate, framerate
- stream type (UDP H.264, RTSP, etc.)

QGroundControl subscribes to this message and opens the stream automatically,
which is why port 5600 is used as the default (the QGC expected port for
`udp://0.0.0.0:5600`).
