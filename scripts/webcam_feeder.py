#!/usr/bin/env python3
"""
Webcam Feeder, Live ONNX Object Detector & ROI Monitor
Industrial C++ Video Analytics Engine

Upgrades:
1. PLAIN CAMERA VIEW: Removed green translucent tint completely for clean natural video.
2. LIVE ONNX DETECTION: Runs YOLOv8n ONNX model in real-time (person, truck, vehicle, etc.).
3. ROI VIOLATION CHECK: Flags RED bounding box when detected objects enter the ROI zone!
4. INTERACTIVE ROI: Click to draw ROI points, press 's' to save directly to config.json.
5. C++ PIPE STREAMING: Feeds frames to C++ engine for pre/post-event recording.
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
    parser = argparse.ArgumentParser(description="Webcam Feeder, Live AI Detector & ROI Monitor")
    parser.add_argument("--device", type=int, default=0, help="Webcam device index (default: 0)")
    parser.add_argument("--width", type=int, default=640, help="Capture width (default: 640)")
    parser.add_argument("--height", type=int, default=480, help="Capture height (default: 480)")
    parser.add_argument("--fps", type=int, default=20, help="Target FPS (default: 20)")
    parser.add_argument("--pipe", type=str, default="/tmp/video_engine_webcam.pipe", help="FIFO pipe path")
    parser.add_argument("--config", type=str, default="config.json", help="Path to config.json")
    parser.add_argument("--model", type=str, default="models/yolov8n.onnx", help="Path to ONNX model")
    parser.add_argument("--preview", action="store_true", default=True, help="Show live OpenCV preview window")
    parser.add_argument("--no-preview", dest="preview", action="store_false", help="Disable live preview window")
    args = parser.parse_args()

    try:
        import cv2
    except ImportError:
        print("[ERROR] OpenCV (cv2) is not installed. Please activate hpcl_env.", file=sys.stderr)
        sys.exit(1)

    # 1. Load Live ONNX Object Detection Model
    detector = None
    if os.path.exists(args.model):
        try:
            from ultralytics import YOLO
            print(f"[AI-DETECTOR] Loading ONNX model for live detection: {args.model}...")
            detector = YOLO(args.model, task="detect")
            print("[AI-DETECTOR] ONNX Detection Model loaded successfully!")
        except Exception as e:
            print(f"[WARNING] Could not load Ultralytics ONNX detector: {e}")
    else:
        print(f"[WARNING] Model file {args.model} not found. Running preview without AI overlay.")

    # 2. Initialize FIFO Pipe
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

    # 3. Open Laptop Webcam
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

    # Load existing ROI from config
    roi_points = load_roi_from_config(args.config)
    if not roi_points:
        # Default central monitoring zone
        x_pad = int(actual_w * 0.15)
        y_pad = int(actual_h * 0.20)
        roi_points = [
            (x_pad, y_pad),
            (actual_w - x_pad, y_pad),
            (actual_w - x_pad, actual_h - y_pad),
            (x_pad, actual_h - y_pad)
        ]

    window_name = "Industrial C++ Engine - Live Camera & AI Detection"
    drawing_points = list(roi_points)
    show_roi = True

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
            print("[PREVIEW] Interactive Live AI Window Active:")
            print("  -> Video View  : PLAIN natural video (no green tint)")
            print("  -> Live AI     : Detecting person, truck, car, etc.")
            print("  -> Left Click  : Add ROI polygon point")
            print("  -> Right Click : Undo last point")
            print("  -> Press 's'   : Save current ROI polygon to config.json")
            print("  -> Press 'r'   : Reset ROI points")
            print("  -> Press 'h'   : Toggle ROI boundary visibility on/off")
            print("  -> Press 'q'   : Quit")
        except Exception as e:
            print(f"[PREVIEW] GUI window unavailable: {e}")
            args.preview = False

    pipe_fd = None
    running = True
    last_alert_time = 0.0

    def sig_handler(sig, frame):
        nonlocal running
        print("\n[WEBCAM-FEEDER] Stopping feeder...")
        running = False

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    try:
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

            # 1. Stream raw uncompressed frame to C++ Engine via pipe
            raw_data = frame.tobytes()
            try:
                os.write(pipe_fd, raw_data)
                frame_count += 1
            except (BrokenPipeError, OSError):
                print("[WEBCAM-FEEDER] C++ engine disconnected from pipe. Exiting.")
                break

            # 2. Render Live Preview Window with AI Detection (PLAIN view, NO green tint)
            if args.preview:
                display = frame.copy()
                roi_poly_np = np.array(drawing_points, np.int32).reshape((-1, 1, 2)) if len(drawing_points) >= 3 else None

                # Live AI Detection using ONNX model
                alert_in_roi = False
                detected_count = 0

                if detector is not None and frame_count % 2 == 0: # Run inference every 2 frames for smooth 25+ FPS
                    try:
                        results = detector(frame, verbose=False, conf=0.35)
                        if len(results) > 0 and results[0].boxes is not None:
                            boxes = results[0].boxes
                            detected_count = len(boxes)

                            for b in boxes:
                                xyxy = b.xyxy[0].cpu().numpy().astype(int)
                                x1, y1, x2, y2 = xyxy
                                conf = float(b.conf[0].cpu().numpy())
                                cls_id = int(b.cls[0].cpu().numpy())
                                label = detector.names.get(cls_id, f"obj_{cls_id}")

                                # Check if detection centroid is inside ROI polygon
                                cx = int((x1 + x2) / 2)
                                cy = int(y2) # Foot/ground contact point
                                in_roi = False
                                if roi_poly_np is not None:
                                    dist = cv2.pointPolygonTest(roi_poly_np, (float(cx), float(cy)), False)
                                    in_roi = (dist >= 0)

                                if in_roi:
                                    alert_in_roi = True
                                    box_color = (0, 0, 255)
                                    tag = f"ALERT: {label} {conf*100:.0f}% [IN ROI]"

                                    # Emit real violation trigger to C++ engine (with 20s cooldown)
                                    now = time.time()
                                    if now - last_alert_time > 20.0:
                                        last_alert_time = now
                                        alert_payload = {
                                            "camera_id": "CAM_01_BAY_GANTRY",
                                            "use_case_id": "tt_crew_entering_below_tt",
                                            "label": label,
                                            "score": float(conf),
                                            "boxes": [[float(x1), float(y1), float(x2), float(y2)]]
                                        }
                                        try:
                                            with open("/tmp/video_engine_alert.json", "w") as f_alert:
                                                json.dump(alert_payload, f_alert)
                                            print(f"\n[ALERT-TRIGGER] >>> REAL VIOLATION IN ROI: {label} ({conf*100:.0f}%) -> Sent to C++ Engine! <<<")
                                        except Exception:
                                            pass
                                else:
                                    # Normal Detection: Draw clean CYAN box
                                    box_color = (255, 200, 0)
                                    tag = f"{label} {conf*100:.0f}%"

                                cv2.rectangle(display, (x1, y1), (x2, y2), box_color, 2)
                                # Tag label banner
                                (tw, th), _ = cv2.getTextSize(tag, cv2.FONT_HERSHEY_SIMPLEX, 0.45, 1)
                                cv2.rectangle(display, (x1, max(0, y1 - th - 6)), (x1 + tw + 6, y1), box_color, -1)
                                cv2.putText(display, tag, (x1 + 3, max(12, y1 - 4)), 
                                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 0, 0) if not in_roi else (255, 255, 255), 1)
                    except Exception as e:
                        pass

                # Draw Plain ROI boundary (Thin 1px line, NO green translucent fill!)
                if show_roi and roi_poly_np is not None:
                    # Clean, thin yellow/cyan boundary line
                    line_color = (0, 0, 255) if alert_in_roi else (0, 255, 255)
                    cv2.polylines(display, [roi_poly_np], isClosed=True, color=line_color, thickness=2)
                    
                    # Corner anchor dots
                    for pt in drawing_points:
                        cv2.circle(display, pt, 4, line_color, -1)

                    # Subtle text tag on top corner
                    cv2.putText(display, "MONITORED ROI ZONE", (drawing_points[0][0] + 5, drawing_points[0][1] + 18), 
                                cv2.FONT_HERSHEY_SIMPLEX, 0.45, line_color, 1)

                # Draw Modern HUD Banner
                banner_color = (0, 0, 160) if alert_in_roi else (25, 25, 25)
                cv2.rectangle(display, (0, 0), (actual_w, 28), banner_color, -1)
                hud_text = f"LIVE WEBCAM @ {fps_display:.1f} FPS | Detections: {detected_count}"
                if alert_in_roi:
                    hud_text += " | *** VIOLATION TRIGGERED IN ROI! ***"
                cv2.putText(display, hud_text, (10, 19), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1)

                # Bottom control guide
                cv2.putText(display, "[Click: Draw ROI | 's': Save | 'r': Reset | 'h': Toggle ROI | 'q': Quit]", 
                            (10, actual_h - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.40, (0, 220, 255), 1)

                cv2.imshow(window_name, display)
                key = cv2.waitKey(1) & 0xFF
                if key == ord('q'):
                    print("[PREVIEW] Quit requested by user.")
                    break
                elif key == ord('s'):
                    if len(drawing_points) >= 3:
                        save_roi_to_config(args.config, drawing_points)
                    else:
                        print("[WARNING] Click at least 3 points to form an ROI polygon.")
                elif key == ord('r'):
                    drawing_points.clear()
                    print("[ROI-DRAW] Cleared all ROI points.")
                elif key == ord('h'):
                    show_roi = not show_roi
                    print(f"[PREVIEW] ROI boundary visibility: {'ON' if show_roi else 'OFF'}")

            # Rate limiter
            elapsed = time.time() - t_start
            sleep_time = interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

            if frame_count % (args.fps * 5) == 0:
                fps_display = frame_count / (time.time() - t0)
                print(f"[WEBCAM-FEEDER] Streaming @ {fps_display:.1f} FPS (Detections: {detected_count}, Alert: {alert_in_roi})")

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
