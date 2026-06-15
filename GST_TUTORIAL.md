# UAV Video Link — Learning Guide

One-day study plan covering every concept in this project.
Estimated time: 5–6 hours reading + hands-on.

---

## Table of Contents

1. [The Big Picture](#1-the-big-picture)
2. [GStreamer](#2-gstreamer)
   - 2.7 [Branching with tee](#27-branching-with-tee)
   - 2.8 [Common caps types](#28-common-caps-types)
   - 2.9 [Transcoding and format conversion](#29-transcoding-and-format-conversion)
3. [H.264 and H.265 Encoding](#3-h264-and-h265-encoding)
4. [RTP — Real-time Transport Protocol](#4-rtp--real-time-transport-protocol)
5. [RTSP — Real-Time Streaming Protocol](#5-rtsp--real-time-streaming-protocol)
6. [MAVLink Video Announcement](#6-mavlink-video-announcement)
7. [Code Walkthrough](#7-code-walkthrough)
8. [Hands-on Exercises](#8-hands-on-exercises)
9. [Interview Talking Points](#9-interview-talking-points)

---

## 1. The Big Picture

```
UAV (air)                                   Ground Station
─────────────────────────────               ──────────────────────────
Camera → encode → RTP/UDP ─────────────────► decode → display
                                            or save to file
         MAVLink UDP ──────────────────────► QGroundControl
                  "video is at udp://0.0.0.0:5600, H264, 30fps"
```

**Why two separate channels?**

| Channel | Protocol | Purpose |
| --- | --- | --- |
| Video | RTP/UDP or RTSP | Pixel data, high bandwidth |
| Telemetry | MAVLink/UDP | Control, status, metadata |

MAVLink does **not** carry video. It carries a message that says *where* the video is,
so the ground control software can open the right port/URI automatically.

---

## 2. GStreamer

### 2.1 Mental model

GStreamer is a **pipeline framework**. Data flows left → right through a chain of
*elements*, each connected by *pads* (input/output ports).

```
[source] → [filter] → [filter] → [sink]
```

Every element has:
- **src pad** (output) — pushes buffers downstream
- **sink pad** (input) — receives buffers from upstream

### 2.2 The pipeline string syntax

```
gst-launch-1.0  element1  !  element2  !  element3
```

`!` links the src pad of the left element to the sink pad of the right element.

**Caps** (capabilities) are type constraints between elements:

```
videotestsrc ! video/x-raw,width=1280,height=720,framerate=30/1 ! videoconvert
```

The `video/x-raw,...` between `!` marks is a caps filter — it forces negotiation
to a specific format so downstream elements know what they're getting.

### 2.3 Key element categories

| Category | Examples | Role |
| --- | --- | --- |
| Source | `videotestsrc`, `v4l2src`, `libcamerasrc` | Produces raw data |
| Codec encoder | `x264enc`, `x265enc` | Compress video |
| RTP payloader | `rtph264pay`, `rtph265pay` | Wrap encoded frames into RTP packets |
| Network sink | `udpsink` | Send UDP packets |
| Network source | `udpsrc` | Receive UDP packets |
| Jitter buffer | `rtpjitterbuffer` | Reorder/buffer RTP for smooth playback |
| RTP depayloader | `rtph264depay` | Extract encoded frames from RTP packets |
| Codec decoder | `avdec_h264` | Decompress video |
| Display sink | `autovideosink` | Render to screen |
| FPS display sink | `fpsdisplaysink` | Wraps another sink and overlays measured frame rate |
| Fan-out | `tee` | Copies one stream to N independent branches |
| Branch buffer | `queue` | Gives each tee branch its own thread; prevents deadlock |
| File sink | `filesink` | Write to disk |

### 2.4 The GLib main loop

GStreamer is asynchronous. The pipeline runs in background threads.
Your main thread runs a `GMainLoop` to:
- Receive bus messages (errors, EOS)
- Fire periodic timers (our MAVLink tick)

```c
g_loop = g_main_loop_new(NULL, FALSE);
gst_bus_add_watch(bus, bus_cb, g_loop);   // pipeline events → callback
g_timeout_add_seconds(1, tick_cb, NULL);  // 1 Hz timer → callback
g_main_loop_run(g_loop);                  // blocks until quit()
```

All callbacks fire on the **same thread** as the main loop — no locking needed
between the bus callback and the MAVLink timer callback.

### 2.5 Pipeline states

```
NULL → READY → PAUSED → PLAYING
```

`gst_element_set_state(pipeline, GST_STATE_PLAYING)` kicks off data flow.
You must set state back to `NULL` before freeing: `gst_element_set_state(pipeline, GST_STATE_NULL)`.

### 2.6 Try it — gst-launch-1.0

The `gst-launch-1.0` CLI lets you test pipelines without writing C:

```bash
# Test pattern on screen
gst-launch-1.0 videotestsrc ! videoconvert ! autovideosink

# H.264 encode and decode in one pipeline (loopback)
gst-launch-1.0 \
  videotestsrc ! videoconvert \
  ! x264enc tune=zerolatency bitrate=2000 \
  ! rtph264pay ! rtph264depay \
  ! avdec_h264 ! videoconvert ! autovideosink

# Send RTP to localhost
gst-launch-1.0 videotestsrc ! videoconvert \
  ! x264enc tune=zerolatency bitrate=2000 \
  ! rtph264pay config-interval=1 pt=96 \
  ! udpsink host=127.0.0.1 port=5600
```

### 2.7 Branching with tee

A linear pipeline can only send data to one destination. `tee` splits one
stream into N identical copies, each flowing down its own branch independently.

```
                     ┌─ branch A ─► udpsink (network)
source ──► tee ──────┤
                     └─ branch B ─► fpsdisplaysink (local screen)
```

#### Why queue is mandatory on every branch

`tee` pushes the same buffer to all branches synchronously on the upstream
thread. If branch A blocks (e.g. waiting for the network), branch B is also
blocked — the whole pipeline stalls.

`queue` puts each branch in its own thread with an internal buffer, so branches
run independently:

```
tee ──► queue ──► udpsink          (thread A)
    └─► queue ──► fpsdisplaysink   (thread B)
```

Without `queue`, a slow sink causes a deadlock. Always add one `queue` per
branch immediately after `tee`.

#### Pipeline string syntax

In `gst-launch-1.0` / `gst_parse_launch` you name the tee and then address
each branch using `name.`:

```
... ! tee name=t
t. ! queue ! sink-A
t. ! queue ! sink-B
```

Whitespace and newlines are ignored — this is identical:

```
... ! tee name=t  t. ! queue ! sink-A  t. ! queue ! sink-B
```

#### How it is used in sender.c (`-d` flag)

```c
// Without -d: single linear branch
"source ! caps ! videoconvert ! x264enc ... ! rtph264pay ... "
"! udpsink host=%s port=%d sync=false async=false"

// With -d: tee fans out to network + local FPS display
"source ! caps ! videoconvert ! tee name=t "
"t. ! queue ! x264enc ... ! rtph264pay ... "
"! udpsink host=%s port=%d sync=false async=false "
"t. ! queue ! fpsdisplaysink video-sink=autovideosink sync=false"
```

`fpsdisplaysink` wraps another sink (here `autovideosink`) and overlays the
measured frame rate on the video window. It receives the raw pre-encode frames,
so the FPS shown reflects the source rate before any encoder buffering.

`sync=false` on both sinks lets each branch clock itself independently —
without it the two sinks would try to synchronise to the same pipeline clock
and one would starve the other.

#### Try it with gst-launch-1.0

```bash
# Split test pattern: encode+send on one branch, display FPS on the other
gst-launch-1.0 \
  videotestsrc pattern=ball is-live=true \
  ! video/x-raw,width=1280,height=720,framerate=30/1 \
  ! videoconvert \
  ! tee name=t \
  t. ! queue ! x264enc tune=zerolatency bitrate=2000 \
       ! rtph264pay config-interval=1 pt=96 \
       ! udpsink host=127.0.0.1 port=5600 sync=false async=false \
  t. ! queue ! fpsdisplaysink video-sink=autovideosink sync=false
```

#### Reading the fpsdisplaysink overlay

When `-d` is active a window opens with four numbers overlaid on the video:

```
rendered: 450  dropped: 2  current: 29.97 fps  average: 29.95 fps
```

| Number | What it measures |
| --- | --- |
| **rendered** | Total frames that made it to the screen since the pipeline started |
| **dropped** | Total frames the sink discarded because they arrived too late to display |
| **current** | FPS measured over the last 500 ms window |
| **average** | FPS averaged over the entire run |

**On the receiver side (`receiver -d`):** measures the decoded output rate
*after* the jitter buffer and decoder. `rendered` counts every frame that
reached the screen; `dropped` counts frames the sink discarded because
they arrived too late.

#### Interpreting common patterns

| Pattern | Cause | Where to look |
| --- | --- | --- |
| `dropped=0`, `average ≈ target fps` | Healthy | — |
| `dropped=0`, `average << target fps` | Sender encoder too slow; only N fps is arriving over the link | Check sender CPU / lower `-W`/`-H`/`-f`/`-b` on sender |
| `dropped` climbing, `average ≈ target fps` | Receiver decoder or display can't keep up | Check receiver CPU |
| `dropped` climbing, `average << target fps` | Packet loss AND receiver overload | Check RF link and receiver CPU |
| `current` spikes low, `dropped` stays 0 | Transient jitter; jitter buffer absorbed it | Usually acceptable; increase `-l` if recurring |

**`dropped=0` with low average fps always means the problem is on the sender
side** — the receiver is faithfully displaying exactly what arrived, which
just isn't much. Confirm by watching the sender's CPU load or running the
GStreamer latency tracer on the sender.

#### Common mistakes

| Mistake | Symptom | Fix |
| --- | --- | --- |
| Missing `queue` after `tee` | Pipeline deadlocks or stutters | Add `queue` on every branch |
| `sync=true` on both sinks | One sink starves the other | Use `sync=false` on the non-primary sink |
| Forgetting `name=t` | Parse error: `tee` has no name to address | Always give the tee a name |
| Putting caps filter after `tee` | Negotiation fails on one branch | Apply caps *before* `tee`; branches share the same format |

### 2.8 Common caps types

Caps (capabilities) are the type system GStreamer uses between elements.
Every `!` link negotiates a matching caps type — if the types don't agree,
the pipeline fails with `not-negotiated`.

#### video/x-raw — uncompressed frames

The most common caps type. Carries raw pixel data between camera, converters,
and encoders.

```
video/x-raw, format=(string)I420, width=(int)1280, height=(int)720,
             framerate=(fraction)30/1
```

Key fields:

| Field | What it means |
| --- | --- |
| `format` | Pixel layout — see table below |
| `width`, `height` | Frame dimensions in pixels |
| `framerate` | Frames per second as a fraction |
| `colorimetry` | Colour space (BT.601, BT.709, etc.) — usually auto-negotiated |

Common `format` values:

| Format | Layout | Notes |
| --- | --- | --- |
| `I420` | YUV 4:2:0 planar | Required by x264enc / x265enc |
| `NV12` | YUV 4:2:0 semi-planar | Common from hardware decoders; U/V interleaved |
| `YUY2` / `YUYV` | YUV 4:2:2 packed | Default output of many CSI/USB cameras |
| `RGBA` | 8-bit per channel, alpha | Used by GL sinks after GPU colour conversion |
| `BGRx` | 8-bit BGR + padding | Common from some USB cameras |

`videoconvert` can convert between any of these, at a CPU cost proportional
to how different the layouts are. YUY2→I420 (chroma subsampling + unpack)
is heavier than NV12→I420 (plane reorder only).

#### image/jpeg — MJPEG frames

Compressed JPEG frames from USB cameras (UVC class) that use Motion JPEG mode.

```
image/jpeg, width=(int)1280, height=(int)720, framerate=(fraction)30/1
```

USB cameras use MJPEG at higher resolutions to stay within USB 2.0 bandwidth:
raw YUYV at 1280×720@30fps needs ~55 MB/s; MJPEG compresses each frame
independently so the same stream fits in ~5 MB/s.

Pipeline for MJPEG USB cameras:

```
v4l2src ! image/jpeg,width=1280,height=720,framerate=30/1 ! jpegdec ! videoconvert ! x264enc
```

`jpegdec` decodes each JPEG frame to `video/x-raw`. `videoconvert` then
converts the raw format (usually I420 or YUY2) to whatever the encoder needs.

#### video/x-h264 / video/x-h265 — encoded bitstream

Sits between the encoder and the RTP payloader. Carries compressed NAL units,
not raw pixels.

```
video/x-h264, stream-format=(string)byte-stream, alignment=(string)au
```

You rarely need to write these caps explicitly — GStreamer negotiates them
automatically between `x264enc` → `rtph264pay` and `h264parse` → `avdec_h264`.
The exception is `udpsrc`, which has no upstream to negotiate with (see below).

#### application/x-rtp — RTP packets

Used on `udpsrc` to tell GStreamer what kind of RTP payload is arriving,
because UDP carries no metadata about its contents.

```
application/x-rtp, media=video, clock-rate=90000,
                   encoding-name=H264, payload=96
```

| Field | Value | Why |
| --- | --- | --- |
| `media` | `video` | Distinguishes from audio RTP |
| `clock-rate` | `90000` | Standard 90 kHz RTP video clock |
| `encoding-name` | `H264` or `H265` | Selects the right depayloader |
| `payload` | `96` | Dynamic payload type; must match sender's `pt=96` |

Without this caps on `udpsrc`, GStreamer cannot pick `rtph264depay` vs
`rtph265depay` automatically and the pipeline fails.

#### video/x-bayer — raw sensor output

Carries raw Bayer pattern data directly from the image sensor before the
ISP processes it. Rarely used in streaming pipelines but appears in
`libcamerasrc`'s pad template because the element can optionally expose
the raw sensor path.

```
video/x-bayer, format=(string)rggb, width=(int)4056, height=(int)3040
```

In normal use, `libcamerasrc` runs the ISP and outputs `video/x-raw` (YUY2).
The Bayer path is only used when you want to do custom ISP processing.

#### Caps negotiation in practice

```
libcamerasrc                videoconvert              x264enc
    │                           │                        │
    │── video/x-raw (YUY2) ───►│── video/x-raw (I420) ─►│
    │    (what camera offers)   │   (what encoder needs)  │
```

Each link queries what the downstream element accepts, then picks a mutually
supported format. `videoconvert` accepts almost anything and converts to match
downstream. An explicit caps filter (`! video/x-raw,format=I420`) pins the
negotiation to one format and prevents surprises.

### 2.9 Transcoding and format conversion

#### Encoders only accept raw video

`x264enc` and `x265enc` are raw video encoders. They accept uncompressed pixel
data (`video/x-raw`) and output a compressed bitstream. They cannot accept
already-compressed input like MJPEG.

```
✗  v4l2src → image/jpeg ──────────────────────────► x264enc
✓  v4l2src → image/jpeg → jpegdec → video/x-raw → x264enc
```

Going from one compressed format to another always requires a full
**decode → encode** round trip:

```
MJPEG → jpegdec → video/x-raw → x264enc → H.264
         decode                   encode
```

This is called **transcoding**. It runs two codecs back-to-back and costs CPU
for both. On Pi 5 with the BRIO 100: `jpegdec` decodes each JPEG frame to
I420, then `x264enc ultrafast` encodes it — total cost is still manageable
because JPEG decode is fast and ultrafast encoding is light.

#### Avoiding transcode — the save path in receiver.c

When saving to file, receiver.c skips `avdec_h264` entirely:

```
# Display path (transcode: H.264 → raw → screen)
udpsrc → rtph264depay → h264parse → avdec_h264 → videoconvert → autovideosink

# Save path (no transcode: H.264 → MP4 container directly)
udpsrc → rtph264depay → h264parse → mp4mux → filesink
```

The bitstream is already H.264 — muxing it into MP4 is just wrapping it in a
container. Decoding then re-encoding would waste CPU and lose quality.

#### MJPEG direct over RTP vs transcode to H.264

USB cameras output MJPEG. Two options for streaming:

| | MJPEG over RTP | Transcode → H.264 over RTP |
| --- | --- | --- |
| Bandwidth | ~10–50 Mbps at 720p30 | ~2 Mbps at 720p30 |
| CPU (sender) | zero encode cost | jpegdec + x264enc |
| Latency | lowest (no encoder delay) | +~7 ms (ultrafast) |
| RF link fit | no — far exceeds link budget | yes |
| QGC / MAVLink | not standard | expected |
| Receiver | needs MJPEG decoder | standard H.264 decoder |

GStreamer does support MJPEG over RTP (`rtpjpegpay` / `rtpjpegdepay`), so
direct transmission is technically possible:

```bash
# MJPEG direct (LAN only — too much bandwidth for RF)
gst-launch-1.0 v4l2src \
  ! image/jpeg,width=1280,height=720,framerate=30/1 \
  ! rtpjpegpay ! udpsink host=192.168.1.1 port=5600
```

**For UAV RF links, always transcode to H.264.** RF links budget 2–10 Mbps
total; MJPEG would consume it all for video alone. H.264 at 2 Mbps gives
equivalent quality at 1/10 the bandwidth. The transcode CPU cost is a
worthwhile trade.

MJPEG direct only makes sense on a gigabit LAN or USB-tethered link where
bandwidth is not a constraint and you want to eliminate encoder latency.

---

## 3. H.264 and H.265 Encoding

### 3.1 Why compress?

Raw 1280×720 @ 30fps = `1280 × 720 × 3 bytes × 30 = ~83 MB/s`.
H.264 at 2 Mbps = ~250 KB/s. That's a **330× reduction**.

### 3.2 How compression works (conceptually)

**Intra-frame (I-frame / IDR):** Compresses one frame against itself.
Self-contained — a decoder can start from here.

**Inter-frame (P-frame):** Stores only the *difference* from the previous frame.
Much smaller, but requires the previous frame to decode.

**B-frame:** Difference from both previous AND next frame.
Best compression, but adds latency because the encoder must look ahead.

```
IDR  P  P  P  P  P  IDR  P  P  P  ...
 │   └──┘  └──┘      │
 └── can decode    └── new receivers start here
     from here
```

### 3.3 The encoding parameters in sender.c

```c
x264enc tune=zerolatency bitrate=2000 key-int-max=30 speed-preset=ultrafast
```

| Parameter | Value | Why |
| --- | --- | --- |
| `tune=zerolatency` | — | Disables B-frames and look-ahead. Every frame is encoded and emitted immediately. Without this, the encoder buffers several frames before outputting — unusable for a live UAV feed |
| `bitrate=2000` | 2000 kbps | Enough for 720p with good quality. Lower = more compression artifacts. Tune to your RF link capacity |
| `key-int-max=30` | 30 frames | Forces an IDR every 30 frames (~1 s at 30 fps). If the RF link drops for <1 s, the receiver recovers at the next IDR without manual intervention |
| `speed-preset=ultrafast` | — | Less CPU used for motion estimation. Important on embedded ARM (Jetson Nano, RPi). Slightly lower quality, but negligible for live video |

### 3.4 H.264 vs H.265

| | H.264 (AVC) | H.265 (HEVC) |
| --- | --- | --- |
| Compression | baseline | ~30–40% better at same quality |
| CPU cost | low | ~2× higher |
| Hardware support | universal | most modern chips |
| Latency | low | slightly higher |
| Use case | default choice | bandwidth-constrained links |

### 3.5 SPS/PPS — why config-interval matters

**SPS** (Sequence Parameter Set) and **PPS** (Picture Parameter Set) are headers
that describe the video format (resolution, profile, etc.). A decoder needs them
before it can decode any frame.

In normal video files, SPS/PPS are at the start of the file. In a live UDP stream
there is no "start" — a receiver that joins mid-stream would never see them.

`config-interval=1` tells `rtph264pay` to repeat SPS/PPS inline before every
IDR frame, so any receiver can start decoding within one key-frame interval.

---

## 4. RTP — Real-time Transport Protocol

### 4.1 What RTP is

RTP (RFC 3550) runs over UDP. It adds a small header to each packet:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─┤─┤─┤─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┤
│V=2│P│X│  CC   │M│    PT     │        Sequence Number           │
├───┴─┴─┴───────┴─┴───────────┴──────────────────────────────────┤
│                           Timestamp                            │
├────────────────────────────────────────────────────────────────┤
│                    Synchronization Source (SSRC)               │
├────────────────────────────────────────────────────────────────┤
│                         Payload (H.264 NAL units...)           │
```

Key fields:
- **Sequence number** — lets the receiver detect missing/reordered packets
- **Timestamp** — 90 kHz clock for video, used to schedule playout
- **PT (payload type)** — tells receiver what codec is inside. We use 96 (dynamic)

### 4.2 The GStreamer RTP chain

**Sender side:**
```
x264enc → [H.264 NAL units] → rtph264pay → [RTP packets] → udpsink
```

`rtph264pay` fragments large NAL units to fit in UDP MTU (~1400 bytes) and adds
the RTP header with incrementing sequence number and correct timestamp.

**Receiver side:**
```
udpsrc → [RTP packets] → rtpjitterbuffer → rtph264depay → [H.264 NAL units] → avdec_h264
```

`rtpjitterbuffer` collects packets, sorts by sequence number, and releases them
at the scheduled timestamp. The `latency=200` property means it will wait up to
200 ms for a late packet before giving up — this smooths out network jitter at
the cost of 200 ms added delay.

### 4.3 caps on udpsrc — why it's necessary

UDP has no connection handshake. When packets arrive at `udpsrc`, GStreamer
doesn't know what's inside. The `caps=` property tells it:

```c
"caps=\"application/x-rtp,media=video,clock-rate=90000,"
       "encoding-name=H264,payload=96\""
```

Without caps, GStreamer can't pick the right depayloader automatically.
(In RTSP, the DESCRIBE response provides this via SDP — no manual caps needed.)

### 4.4 RTCP (briefly)

RTCP is a companion protocol that runs alongside RTP (usually port+1).
It carries:
- **SR (Sender Report):** sender statistics (packets sent, bytes sent, NTP timestamp)
- **RR (Receiver Report):** receiver statistics (fraction lost, jitter, delay)

We don't implement RTCP in this demo, but a production system uses it to adapt
bitrate when the RF link degrades.

---

## 5. RTSP — Real-Time Streaming Protocol

### 5.1 RTP vs RTSP

| | RTP/UDP (sender.c) | RTSP (rtsp_server.c) |
| --- | --- | --- |
| Setup | Pre-configured port | Client negotiates via TCP |
| Discovery | Manual / MAVLink | DESCRIBE returns SDP |
| Multi-client | One sender → one port | Server multiplexes per client |
| Reconnect | Receiver just keeps listening | Client sends PLAY again |
| Overhead | Minimal | Small RTSP TCP exchange |

**Rule of thumb:** Use RTP/UDP for a dedicated ground station. Use RTSP when
multiple clients (laptop, tablet, QGC) need to connect on demand.

### 5.2 The RTSP handshake

```
Client                          Server
  │── DESCRIBE rtsp://…/uav ──►│  "what's available?"
  │◄── 200 OK + SDP ────────────│  codec, resolution, ports
  │── SETUP …/track1 ──────────►│  "allocate RTP session"
  │◄── 200 OK + Transport ──────│  actual RTP port assigned
  │── PLAY ────────────────────►│  "start sending"
  │◄── 200 OK ──────────────────│
  │◄══ RTP video packets ════════│  UDP stream begins
  │── TEARDOWN ────────────────►│  "stop"
```

### 5.3 How gst-rtsp-server works

```c
GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
gst_rtsp_media_factory_set_launch(factory,
    "( videotestsrc ! x264enc ! rtph264pay name=pay0 pt=96 )");
gst_rtsp_media_factory_set_shared(factory, TRUE);
gst_rtsp_mount_points_add_factory(mounts, "/uav", factory);
```

- The `(...)` wrapper marks this as a bin the server manages
- `name=pay0` is mandatory — it tells the server which element produces RTP
- `shared=TRUE` — one pipeline, many clients. `shared=FALSE` would spawn a
  separate encoder per client (useful for adaptive bitrate, wasteful for UAV)

When a client sends `PLAY`, the server calls `gst_rtsp_server_attach()` which
creates a socket source in the GLib main loop — no extra threads needed.

### 5.4 Connect to the RTSP server

```bash
# GStreamer
gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/uav latency=200 \
  ! rtph264depay ! h264parse ! avdec_h264 ! autovideosink

# FFmpeg
ffplay rtsp://127.0.0.1:8554/uav

# VLC
vlc rtsp://127.0.0.1:8554/uav
```

---

## 6. MAVLink Video Announcement

### 6.1 What MAVLink is

MAVLink is a lightweight binary protocol for UAV telemetry. It runs over
UDP (ground ↔ air), serial, or TCP. Every message is:

```
STX | LEN | SEQ | SYSID | COMPID | MSGID | PAYLOAD | CHECKSUM
```

Our code sends two message types:

**HEARTBEAT (#0):** "I am alive, I am a camera component."
Must be sent at 1 Hz or the GCS considers the component offline.

**VIDEO_STREAM_INFORMATION (#269):** Tells the GCS:
- Where is the stream (URI)
- What type (RTP UDP, RTSP, ...)
- What codec (H264, H265)
- Resolution, framerate, bitrate

### 6.2 Why this matters in practice

Without MAVLink announce:
1. Ground operator opens QGroundControl
2. Manually navigates to Video Settings
3. Types in `udp://0.0.0.0:5600`

With MAVLink announce:
1. Ground operator opens QGroundControl
2. Video appears automatically

### 6.3 The pack → buffer → sendto pattern

The MAVLink C library is **header-only** and entirely in-memory:

```c
mavlink_message_t msg;                         // message container
mavlink_msg_heartbeat_pack(                    // fill msg from fields
    sysid, compid, &msg, type, autopilot, ...);

uint8_t buf[MAVLINK_MAX_PACKET_LEN];
int len = mavlink_msg_to_send_buffer(buf, &msg);  // serialize to wire format
sendto(sock, buf, len, 0, &gcs_addr, sizeof(gcs_addr));
```

No dynamic allocation, no sockets inside the library — you own the transport.

### 6.4 The VIDEO_STREAM_TYPE vs encoding split

```
stream_type = VIDEO_STREAM_TYPE_RTPUDP    ← transport layer
encoding    = VIDEO_STREAM_ENCODING_H264  ← codec
```

`stream_type` describes *how* to connect (RTP over UDP, RTSP, etc.).
`encoding` describes *what codec* is inside the stream.
Separating them lets you have, e.g., H.265 over RTSP: `stream_type=RTSP, encoding=H265`.

---

## 7. Code Walkthrough

### File map

```
sender.c              — UAV side: encode + RTP/UDP send + MAVLink announce
receiver.c            — Ground side: RTP/UDP receive + decode + display/save
rtsp_server.c         — Alternative: RTSP server + MAVLink announce
inc/mavlink_announce.h — Public API: MavlinkVideoConfig struct + 3 functions
src/mavlink_announce.c — Implementation: UDP socket + MAVLink pack/send
CMakeLists.txt        — Build: 3 executables + 1 static lib
```

### sender.c — key sections

```c
// 1. Build encoder+payloader string based on codec flag
gchar *enc_pay = g_strdup_printf(
    "x264enc tune=zerolatency bitrate=%d key-int-max=30 speed-preset=ultrafast "
    "! rtph264pay config-interval=1 pt=96", bitrate);

// 2. Full pipeline: source → encode → RTP → UDP
gchar *desc = g_strdup_printf(
    "videotestsrc pattern=ball is-live=true "
    "! video/x-raw,width=%d,height=%d,framerate=%d/1 "
    "! videoconvert ! %s "
    "! udpsink host=%s port=%d sync=false async=false", ...);

// 3. Parse the string into a real pipeline object
GstElement *pipeline = gst_parse_launch(desc, &err);

// 4. Attach a bus watcher for errors/EOS
gst_bus_add_watch(bus, bus_cb, g_loop);

// 5. Start streaming
gst_element_set_state(pipeline, GST_STATE_PLAYING);

// 6. Announce over MAVLink at 1 Hz (optional, only if -g passed)
if (gcs_host) {
    mavlink_announce_init(&mav_cfg);
    g_timeout_add_seconds(1, mavlink_announce_tick, NULL);
}

g_main_loop_run(g_loop);  // blocks here
```

### receiver.c — key sections

```c
// Display path: decode video and render
"udpsrc port=%d caps=... ! rtpjitterbuffer latency=%d "
"! rtph264depay ! h264parse ! avdec_h264 "
"! videoconvert ! autovideosink sync=false"

// Save path: mux directly into MP4 without decode+re-encode
"udpsrc port=%d caps=... ! rtpjitterbuffer latency=%d "
"! rtph264depay ! h264parse "
"! mp4mux ! filesink location=%s"
```

Note: the save path skips `avdec_h264` entirely — the raw Annex-B bitstream
from `h264parse` goes straight to the muxer. This is more efficient and
preserves quality since there is no transcode step.

### mavlink_announce.c — key sections

```c
// init: open a UDP socket pointing at the GCS
g_sock = socket(AF_INET, SOCK_DGRAM, 0);
inet_pton(AF_INET, cfg->gcs_host, &g_gcs_addr.sin_addr);

// tick (called at 1 Hz by g_timeout_add_seconds):
static void send_heartbeat(void) {
    mavlink_message_t msg;
    mavlink_msg_heartbeat_pack(SYSID, COMPID, &msg,
        MAV_TYPE_CAMERA, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    // serialize and sendto(g_sock, ...)
}

static void send_video_stream_info(void) {
    mavlink_msg_video_stream_information_pack(SYSID, COMPID, &msg,
        stream_id, count, stream_type, flags,
        fps, width, height, bitrate_bps,
        rotation, hfov, name, uri,
        encoding, camera_device_id);   // extension fields last
}
```

### CMakeLists.txt — build design

```cmake
# mavlink_announce has no GStreamer dependency.
# Compile once, link into sender and rtsp_server.
add_library(mavlink_announce STATIC src/mavlink_announce.c)
target_include_directories(mavlink_announce PUBLIC
    inc              # makes "mavlink_announce.h" available to consumers
    third_party      # makes <mavlink/common/mavlink.h> available to consumers
    ${GLIB_INCLUDE_DIRS})

# PUBLIC include dirs propagate to any target that links mavlink_announce,
# so sender and rtsp_server automatically see inc/ and third_party/.
target_link_libraries(sender PRIVATE ${GST_LIBRARIES} mavlink_announce)
```

---

## 8. Hands-on Exercises

Work through these in order. Each builds on the previous.

### Exercise 1 — explore gst-launch (20 min)

```bash
# See what plugins are available
gst-inspect-1.0 | grep -i h264
gst-inspect-1.0 x264enc     # read all the properties

# Inspect a single element
gst-inspect-1.0 rtph264pay
gst-inspect-1.0 rtpjitterbuffer
```

### Exercise 2 — run sender + receiver (15 min)

```bash
# Terminal 1 — H.264 (-c 0), 2 Mbps, test pattern (-s 0)
./build/sender -h 127.0.0.1 -p 5600 -b 2000 -c 0

# Terminal 2 — save to file (works without a display)
./build/receiver -p 5600 -c h264 -s /tmp/test.mp4

# After a few seconds, Ctrl+C both.  Play the file:
ffplay /tmp/test.mp4
```

### Exercise 3 — switch codec to H.265 (5 min)

```bash
./build/sender   -c 1 -b 1200
./build/receiver -c h265 -s /tmp/test265.mp4
```

Observe: lower bitrate (`-b 1200` vs 2000), same resolution.

### Exercise 4 — RTSP server (15 min)

```bash
./build/rtsp_server -p 8554 -c 0

# From another terminal (or Windows)
ffplay rtsp://127.0.0.1:8554/uav
```

### Exercise 5b — tee: local FPS display alongside sending (10 min)

```bash
# Pass -d to open a local preview window showing pre-encode FPS
./build/sender -h 127.0.0.1 -p 5600 -b 2000 -d
```

A window opens showing the raw source frame rate via `fpsdisplaysink`.
The UDP stream continues unaffected on the other tee branch.

Try different speed presets and observe the FPS stability:

```bash
./build/sender -d -P 0   # ultrafast — lowest CPU
./build/sender -d -P 5   # medium — heavier CPU, may drop source FPS on RPi
```

### Exercise 6 — read the MAVLink message definition (10 min)

Open the installed header and find VIDEO_STREAM_INFORMATION:

```bash
find /home -path "*/mavlink/common/mavlink_msg_video_stream_information.h" | head -1 | xargs less
```

Read the struct `mavlink_video_stream_information_t` — note every field and
its size. Compare to the `MavlinkVideoConfig` struct in `inc/mavlink_announce.h`.

### Exercise 7 — adjust jitter buffer latency (10 min)

```bash
# Low latency (may drop frames on a lossy link)
./build/receiver -p 5600 -l 50

# High latency (smooth on a lossy link, noticeable delay)
./build/receiver -p 5600 -l 500
```

Observe the trade-off: lower latency = more likely to show glitches when
packets arrive slightly out of order.

---

## 9. Interview Talking Points

These are the answers to likely interview questions based on the JD.

**"What is the difference between RTP and RTSP?"**
> RTP carries the actual media data over UDP. RTSP is a control protocol over TCP
> that handles session setup (DESCRIBE/SETUP/PLAY). Think of RTSP like HTTP for
> video control, and RTP as the actual video channel. In a UAV link, we often skip
> RTSP and use raw RTP/UDP directly because there is no need for on-demand
> negotiation — both ends are pre-configured.

**"Why use tune=zerolatency in x264enc?"**
> Without it, x264 uses B-frames which require buffering future frames before
> encoding — adding hundreds of milliseconds of encoder-side latency. For a UAV
> where the pilot is reacting to what they see, every millisecond matters.
> zerolatency disables B-frames and look-ahead so each frame is encoded and
> emitted immediately.

**"How does a receiver recover after a link dropout?"**
> Two mechanisms: the jitter buffer (`rtpjitterbuffer`) handles short disruptions
> by masking reordering and holding the display for late packets. For longer
> outages, the receiver waits for the next IDR (key frame). We set
> `key-int-max=30` so an IDR appears every ~1 second, meaning the receiver
> recovers within one second of the link returning.

**"What is config-interval=1 in rtph264pay?"**
> It causes SPS and PPS — the codec configuration headers — to be repeated
> inline before every IDR frame. Without this, a receiver that joins mid-stream
> or reconnects after dropout would never see the codec configuration and couldn't
> decode the video. With it, any receiver that catches an IDR has everything
> it needs.

**"How does QGroundControl find the video stream?"**
> Via MAVLink `VIDEO_STREAM_INFORMATION` (message #269). The UAV companion
> computer sends this at 1 Hz alongside the heartbeat. It contains the stream
> URI (e.g. `udp://0.0.0.0:5600`), codec, resolution, and bitrate. QGC
> subscribes to this message and opens the video player automatically.

**"What is the difference between H.264 and H.265?"**
> H.265 (HEVC) achieves roughly the same visual quality at ~30–40% lower bitrate,
> which matters on bandwidth-constrained RF links. The cost is higher encoding
> CPU (~2×), which can be a constraint on small embedded boards. For a Jetson
> or any platform with hardware H.265 encoders, H.265 is always preferred.

**"How would you replace the test source with a real camera?"**
> Swap `videotestsrc` for `v4l2src device=/dev/video0` for a USB camera, or
> `nvarguscamerasrc` on NVIDIA Jetson. The rest of the pipeline is identical.
> On Jetson you'd also use `nvv4l2h264enc` instead of `x264enc` to get hardware
> encoding and much lower CPU load.
