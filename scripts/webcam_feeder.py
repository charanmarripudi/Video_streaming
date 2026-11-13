#!/usr/bin/env python3
"""
Webcam Feeder & Live ROI Preview Monitor
Industrial C++ Video Analytics Engine

Features:
1. Live Camera Window: Displays live webcam feed with FPS overlay.
2. Interactive ROI Drawing: Click points with the mouse to define your Region of Interest (ROI).
3. Auto-Save: Press 's' to save the ROI polygon directly into config.json!
4. Real-time C++ Streaming: Feeds live frames to the C++ engine via FIFO pipe.
"""

import os
import sys
import time
import json
import signal
import argparse
import numpy as np

def load_roi_from_config(config_path, camera_id="CAM_01_BAY_GANTRY"):
    try:
        if os.path.exists(config_path):
            with open(config_path, "r") as f:
                cfg = json.load(f)
            for cam in cfg.get("cameras", []):
                if cam.get("camera_id") == camera_id:
                    for uc in cam.get("assigned_use_cases", []):
                        if "roi_polygon" in uc and len(uc["roi_polygon"]) >= 3:
                            return [tuple(pt) for pt in uc["roi_polygon"]]
    except Exception as e:
        print(f"[PREVIEW] Note: Could not load ROI from config: {e}")
    return []

def save_roi_to_config(config_path, polygon, camera_id="CAM_01_BAY_GANTRY"):
    try:
        if not os.path.exists(config_path):
            return False
        with open(config_path, "r") as f:
            cfg = json.load(f)
        
        updated = False
        for cam in cfg.get("cameras", []):
            if cam.get("camera_id") == camera_id:
                for uc in cam.get("assigned_use_cases", []):
                    uc["roi_polygon"] = [[int(p[0]), int(p[1])] for p in polygon]
                    updated = True
        
        if updated:
            with open(config_path, "w") as f:
                json.dump(cfg, f, indent=2)
            print(f"\n[ROI-SAVER] Saved {len(polygon)} ROI points to {config_path} for {camera_id}!")
            return True
    except Exception as e:
        print(f"[ERROR] Failed to save ROI to config: {e}", file=sys.stderr)
    return False

