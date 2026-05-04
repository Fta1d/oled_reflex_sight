#!/usr/bin/env python3
"""
Holosight test sender
  - Visualizes frames on a scaled OLED preview
  - Sends detection frames over serial
  - Measures round-trip software latency (requires ACK from ESP — see README)

Controls:
  Left-drag   draw bounding box
  Right-click set predicted lead point
"""

import tkinter as tk
from tkinter import ttk
import serial
import serial.tools.list_ports
import threading
import time
import math
import struct
import logging
import queue


logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s.%(msecs)03d  %(levelname)-5s  %(message)s",
    datefmt="%H:%M:%S",
)

# ── Protocol ────────────────────────────────────────────────────────────────

SYNC0, SYNC1       = 0x55, 0xAA
FRAME_TYPE_TARGET  = 0x01
FRAME_TYPE_LOST    = 0x02
ACK_BYTE           = b'\xac'   # ESP sends this after processing (optional)

OLED_W, OLED_H = 128, 64
SCALE          = 5             # each OLED pixel → 5×5 screen pixels
CW, CH         = OLED_W * SCALE, OLED_H * SCALE
SIGHT_ZOOM_X   = 1.74          # camera_hfov / oled_hfov — measured: 314cm / 180cm at 5m
SIGHT_ZOOM_Y   = 2.38          # camera_vfov / oled_vfov — measured: 190cm / 80cm at 5m

# Boresight: camera pixel that corresponds to the barrel aim point (= OLED crosshair).
# Measure: aim crosshair at a distant object, note its camera pixel.
CAM_BX = 335
CAM_BY = 175
PARALLAX_Y = 0   # OLED pixels to shift box vertically — tune at typical engagement distance
               # negative = shift up, positive = shift down

# Crosshair geometry — must match ui_build_reticle() in ui.c
CROSS_CX  = OLED_W // 2
CROSS_CY  = OLED_H // 2 + 8
CROSS_ARM = 10


def build_frame(ftype, bx=0, by=0, bw=0, bh=0, px=0, py=0) -> bytes:
    return struct.pack('9B', SYNC0, SYNC1, ftype, bx, by, bw, bh, px, py)


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


