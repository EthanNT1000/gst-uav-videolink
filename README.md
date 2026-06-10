# gst-uav-videolink

Low-latency H.264/H.265 video link for UAV systems, built with GStreamer.
Demonstrates RTP/UDP streaming, an RTSP server, and MAVLink video-stream
announcement compatible with QGroundControl.

```text
Air vehicle                          Ground station
──────────────────                   ──────────────────────────────
videotestsrc / v4l2src
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

| Binary / Script | Role |
| --- | --- |
| `sender` | Encodes and sends an RTP/UDP video stream |
| `receiver` | Receives, decodes, and displays or saves the stream |
| `rtsp_server` | Exposes the stream over RTSP for on-demand clients |
| `mavlink_announce.py` | Sends MAVLink `VIDEO_STREAM_INFORMATION` so QGC discovers the feed |

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

pip install pymavlink   # for mavlink_announce.py
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Usage

### RTP/UDP — point-to-point (lowest latency)

Terminal 1 — sender (simulates UAV camera):

```bash
./build/sender -h 127.0.0.1 -p 5600 -b 2000 -c h264 -W 1280 -H 720 -f 30
```

Terminal 2 — receiver (simulates ground station):

```bash
./build/receiver -p 5600 -c h264 -l 200
```

Save to file instead of displaying:

```bash
./build/receiver -p 5600 -c h264 -s recording.mp4
```

### H.265 (better compression, ~30% lower bitrate at equivalent quality)

```bash
./build/sender   -c h265 -b 1200
./build/receiver -c h265
```

### RTSP — on-demand, multi-client

```bash
./build/rtsp_server -p 8554 -c h264 -b 2000

# Connect from any RTSP client
vlc rtsp://127.0.0.1:8554/uav
ffplay rtsp://127.0.0.1:8554/uav
gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/uav latency=200 \
  ! rtph264depay ! h264parse ! avdec_h264 ! autovideosink
```

### MAVLink auto-discovery

Run alongside `sender` so QGroundControl shows the video without manual setup:

```bash
# Announce RTP stream
python3 mavlink_announce.py --uri udp://0.0.0.0:5600 --type udp --gcs 192.168.1.1:14550

# Announce RTSP stream
python3 mavlink_announce.py --uri rtsp://192.168.1.100:8554/uav --type rtsp --gcs 192.168.1.1:14550
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

### Replacing the test source with a real camera

Swap `videotestsrc pattern=ball` for a V4L2 USB camera:

```gst
v4l2src device=/dev/video0 ! image/jpeg,width=1280,height=720 ! jpegdec
```

Or for CSI cameras on NVIDIA Jetson:

```gst
nvarguscamerasrc sensor-id=0 ! nvvidconv ! video/x-raw(memory:NVMM),format=I420
```
