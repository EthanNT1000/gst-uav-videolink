#!/usr/bin/env python3
"""
Announce the GStreamer video stream over MAVLink so that QGroundControl
and other MAVLink-compatible ground control software can auto-discover
and display the feed without manual URL configuration.

Sends VIDEO_STREAM_INFORMATION (#269) at 1 Hz and HEARTBEAT (#0) at 1 Hz
on the standard GCS port.  Run this alongside sender or rtsp_server.

MAVLink video stream flow:
    UAV companion computer
      ├── sender / rtsp_server  →  video packets  →  GCS
      └── mavlink_announce.py  →  MAVLink UDP     →  GCS (auto-discovers stream URI)

Usage:
    # Announce an RTP/UDP stream (sender binary)
    python3 mavlink_announce.py --uri udp://0.0.0.0:5600 --type udp

    # Announce the RTSP server
    python3 mavlink_announce.py --uri rtsp://192.168.1.100:8554/uav --type rtsp

Dependencies:
    pip install pymavlink
"""

import argparse
import time
from pymavlink import mavutil

SYSID     = 1    # MAVLink system ID of the UAV
COMPID    = 100  # MAVLink component ID — camera component

def build_connection(gcs_addr: str):
    return mavutil.mavlink_connection(
        f"udpout:{gcs_addr}",
        source_system=SYSID,
        source_component=COMPID,
    )

def send_heartbeat(mav):
    mav.mav.heartbeat_send(
        mavutil.mavlink.MAV_TYPE_CAMERA,
        mavutil.mavlink.MAV_AUTOPILOT_INVALID,
        0, 0,
        mavutil.mavlink.MAV_STATE_ACTIVE,
    )

def send_video_stream_info(mav, uri: str, stream_type: int,
                            fps: float, width: int, height: int, bitrate: int):
    mav.mav.video_stream_information_send(
        1,            # stream_id (1-based)
        1,            # count — total number of streams on this camera
        stream_type,
        mavutil.mavlink.VIDEO_STREAM_STATUS_FLAGS_RUNNING,
        fps,
        width,
        height,
        bitrate * 1000,   # bps
        0,                # rotation degrees
        0,                # horizontal FOV (0 = unknown)
        b"UAV Camera\x00",
        uri.encode() + b"\x00",
    )

def main():
    ap = argparse.ArgumentParser(description="MAVLink video stream announcer")
    ap.add_argument("--gcs",     default="127.0.0.1:14550",
                    help="GCS address (default: 127.0.0.1:14550)")
    ap.add_argument("--uri",     default="udp://0.0.0.0:5600",
                    help="Video stream URI")
    ap.add_argument("--type",    choices=["udp", "rtsp"], default="udp",
                    help="Stream protocol (default: udp)")
    ap.add_argument("--fps",     type=float, default=30.0)
    ap.add_argument("--width",   type=int,   default=1280)
    ap.add_argument("--height",  type=int,   default=720)
    ap.add_argument("--bitrate", type=int,   default=2000, help="kbps")
    args = ap.parse_args()

    stream_type = (mavutil.mavlink.VIDEO_STREAM_TYPE_RTSP
                   if args.type == "rtsp"
                   else mavutil.mavlink.VIDEO_STREAM_TYPE_UDP_H264)

    mav = build_connection(args.gcs)
    print(f"Announcing {args.uri} → MAVLink GCS at {args.gcs}")
    print("Press Ctrl+C to stop.\n")

    try:
        while True:
            send_heartbeat(mav)
            send_video_stream_info(mav, args.uri, stream_type,
                                   args.fps, args.width, args.height,
                                   args.bitrate)
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nStopped.")

if __name__ == "__main__":
    main()
