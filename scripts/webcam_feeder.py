#!/usr/bin/env python3
"""
Webcam Feeder, Live ONNX Object Detector & ROI Monitor
Industrial C++ Video Analytics Engine

Key Upgrades:
1. TARGET MODEL: Uses models/intrusion.onnx exclusively for intrusion monitoring.
2. ZERO DETECTION FLICKERING / BLINKING: Persistent detection box caching ensures
   smooth continuous bounding boxes rendered on every frame at full FPS.
3. HIGH-SPEED TCP STREAMING: Direct localhost TCP streaming to C++ Video Engine
   (tcp://127.0.0.1:9876) with zero drops, zero OS buffer limits, and zero desync.
4. STRICT CLASS FILTERING: Only tracks configured classes (e.g., 'person').
   Furniture, chairs, TVs, and background objects are 100% ignored.
5. ROI INTRUSION MONITORING: Triggers bright RED alert only when a person enters
   the user-defined ROI polygon. Emits real-time alert trigger to C++ engine.
6. NATURAL PLAIN VIDEO: Clean camera video without any green or tinted wash.
7. PERSISTENT ROI DRAWING: Interactive mouse clicking to draw or refine ROI zone,
   saved directly to config.json.
"""

import os
import sys
import time
import json
import signal
import socket
import argparse
import numpy as np