# ── App ──────────────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Holosight Test Sender")
        self.resizable(False, False)

        self._serial: serial.Serial | None = None
        self._tx_time: float | None = None
        self._tx_queue: queue.Queue = queue.Queue(maxsize=1)
        threading.Thread(target=self._tx_worker, daemon=True).start()
        self._auto_stop = threading.Event()
        self._auto_stop.set()   # stopped initially
        self._animating = False

        self._yolo_stop = threading.Event()
        self._yolo_stop.set()
        self._yolo_running = False

        # OLED state
        self.bbox = [20, 10, 30, 20]   # [x, y, w, h]
        self.pred = [70, 28]            # [x, y]
        self._drag_origin = None

        self._build_ui()
        self._redraw()

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self):
        # Serial bar
        bar = ttk.Frame(self, padding=(6, 4))
        bar.pack(fill=tk.X)

        ttk.Label(bar, text="Port:").pack(side=tk.LEFT)
        self._port_var = tk.StringVar()
        port_cb = ttk.Combobox(bar, textvariable=self._port_var, width=16)
        port_cb['values'] = [p.device for p in serial.tools.list_ports.comports()]
        if port_cb['values']:
            self._port_var.set(port_cb['values'][0])
        port_cb.pack(side=tk.LEFT, padx=4)

        ttk.Label(bar, text="Baud:").pack(side=tk.LEFT)
        self._baud_var = tk.StringVar(value="115200")
        ttk.Entry(bar, textvariable=self._baud_var, width=8).pack(side=tk.LEFT, padx=4)

        self._conn_btn = ttk.Button(bar, text="Connect", command=self._toggle_connect)
        self._conn_btn.pack(side=tk.LEFT, padx=4)

        self._status = ttk.Label(bar, text="● Disconnected", foreground="red")
        self._status.pack(side=tk.LEFT, padx=8)

        # Canvas
        frame = ttk.Frame(self, padding=(6, 2))
        frame.pack()
        ttk.Label(frame, text="OLED preview  —  left-drag: bbox   right-click: pred point").pack()

        self._canvas = tk.Canvas(frame, width=CW, height=CH,
                                  bg="black", cursor="crosshair",
                                  highlightthickness=1, highlightbackground="#444")
        self._canvas.pack()
        self._canvas.bind("<ButtonPress-1>",   self._drag_start)
        self._canvas.bind("<B1-Motion>",        self._drag_move)
        self._canvas.bind("<ButtonRelease-1>", self._drag_end)
        self._canvas.bind("<ButtonPress-3>",   self._set_pred)

        self._coords = tk.StringVar()
        ttk.Label(frame, textvariable=self._coords, foreground="#aaa").pack()

        # Controls
        ctrl = ttk.Frame(self, padding=(6, 4))
        ctrl.pack(fill=tk.X)

        ttk.Button(ctrl, text="Send TARGET", command=self._send_target).pack(side=tk.LEFT, padx=4)
        ttk.Button(ctrl, text="Send LOST",   command=self._send_lost).pack(side=tk.LEFT, padx=2)

        ttk.Separator(ctrl, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8, pady=2)

        self._auto_var = tk.BooleanVar()
        ttk.Checkbutton(ctrl, text="Auto-send", variable=self._auto_var,
                        command=self._toggle_auto).pack(side=tk.LEFT)
        ttk.Label(ctrl, text="FPS:").pack(side=tk.LEFT, padx=(6, 2))
        self._fps_var = tk.StringVar(value="60")
        ttk.Entry(ctrl, textvariable=self._fps_var, width=4).pack(side=tk.LEFT)

        ttk.Separator(ctrl, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8, pady=2)

        self._anim_btn = ttk.Button(ctrl, text="Animate", command=self._toggle_animate)
        self._anim_btn.pack(side=tk.LEFT, padx=4)

        # Latency
        lat = ttk.Frame(self, padding=(6, 4))
        lat.pack(fill=tk.X)
        self._latency = tk.StringVar(value="Latency: — ms")
        ttk.Label(lat, textvariable=self._latency, foreground="#aaa").pack(side=tk.LEFT)

        # YOLO
        yolo = ttk.LabelFrame(self, text="YOLO Detection", padding=(6, 4))
        yolo.pack(fill=tk.X, padx=6, pady=(2, 6))

        row1 = ttk.Frame(yolo)
        row1.pack(fill=tk.X)
        ttk.Label(row1, text="Model:").pack(side=tk.LEFT)
        self._yolo_model_var = tk.StringVar(value="best.pt")
        model_cb = ttk.Combobox(row1, textvariable=self._yolo_model_var, width=16)
        model_cb['values'] = ["yolov8n.pt", "yolov8s.pt", "yolov8m.pt",
                              "yolo11n.pt", "yolo11s.pt"]
        model_cb.pack(side=tk.LEFT, padx=4)
        ttk.Label(row1, text="Source:").pack(side=tk.LEFT, padx=(8, 2))
        self._yolo_src_var = tk.StringVar(value="0")
        ttk.Entry(row1, textvariable=self._yolo_src_var, width=8).pack(side=tk.LEFT)

        row2 = ttk.Frame(yolo)
        row2.pack(fill=tk.X, pady=(4, 0))
        ttk.Label(row2, text="Conf:").pack(side=tk.LEFT)
        self._yolo_conf_var = tk.StringVar(value="0.40")
        ttk.Entry(row2, textvariable=self._yolo_conf_var, width=5).pack(side=tk.LEFT, padx=4)
        ttk.Label(row2, text="Class (−1=any):").pack(side=tk.LEFT, padx=(8, 2))
        self._yolo_cls_var = tk.StringVar(value="-1")
        ttk.Entry(row2, textvariable=self._yolo_cls_var, width=4).pack(side=tk.LEFT)
        ttk.Label(row2, text="Device:").pack(side=tk.LEFT, padx=(8, 2))
        self._yolo_dev_var = tk.StringVar(value="cpu")
        dev_cb = ttk.Combobox(row2, textvariable=self._yolo_dev_var, width=6)
        dev_cb['values'] = ["cpu", "0", "1"]
        dev_cb.pack(side=tk.LEFT)
        self._yolo_btn = ttk.Button(row2, text="Start YOLO", command=self._toggle_yolo)
        self._yolo_btn.pack(side=tk.LEFT, padx=8)
        self._yolo_lbl = ttk.Label(row2, text="", foreground="#aaa")
        self._yolo_lbl.pack(side=tk.LEFT)

        self._cam_label = ttk.Label(yolo)
        self._cam_label.pack(pady=(6, 2))
        self._cam_photo = None  # keep reference to prevent GC

    # ── Serial ────────────────────────────────────────────────────────────────

    def _toggle_connect(self):
        if self._serial and self._serial.is_open:
            self._serial.close()
            self._serial = None
            self._status.config(text="● Disconnected", foreground="red")
            self._conn_btn.config(text="Connect")
            return

        try:
            self._serial = serial.Serial(
                self._port_var.get(), int(self._baud_var.get()), timeout=0.1)
            self._status.config(text=f"● {self._port_var.get()}", foreground="lime")
            self._conn_btn.config(text="Disconnect")
            threading.Thread(target=self._ack_reader, daemon=True).start()
        except Exception as e:
            self._status.config(text=f"● {e}", foreground="red")

    def _tx_worker(self):
        while True:
            try:
                data = self._tx_queue.get(timeout=1.0)
            except queue.Empty:
                continue
            if self._serial and self._serial.is_open:
                try:
                    self._tx_time = time.perf_counter()
                    self._serial.write(data)
                    logging.debug("TX  %s", data.hex(" "))
                except Exception as e:
                    logging.warning("TX error: %s", e)

    def _write(self, data: bytes):
        if self._serial and self._serial.is_open:
            try:
                self._tx_queue.put_nowait(data)
            except queue.Full:
                pass  # drop frame — sender thread still busy with previous

    def _send_target(self):
        bx, by, bw, bh = self.bbox
        px, py = self.pred
        self._write(build_frame(FRAME_TYPE_TARGET, bx, by, bw, bh, px, py))

    def _send_lost(self):
        self._write(build_frame(FRAME_TYPE_LOST))

    # Reads optional ACK byte (0xAC) sent by ESP after processing the frame.
    # To enable: add  uart_comm_send((uint8_t[]){0xAC}, 1);  after ui_unlock()
    # in main.c. The latency shown is round-trip minus ~0.9 ms UART overhead.
    def _ack_reader(self):
        while self._serial and self._serial.is_open:
            try:
                b = self._serial.read(1)
            except Exception:
                break
            if not b:
                continue
            byte_val = b[0]
            if byte_val == 0xBE:
                logging.info("RX  heartbeat 0xBE — ESP TX works, RX not receiving frames")
                self.after(0, lambda: self._latency.set(
                    "Heartbeat 0xBE — ESP TX works but RX not receiving frames"))
            elif b == ACK_BYTE and self._tx_time is not None:
                rtt_ms = (time.perf_counter() - self._tx_time) * 1000
                sw_ms = rtt_ms - 0.9
                logging.info("ACK rtt=%.1f ms  sw=%.1f ms", rtt_ms, sw_ms)
                text = (f"RTT: {rtt_ms:.1f} ms   SW latency: ~{sw_ms:.1f} ms"
                        f"   (+LVGL tick ≤5 ms  +I2C flush ~23 ms for visual)")
                self.after(0, lambda t=text: self._latency.set(t))
                self._tx_time = None
            else:
                logging.warning("RX  unexpected byte 0x%02X", byte_val)
                self.after(0, lambda v=byte_val: self._latency.set(f"RX byte: 0x{v:02X}"))

    # ── Auto-send / animation ─────────────────────────────────────────────────

    def _fps(self) -> float:
        try:
            return max(1.0, float(self._fps_var.get()))
        except ValueError:
            return 30.0

    def _toggle_auto(self):
        if self._auto_var.get():
            self._auto_stop.clear()
            threading.Thread(target=self._auto_loop, daemon=True).start()
        else:
            self._auto_stop.set()

    def _auto_loop(self):
        while not self._auto_stop.is_set():
            self._send_target()
            time.sleep(1.0 / self._fps())

    def _toggle_animate(self):
        if self._animating:
            self._animating = False
            self._auto_stop.set()
            self._auto_var.set(False)
            self._anim_btn.config(text="Animate")
        else:
            self._animating = True
            self._auto_stop.clear()
            self._auto_var.set(True)
            self._anim_btn.config(text="Stop")
            threading.Thread(target=self._animate_loop, daemon=True).start()

    def _animate_loop(self):
        t = 0.0
        while not self._auto_stop.is_set() and self._animating:
            # Circular drone path with offset lead point
            cx = int(OLED_W / 2 + 35 * math.sin(t))
            cy = int(OLED_H / 2 + 18 * math.cos(t * 0.9))
            bw, bh = 22, 14
            self.bbox = [
                clamp(cx - bw // 2, 0, OLED_W - bw),
                clamp(cy - bh // 2, 0, OLED_H - bh),
                bw, bh
            ]
            # Lead point slightly ahead of motion direction
            self.pred = [
                clamp(cx + int(9 * math.cos(t)), 0, OLED_W - 1),
                clamp(cy + int(5 * math.sin(t * 0.9 + 0.5)), 0, OLED_H - 1),
            ]
            logging.debug("ANI bbox=%s pred=%s", self.bbox, self.pred)
            self.after(0, self._redraw)
            self._send_target()
            t += 2 * math.pi / (self._fps() * 4)   # full loop in ~4 s
            time.sleep(1.0 / self._fps())

    # ── Canvas interaction ────────────────────────────────────────────────────

    def _to_oled(self, cx, cy):
        return clamp(cx // SCALE, 0, OLED_W - 1), clamp(cy // SCALE, 0, OLED_H - 1)

    def _drag_start(self, e):
        self._drag_origin = self._to_oled(e.x, e.y)

    def _drag_move(self, e):
        if not self._drag_origin:
            return
        x0, y0 = self._drag_origin
        x1, y1 = self._to_oled(e.x, e.y)
        self.bbox = [min(x0, x1), min(y0, y1),
                     max(abs(x1 - x0), 1), max(abs(y1 - y0), 1)]
        self._redraw()

    def _drag_end(self, _e):
        self._drag_origin = None

    def _set_pred(self, e):
        self.pred = list(self._to_oled(e.x, e.y))
        self._redraw()

    # ── Drawing ───────────────────────────────────────────────────────────────

    def _redraw(self):
        c = self._canvas
        c.delete("all")

        # Subtle grid (8-pixel OLED blocks)
        for x in range(0, CW + 1, SCALE * 8):
            c.create_line(x, 0, x, CH, fill="#0f0f0f")
        for y in range(0, CH + 1, SCALE * 8):
            c.create_line(0, y, CW, y, fill="#0f0f0f")

        # "pop" label — matches lv_obj_align TOP_MID + y=2
        c.create_text(CW // 2, 2 * SCALE + 4, text="pop",
                      fill="white", font=("Courier", 7), anchor=tk.N)

        # Crosshair — matches CROSS_CX / CROSS_CY / CROSS_ARM in ui.c
        ccx = CROSS_CX * SCALE
        ccy = CROSS_CY * SCALE
        arm = CROSS_ARM * SCALE
        c.create_line(ccx - arm, ccy, ccx + arm, ccy, fill="white")
        c.create_line(ccx, ccy - arm, ccx, ccy + arm, fill="white")

        # Bounding box
        bx, by, bw, bh = self.bbox
        c.create_rectangle(
            bx * SCALE, by * SCALE,
            (bx + bw) * SCALE, (by + bh) * SCALE,
            outline="white", width=1)

        # Predicted lead point (3×3 dot, centered)
        px, py = self.pred
        c.create_rectangle(
            (px - 1) * SCALE, (py - 1) * SCALE,
            (px + 2) * SCALE, (py + 2) * SCALE,
            fill="white", outline="")

        # Coord readout
        bx, by, bw, bh = self.bbox
        px, py = self.pred
        self._coords.set(
            f"bbox  x={bx:3d}  y={by:3d}  w={bw:3d}  h={bh:3d}    "
            f"pred  x={px:3d}  y={py:3d}")

    # ── YOLO ──────────────────────────────────────────────────────────────────

    def _toggle_yolo(self):
        if self._yolo_running:
            self._yolo_stop.set()
            self._yolo_btn.config(text="Start YOLO")
            self._yolo_lbl.config(text="Stopping…", foreground="#aaa")
        else:
            self._yolo_stop.clear()
            self._yolo_running = True
            self._yolo_btn.config(text="Stop YOLO")
            self._yolo_lbl.config(text="Loading model…", foreground="yellow")
            threading.Thread(target=self._yolo_loop, daemon=True).start()

    def _yolo_loop(self):
        try:
            from ultralytics import YOLO
            import cv2
            from PIL import Image, ImageTk
        except ImportError as e:
            self.after(0, lambda: self._yolo_lbl.config(
                text=f"Import error: {e}", foreground="red"))
            self._yolo_running = False
            self.after(0, lambda: self._yolo_btn.config(text="Start YOLO"))
            return

        try:
            model = YOLO(self._yolo_model_var.get())
        except Exception as e:
            self.after(0, lambda: self._yolo_lbl.config(
                text=f"Model error: {e}", foreground="red"))
            self._yolo_running = False
            self.after(0, lambda: self._yolo_btn.config(text="Start YOLO"))
            return

        src = self._yolo_src_var.get()
        try:
            src = int(src)
        except ValueError:
            pass  # file path — leave as string

        cap = cv2.VideoCapture(src)
        cap.set(cv2.CAP_PROP_FRAME_WIDTH,  640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        if not cap.isOpened():
            self.after(0, lambda: self._yolo_lbl.config(
                text="Cannot open source", foreground="red"))
            self._yolo_running = False
            self.after(0, lambda: self._yolo_btn.config(text="Start YOLO"))
            return

        try:
            conf = float(self._yolo_conf_var.get())
        except ValueError:
            conf = 0.40
        try:
            cls_filter = int(self._yolo_cls_var.get())
        except ValueError:
            cls_filter = -1
        device = self._yolo_dev_var.get() or "cpu"

        self.after(0, lambda: self._yolo_lbl.config(text="Running", foreground="lime"))

        try:
            while not self._yolo_stop.is_set():
                ret, frame = cap.read()
                if ret and not hasattr(self, '_cam_res_logged'):
                    h, w = frame.shape[:2]
                    logging.info("Camera resolution: %dx%d", w, h)
                    self._cam_res_logged = True
                if not ret:
                    break

                # frame = cv2.rotate(frame, cv2.ROTATE_90_COUNTERCLOCKWISE)
                cam_h, cam_w = frame.shape[:2]
                classes = None if cls_filter < 0 else [cls_filter]
                try:
                    results = model(frame, conf=conf, verbose=False,
                                    device=device, classes=classes, imgsz=320)
                except Exception as e:
                    logging.error("YOLO inference error: %s", e)
                    self.after(0, lambda msg=str(e): self._yolo_lbl.config(
                        text=f"Error: {msg}", foreground="red"))
                    break

                best = None
                best_conf = -1.0
                for r in results:
                    for box in r.boxes:
                        c = float(box.conf[0])
                        if c > best_conf:
                            best_conf = c
                            best = box

                # Camera preview — annotated frame scaled to 320px wide
                annotated = results[0].plot()  # BGR numpy array with boxes drawn
                prev_w = 320
                prev_h = int(cam_h * prev_w / cam_w)
                annotated = cv2.resize(annotated, (prev_w, prev_h))
                pil_img = Image.fromarray(cv2.cvtColor(annotated, cv2.COLOR_BGR2RGB))
                def _update_preview(im=pil_img):
                    photo = ImageTk.PhotoImage(im)  # must be created on main thread
                    self._cam_photo = photo
                    self._cam_label.config(image=photo)
                self.after(0, _update_preview)

                if best is not None:
                    x1, y1, x2, y2 = best.xyxy[0].tolist()
                    tcx = (x1 + x2) / 2
                    tcy = (y1 + y2) / 2
                    cx = CROSS_CX + (tcx - CAM_BX) * OLED_W / cam_w * SIGHT_ZOOM_X
                    cy = CROSS_CY + (tcy - CAM_BY) * OLED_H / cam_h * SIGHT_ZOOM_Y + PARALLAX_Y
                    # skip if target projects outside sight FOV
                    if not (0 <= cx < OLED_W and 0 <= cy < OLED_H):
                        self._write(build_frame(FRAME_TYPE_LOST))
                        logging.debug("YOLO TARGET outside sight FOV, sending LOST")
                        continue
                    bw = clamp(int((x2 - x1) * OLED_W / cam_w * SIGHT_ZOOM_X), 1, OLED_W)
                    bh = clamp(int((y2 - y1) * OLED_H / cam_h * SIGHT_ZOOM_Y), 1, OLED_H)
                    bx = clamp(int(cx - bw / 2), 0, OLED_W - bw)
                    by = clamp(int(cy - bh / 2), 0, OLED_H - bh)
                    px = clamp(int(cx), 0, OLED_W - 1)
                    py = clamp(int(cy), 0, OLED_H - 1)
                    self.bbox = [bx, by, bw, bh]
                    self.pred = [px, py]
                    self.after(0, self._redraw)
                    self._write(build_frame(FRAME_TYPE_TARGET, bx, by, bw, bh, px, py))
                    logging.debug(
                        "YOLO TARGET bbox=[%d,%d,%d,%d] pred=[%d,%d] conf=%.2f",
                        bx, by, bw, bh, px, py, best_conf)
                else:
                    self._write(build_frame(FRAME_TYPE_LOST))
                    logging.debug("YOLO LOST (no detection)")
        finally:
            cap.release()
            self._yolo_running = False
            self.after(0, lambda: self._yolo_btn.config(text="Start YOLO"))
            self.after(0, lambda: self._yolo_lbl.config(text="Stopped", foreground="#aaa"))
            self.after(0, lambda: (self._cam_label.config(image=""), setattr(self, "_cam_photo", None)))

    # ── Cleanup ───────────────────────────────────────────────────────────────

    def _on_close(self):
        self._auto_stop.set()
        if self._serial and self._serial.is_open:
            self._serial.close()
        self.destroy()


if __name__ == "__main__":
    app = App()
    app.protocol("WM_DELETE_WINDOW", app._on_close)
    app.mainloop()
