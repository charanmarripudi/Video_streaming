#!/usr/bin/env python3
"""
Webcam Feeder for Industrial C++ Video Analytics Engine
Captures live frames from laptop webcam (device 0) using OpenCV
and streams raw BGR frames into a named FIFO pipe for C++ ingestion.
"""

import os
import sys
import time
import signal
import argparse

def main():
    parser = argparse.ArgumentParser(description="Webcam Feeder for C++ Video Engine")
    parser.add_argument("--device", type=int, default=0, help="Webcam device index (default: 0)")
    parser.add_argument("--width", type=int, default=640, help="Capture width (default: 640)")
    parser.add_argument("--height", type=int, default=480, help="Capture height (default: 480)")
    parser.add_argument("--fps", type=int, default=20, help="Target FPS (default: 20)")
    parser.add_argument("--pipe", type=str, default="/tmp/video_engine_webcam.pipe", help="FIFO pipe path")
    args = parser.parse_args()

    try:
        import cv2
    except ImportError:
        print("[ERROR] OpenCV (cv2) is not installed. Please activate hpcl_env.", file=sys.stderr)
        sys.exit(1)

    # 1. Initialize FIFO Pipe
    if os.path.exists(args.pipe):
        try:
            os.remove(args.pipe)
        except OSError:
            pass

    try:
        os.mkfifo(args.pipe)
        print(f"[WEBCAM-FEEDER] Created FIFO pipe at: {args.pipe}")
    except OSError as e:
        print(f"[ERROR] Failed to create FIFO pipe {args.pipe}: {e}", file=sys.stderr)
        sys.exit(1)

    # 2. Open Laptop Webcam
    cap = cv2.VideoCapture(args.device)
    if not cap.isOpened():
        print(f"[ERROR] Cannot open webcam device {args.device}!", file=sys.stderr)
        sys.exit(1)

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    cap.set(cv2.CAP_PROP_FPS, args.fps)

    actual_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    frame_bytes = actual_w * actual_h * 3

    print(f"[WEBCAM-FEEDER] Webcam opened: {actual_w}x{actual_h} @ ~{args.fps} FPS")
    print(f"[WEBCAM-FEEDER] Frame size: {frame_bytes} bytes (BGR24)")
    print("[WEBCAM-FEEDER] Waiting for C++ Video Engine to connect to pipe...")

    # Open FIFO for writing (blocks until C++ engine opens it for reading)
    pipe_fd = None
    running = True

    def sig_handler(sig, frame):
        nonlocal running
        print("\n[WEBCAM-FEEDER] Stopping webcam feeder...")
        running = False

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    try:
        pipe_fd = os.open(args.pipe, os.O_WRONLY)
        print("[WEBCAM-FEEDER] C++ Video Engine connected! Streaming live webcam frames...")

        interval = 1.0 / args.fps
        frame_count = 0
        t0 = time.time()

        while running:
            t_start = time.time()
            ret, frame = cap.read()
            if not ret:
                print("[WARNING] Failed to grab frame from webcam", file=sys.stderr)
                time.sleep(0.01)
                continue

            # Resize if webcam resolution doesn't match requested
            if frame.shape[1] != actual_w or frame.shape[0] != actual_h:
                frame = cv2.resize(frame, (actual_w, actual_h))

            # Write raw BGR bytes to pipe
            raw_data = frame.tobytes()
            try:
                os.write(pipe_fd, raw_data)
                frame_count += 1
            except (BrokenPipeError, OSError):
                print("[WEBCAM-FEEDER] C++ engine disconnected from pipe. Exiting.")
                break

            # Rate limiter to maintain target FPS
            elapsed = time.time() - t_start
            sleep_time = interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

            if frame_count % (args.fps * 5) == 0:
                fps_actual = frame_count / (time.time() - t0)
                print(f"[WEBCAM-FEEDER] Pushed {frame_count} frames to C++ Engine ({fps_actual:.1f} FPS)")

    finally:
        if pipe_fd is not None:
            try:
                os.close(pipe_fd)
            except OSError:
                pass
        cap.release()
        if os.path.exists(args.pipe):
            try:
                os.remove(args.pipe)
            except OSError:
                pass
        print("[WEBCAM-FEEDER] Cleaned up resources. Goodbye.")

if __name__ == "__main__":
    main()