def load_camera_use_case_config(config_path, camera_id="CAM_01_BAY_GANTRY"):
    cfg_data = {
        "roi_polygon": [],
        "model_ref": "models/intrusion.onnx",
        "threshold": 0.50,
        "enabled_classes": ["person"],
        "use_case_id": "intrusion",
        "fps": 20,
        "width": 640,
        "height": 480,
        "pre_event_s": 15
    }
    try:
        if os.path.exists(config_path):
            with open(config_path, "r") as f:
                cfg = json.load(f)
            for cam in cfg.get("cameras", []):
                if cam.get("camera_id") == camera_id:
                    cfg_data["fps"] = cam.get("fps", 20)
                    cfg_data["pre_event_s"] = cam.get("event_config", {}).get("pre_event_s", 15)
                    res = cam.get("capture_resolution", {})
                    cfg_data["width"] = res.get("width", 640)
                    cfg_data["height"] = res.get("height", 480)
                    for uc in cam.get("assigned_use_cases", []):
                        if uc.get("enabled", True):
                            cfg_data["use_case_id"] = uc.get("use_case_id", "intrusion")
                            cfg_data["model_ref"] = uc.get("model_ref", "models/intrusion.onnx")
                            cfg_data["threshold"] = float(uc.get("threshold", 0.50))
                            if "enabled_classes" in uc:
                                cfg_data["enabled_classes"] = uc["enabled_classes"]
                            if "roi_polygon" in uc and len(uc["roi_polygon"]) >= 3:
                                cfg_data["roi_polygon"] = [tuple(pt) for pt in uc["roi_polygon"]]
                            break
    except Exception as e:
        print(f"[PREVIEW] Note: Could not load config: {e}")
    return cfg_data

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
    parser.add_argument("--tcp-port", type=int, default=9876, help="C++ Engine TCP port (default: 9876)")
    parser.add_argument("--config", type=str, default="config.json", help="Path to config.json")
    parser.add_argument("--model", type=str, default="models/intrusion.onnx", help="Path to ONNX model (default: models/intrusion.onnx)")
    parser.add_argument("--threshold", type=float, default=None, help="Confidence threshold (e.g. 0.50)")
    parser.add_argument("--arming-delay", type=float, default=None, help="Initial arming grace period in seconds to buffer pre-event history (default: pre_event_s from config)")
    parser.add_argument("--preview", action="store_true", default=True, help="Show live OpenCV preview window")
    parser.add_argument("--no-preview", dest="preview", action="store_false", help="Disable live preview window")
    args = parser.parse_args()

    try:
        import cv2
    except ImportError:
        print("[ERROR] OpenCV (cv2) is not installed. Please activate hpcl_env.", file=sys.stderr)
        sys.exit(1)

    # 1. Load Camera & Use-Case Configuration from config.json
    cfg_data = load_camera_use_case_config(args.config)
    model_path = args.model if args.model != "models/intrusion.onnx" else cfg_data.get("model_ref", "models/intrusion.onnx")
    conf_threshold = args.threshold if args.threshold is not None else cfg_data.get("threshold", 0.50)
    enabled_classes = cfg_data.get("enabled_classes", ["person"])
    use_case_id = cfg_data.get("use_case_id", "intrusion")
    camera_id = "CAM_01_BAY_GANTRY"
    arming_delay = args.arming_delay if args.arming_delay is not None else float(cfg_data.get("pre_event_s", 15.0))

    print("=======================================================")
    print("      INDUSTRIAL C++ VIDEO ENGINE - WEBCAM FEEDER      ")
    print("=======================================================")
    print(f"  -> Model Path       : {model_path}")
    print(f"  -> Use Case ID      : {use_case_id}")
    print(f"  -> Enabled Classes  : {enabled_classes} (All other objects ignored)")
    print(f"  -> Min Confidence   : {conf_threshold:.2f}")
    print(f"  -> Arming Delay     : {arming_delay:.1f}s (Pre-event buffer accumulation)")
    print(f"  -> Resolution       : {args.width}x{args.height} @ {args.fps} FPS")
    print(f"  -> Video Stream Sink: tcp://127.0.0.1:{args.tcp_port}")
    print("=======================================================")

    # 2. Load Live ONNX Object Detection Model (intrusion.onnx)
    detector = None
    if os.path.exists(model_path):
        try:
            from ultralytics import YOLO
            print(f"[AI-DETECTOR] Loading model for intrusion monitoring: {model_path}...")
            detector = YOLO(model_path, task="detect")
            print(f"[AI-DETECTOR] ONNX Detection Model loaded successfully!")
        except Exception as e:
            print(f"[WARNING] Could not load Ultralytics ONNX detector: {e}")
    else:
        print(f"[WARNING] Model file {model_path} not found. Running preview without AI overlay.")

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

    # Warm up camera sensor to calibrate auto-exposure and white balance
    print("[WEBCAM-FEEDER] Warming up camera sensor (auto-exposure)...")
    for _ in range(8):
        cap.read()
        time.sleep(0.03)

    # Load existing ROI points from config
    roi_points = cfg_data.get("roi_polygon", [])
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
            print("\n[PREVIEW] Interactive Live AI Window Active:")
            print("  -> Video View  : Plain natural video (no tint)")
            print(f"  -> Model       : {os.path.basename(model_path)}")
            print(f"  -> Filter      : Only tracking {enabled_classes}")
            print("  -> Left Click  : Add ROI polygon point")
            print("  -> Right Click : Undo last point")
            print("  -> Press 's'   : Save current ROI polygon to config.json")
            print("  -> Press 'r'   : Reset ROI points")
            print("  -> Press 'h'   : Toggle ROI boundary visibility on/off")
            print("  -> Press 'q'   : Quit\n")
        except Exception as e:
            print(f"[PREVIEW] GUI window unavailable: {e}")
            args.preview = False

    running = True
    last_alert_time = 0.0

    # Persistent Detection Cache to ELIMINATE BLINKING / FLICKERING completely
    cached_detections = []

    # High-speed TCP stream client
    stream_sock = None
    last_connect_attempt = 0.0

    def get_socket():
        nonlocal stream_sock, last_connect_attempt
        if stream_sock is not None:
            return stream_sock
        now = time.time()
        if now - last_connect_attempt < 1.0: # Retry at most once per second
            return None
        last_connect_attempt = now
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.connect(("127.0.0.1", args.tcp_port))
            print(f"[WEBCAM-FEEDER] Connected to C++ Video Engine on tcp://127.0.0.1:{args.tcp_port}!")
            stream_sock = s
            return stream_sock
        except (ConnectionRefusedError, OSError):
            return None

    def sig_handler(sig, frame):
        nonlocal running
        print("\n[WEBCAM-FEEDER] Stopping feeder...")
        running = False

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    try:
        interval = 1.0 / args.fps
        frame_count = 0
        t0 = time.time()
        feeder_start_time = time.time()
        fps_display = args.fps
        detected_count = 0
        alert_in_roi = False

        while running:
            t_start = time.time()
            now = time.time()
            elapsed_since_start = now - feeder_start_time
            is_armed = (elapsed_since_start >= arming_delay)
            rem_arming_s = max(0, int(arming_delay - elapsed_since_start) + 1)
            ret, frame = cap.read()
            if not ret:
                time.sleep(0.01)
                continue

            if frame.shape[1] != actual_w or frame.shape[0] != actual_h:
                frame = cv2.resize(frame, (actual_w, actual_h))

            # 1. Prepare Annotated AI Display Frame (Boxes, ROI, HUD Banner)
            display = frame.copy()
            roi_poly_np = np.array(drawing_points, np.int32).reshape((-1, 1, 2)) if len(drawing_points) >= 3 else None

            # Run Inference periodically (every 2 frames) to conserve CPU while updating cache
            if detector is not None and frame_count % 2 == 0:
                try:
                    results = detector(frame, verbose=False, conf=conf_threshold)
                    new_detections = []
                    if len(results) > 0 and results[0].boxes is not None:
                        boxes = results[0].boxes
                        for b in boxes:
                            cls_id = int(b.cls[0].cpu().numpy())
                            label = detector.names.get(cls_id, f"obj_{cls_id}")

                            # STRICT CLASS FILTER: Ignore chair, tv, cup, etc. Only track enabled_classes
                            if enabled_classes and label not in enabled_classes:
                                continue

                            conf = float(b.conf[0].cpu().numpy())
                            if conf < conf_threshold:
                                continue

                            xyxy = b.xyxy[0].cpu().numpy().astype(int)
                            x1, y1, x2, y2 = xyxy

                            # Centroid for ROI calculation
                            cx = int((x1 + x2) / 2)
                            cy = int((y1 + y2) / 2)

                            in_roi = False
                            if roi_poly_np is not None:
                                dist = cv2.pointPolygonTest(roi_poly_np, (float(cx), float(cy)), False)
                                in_roi = (dist >= 0)

                            new_detections.append({
                                "box": (x1, y1, x2, y2),
                                "label": label,
                                "conf": conf,
                                "in_roi": in_roi,
                                "cx": cx,
                                "cy": cy,
                                "updated_at": time.time()
                            })
                    cached_detections = new_detections
                except Exception:
                    pass

            # Filter cached detections (retain active ones up to 0.6s to eliminate flickering)
            now = time.time()
            active_detections = [d for d in cached_detections if (now - d.get("updated_at", now)) < 0.6]

            # Re-evaluate ROI on cached detections in case drawing points change
            any_in_roi = False
            for d in active_detections:
                if roi_poly_np is not None:
                    d["in_roi"] = (cv2.pointPolygonTest(roi_poly_np, (float(d["cx"]), float(d["cy"])), False) >= 0)
                if d["in_roi"]:
                    any_in_roi = True

            alert_in_roi = any_in_roi and is_armed
            detected_count = len(active_detections)

            # RENDER SMOOTH BOUNDING BOXES ON EVERY FRAME (NO BLINKING)
            for det in active_detections:
                x1, y1, x2, y2 = det["box"]
                label = det["label"]
                conf = det["conf"]
                in_roi = det["in_roi"]

                if in_roi and not is_armed:
                    # Grace period: buffering pre-event frames
                    box_color = (0, 165, 255)  # Amber
                    tag = f"[ARMING: {rem_arming_s}s] {label} {conf*100:.0f}% IN ROI"
                    text_color = (255, 255, 255)
                elif in_roi and is_armed:
                    box_color = (0, 0, 255)  # Bright RED for Intrusion
                    tag = f"ALERT: {label} {conf*100:.0f}% [INTRUSION IN ROI]"
                    text_color = (255, 255, 255)

                    # Emit real violation trigger to C++ engine (with 20s cooldown)
                    if now - last_alert_time > 20.0:
                        last_alert_time = now
                        alert_payload = {
                            "camera_id": camera_id,
                            "use_case_id": use_case_id,
                            "label": label,
                            "score": float(conf),
                            "boxes": [[float(x1), float(y1), float(x2), float(y2)]]
                        }
                        try:
                            with open("/tmp/video_engine_alert.json", "w") as f_alert:
                                json.dump(alert_payload, f_alert)
                            print(f"\n[ALERT-TRIGGER] >>> REAL INTRUSION VIOLATION: {label} ({conf*100:.0f}%) in ROI -> Sent to C++ Engine! <<<")
                        except Exception:
                            pass
                else:
                    # Clean CYAN box for person outside monitored zone
                    box_color = (255, 200, 0)
                    tag = f"{label} {conf*100:.0f}%"
                    text_color = (0, 0, 0)

                # Draw box and label banner
                cv2.rectangle(display, (x1, y1), (x2, y2), box_color, 2)
                (tw, th), _ = cv2.getTextSize(tag, cv2.FONT_HERSHEY_SIMPLEX, 0.45, 1)
                cv2.rectangle(display, (x1, max(0, y1 - th - 6)), (x1 + tw + 6, y1), box_color, -1)
                cv2.putText(display, tag, (x1 + 3, max(12, y1 - 4)), 
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, text_color, 1)

            # Draw Plain ROI boundary (Thin 2px line, NO translucent tint!)
            if show_roi and roi_poly_np is not None:
                if alert_in_roi:
                    line_color = (0, 0, 255)
                    roi_tag = "*** INTRUSION ACTIVE ***"
                elif any_in_roi and not is_armed:
                    line_color = (0, 165, 255)
                    roi_tag = f"ROI ZONE [ARMING: {rem_arming_s}s]"
                else:
                    line_color = (0, 255, 255)
                    roi_tag = "MONITORED ROI ZONE"

                cv2.polylines(display, [roi_poly_np], isClosed=True, color=line_color, thickness=2)
                
                # Corner anchor dots
                for pt in drawing_points:
                    cv2.circle(display, pt, 4, line_color, -1)

                cv2.putText(display, roi_tag, (drawing_points[0][0] + 5, drawing_points[0][1] + 18), 
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, line_color, 1)

            # Draw Modern HUD Banner
            banner_color = (0, 0, 160) if alert_in_roi else ((30, 70, 120) if not is_armed else (25, 25, 25))
            cv2.rectangle(display, (0, 0), (actual_w, 28), banner_color, -1)
            connected_str = "[STREAMING TO C++ ENGINE]" if stream_sock is not None else "[WAITING FOR C++ ENGINE]"
            if alert_in_roi:
                hud_text = f"LIVE WEBCAM @ {fps_display:.1f} FPS | {connected_str} | *** INTRUSION IN ROI! ***"
            elif not is_armed:
                hud_text = f"LIVE WEBCAM @ {fps_display:.1f} FPS | {connected_str} | [PRE-EVENT BUFFERING: ARMED IN {rem_arming_s}s]"
            else:
                hud_text = f"LIVE WEBCAM @ {fps_display:.1f} FPS | {connected_str} | Filter: {','.join(enabled_classes)} | Count: {detected_count}"
            cv2.putText(display, hud_text, (10, 19), cv2.FONT_HERSHEY_SIMPLEX, 0.40, (255, 255, 255), 1)

            # 2. Stream Annotated Frame to C++ Video Engine (Records video with bounding boxes!)
            stream_data = display.tobytes()
            s = get_socket()
            if s is not None:
                try:
                    s.sendall(stream_data)
                    frame_count += 1
                except (BrokenPipeError, ConnectionResetError, OSError):
                    print("[WEBCAM-FEEDER] C++ engine disconnected. Waiting to reconnect...")
                    try:
                        s.close()
                    except Exception:
                        pass
                    stream_sock = None
            else:
                frame_count += 1

            # 3. Render GUI Window (if preview enabled)
            if args.preview:
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
                status = "Connected" if stream_sock is not None else "Standby"
                print(f"[WEBCAM-FEEDER] Streaming @ {fps_display:.1f} FPS (C++ Link: {status}, Detections: {detected_count}, Alert: {alert_in_roi})")

    finally:
        if stream_sock is not None:
            try:
                stream_sock.close()
            except Exception:
                pass
        cap.release()
        if args.preview:
            try:
                cv2.destroyAllWindows()
            except Exception:
                pass
        print("[WEBCAM-FEEDER] Cleaned up resources. Goodbye.")

if __name__ == "__main__":
    main()