def main():
    parser = argparse.ArgumentParser(description="Webcam Feeder & Live ROI Monitor")
    parser.add_argument("--device", type=int, default=0, help="Webcam device index (default: 0)")
    parser.add_argument("--width", type=int, default=640, help="Capture width (default: 640)")
    parser.add_argument("--height", type=int, default=480, help="Capture height (default: 480)")
    parser.add_argument("--fps", type=int, default=20, help="Target FPS (default: 20)")
    parser.add_argument("--pipe", type=str, default="/tmp/video_engine_webcam.pipe", help="FIFO pipe path")
    parser.add_argument("--config", type=str, default="config.json", help="Path to config.json")
    parser.add_argument("--preview", action="store_true", default=True, help="Show live OpenCV preview window")
    parser.add_argument("--no-preview", dest="preview", action="store_false", help="Disable preview window (headless)")
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

    # Load existing ROI from config if available
    roi_points = load_roi_from_config(args.config)
    if not roi_points:
        # Default gantry ROI box (central 70% zone)
        x_pad = int(actual_w * 0.15)
        y_pad = int(actual_h * 0.20)
        roi_points = [
            (x_pad, y_pad),
            (actual_w - x_pad, y_pad),
            (actual_w - x_pad, actual_h - y_pad),
            (x_pad, actual_h - y_pad)
        ]

    # Mouse Callback for interactive ROI drawing
    window_name = "Industrial C++ Engine - Live Camera & ROI Preview"
    drawing_points = list(roi_points)

    def on_mouse(event, x, y, flags, param):
        nonlocal drawing_points
        if event == cv2.EVENT_LBUTTONDOWN:
            drawing_points.append((x, y))
            print(f"[ROI-DRAW] Added Point #{len(drawing_points)}: ({x}, {y})")
        elif event == cv2.EVENT_RBUTTONDOWN:
            if drawing_points:
                removed = drawing_points.pop()
                print(f"[ROI-DRAW] Removed Point: {removed}")

    if args.preview:
        try:
            cv2.namedWindow(window_name, cv2.WINDOW_AUTOSIZE)
            cv2.setMouseCallback(window_name, on_mouse)
            print("[PREVIEW] Interactive ROI Window Active:")
            print("  -> Left Click  : Add polygon point")
            print("  -> Right Click : Undo last point")
            print("  -> Press 's'   : Save current ROI polygon to config.json")
            print("  -> Press 'r'   : Reset ROI points")
            print("  -> Press 'q'   : Quit")
        except Exception as e:
            print(f"[PREVIEW] GUI window unavailable, falling back to headless: {e}")
            args.preview = False

    pipe_fd = None
    running = True

    def sig_handler(sig, frame):
        nonlocal running
        print("\n[WEBCAM-FEEDER] Stopping feeder...")
        running = False

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    try:
        # Open FIFO for writing
        pipe_fd = os.open(args.pipe, os.O_WRONLY)
        print("[WEBCAM-FEEDER] C++ Video Engine connected! Streaming live webcam frames...")

        interval = 1.0 / args.fps
        frame_count = 0
        t0 = time.time()
        fps_display = args.fps

        while running:
            t_start = time.time()
            ret, frame = cap.read()
            if not ret:
                time.sleep(0.01)
                continue

            if frame.shape[1] != actual_w or frame.shape[0] != actual_h:
                frame = cv2.resize(frame, (actual_w, actual_h))

            # 1. Stream raw frame to C++ Engine via pipe
            raw_data = frame.tobytes()
            try:
                os.write(pipe_fd, raw_data)
                frame_count += 1
            except (BrokenPipeError, OSError):
                print("[WEBCAM-FEEDER] C++ engine disconnected from pipe. Exiting.")
                break

            # 2. Render Live Preview Window with ROI
            if args.preview:
                display = frame.copy()

                # Draw ROI Polygon with semi-transparent overlay
                if len(drawing_points) >= 3:
                    pts = np.array(drawing_points, np.int32).reshape((-1, 1, 2))
                    overlay = display.copy()
                    cv2.fillPoly(overlay, [pts], (0, 255, 128)) # Light green translucent fill
                    cv2.addWeighted(overlay, 0.25, display, 0.75, 0, display)
                    cv2.polylines(display, [pts], isClosed=True, color=(0, 255, 0), thickness=2)

                    # Label ROI centroid
                    cx = int(np.mean([p[0] for p in drawing_points]))
                    cy = int(np.mean([p[1] for p in drawing_points]))
                    cv2.putText(display, "ROI ZONE [MONITORED]", (max(10, cx - 80), max(25, cy)), 
                                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)
                elif len(drawing_points) > 0:
                    for pt in drawing_points:
                        cv2.circle(display, pt, 5, (0, 255, 255), -1)

                # Draw Heads-Up Information Banner
                cv2.rectangle(display, (0, 0), (actual_w, 32), (30, 30, 30), -1)
                cv2.putText(display, f"LIVE WEBCAM @ {fps_display:.1f} FPS | Frame #{frame_count} | ROI Points: {len(drawing_points)}", 
                            (10, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1)

                cv2.putText(display, "[Click: Draw ROI | 's': Save | 'r': Reset | 'q': Quit]", 
                            (10, actual_h - 12), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 220, 255), 1)

                cv2.imshow(window_name, display)
                key = cv2.waitKey(1) & 0xFF
                if key == ord('q'):
                    print("[PREVIEW] Quit requested by user.")
                    break
                elif key == ord('s'):
                    if len(drawing_points) >= 3:
                        save_roi_to_config(args.config, drawing_points)
                    else:
                        print("[WARNING] Please click at least 3 points to form a valid ROI polygon before saving.")
                elif key == ord('r'):
                    drawing_points.clear()
                    print("[ROI-DRAW] Cleared all ROI points. Click on window to draw new polygon.")

            # Rate limiter
            elapsed = time.time() - t_start
            sleep_time = interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

            if frame_count % (args.fps * 5) == 0:
                fps_display = frame_count / (time.time() - t0)
                print(f"[WEBCAM-FEEDER] Live Stream: {frame_count} frames ({fps_display:.1f} FPS)")

    finally:
        if pipe_fd is not None:
            try:
                os.close(pipe_fd)
            except OSError:
                pass
        cap.release()
        if args.preview:
            try:
                cv2.destroyAllWindows()
            except Exception:
                pass
        if os.path.exists(args.pipe):
            try:
                os.remove(args.pipe)
            except OSError:
                pass
        print("[WEBCAM-FEEDER] Cleaned up resources. Goodbye.")

if __name__ == "__main__":
    main()
