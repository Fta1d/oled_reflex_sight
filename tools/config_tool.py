#!/usr/bin/env python3
"""
Holosight live config tool — run on the laptop, connect via USB Serial/JTAG.

Lets you tune in real-time:
  - Crosshair position (cx, cy)
  - Hold frame inset (inset_x, inset_y)  →  scales frame symmetrically, stays centered
  - Arrow-tip circle diameter

Each parameter uses its own frame type so changing one value never touches others:
  FRAME_TYPE_CROSSHAIR  0x03  [0x55][0xAA][0x03][cx][cy][0][0][0][0]
  FRAME_TYPE_SET_HOLD   0x07  [0x55][0xAA][0x07][inset_x][inset_y][0][0][0][0]
  FRAME_TYPE_SET_CIRCLE 0x08  [0x55][0xAA][0x08][diameter][0][0][0][0][0]

On connect the tool sends all three to sync the device to the saved config.
Changes are auto-saved so the JSON always reflects the last-sent state.

Usage:
  python3 config_tool.py                   # auto-detect port
  python3 config_tool.py -p /dev/ttyACM0  # explicit port
  python3 config_tool.py --no-serial       # preview only, no device needed
"""

import argparse
import curses
import json
import os
import sys

CONFIG_FILE = os.path.expanduser("~/.holosight_config.json")

FRAME_SYNC0            = 0x55
FRAME_SYNC1            = 0xAA
FRAME_TYPE_CROSSHAIR   = 0x03
FRAME_TYPE_SET_HOLD    = 0x07
FRAME_TYPE_SET_CIRCLE  = 0x08

OLED_W = 128
OLED_H = 64

DEFAULTS = {
    "ch_x":         64,
    "ch_y":         32,
    "hold_inset_x": 35,
    "hold_inset_y": 12,
    "circle_d":     10,
}

# (key, display label, min, max, step)
FIELDS = [
    ("ch_x",         "Crosshair X",      0,  OLED_W - 1, 1),
    ("ch_y",         "Crosshair Y",      0,  OLED_H - 1, 1),
    ("hold_inset_x", "Hold Inset X",     0,  OLED_W // 2 - 2, 1),
    ("hold_inset_y", "Hold Inset Y",     0,  OLED_H // 2 - 2, 1),
    ("circle_d",     "Circle Diameter",  4,  30,          1),
]


def _frame(ftype, b0=0, b1=0, b2=0, b3=0, b4=0, b5=0):
    return bytes([FRAME_SYNC0, FRAME_SYNC1, ftype, b0, b1, b2, b3, b4, b5])


def _write(ser, frame):
    if ser and ser.is_open:
        try:
            ser.write(frame)
        except Exception:
            pass


def send_crosshair(ser, cfg):
    _write(ser, _frame(FRAME_TYPE_CROSSHAIR, cfg["ch_x"], cfg["ch_y"]))

def send_hold_inset(ser, cfg):
    _write(ser, _frame(FRAME_TYPE_SET_HOLD, cfg["hold_inset_x"], cfg["hold_inset_y"]))

def send_circle_d(ser, cfg):
    _write(ser, _frame(FRAME_TYPE_SET_CIRCLE, cfg["circle_d"]))

def send_all(ser, cfg):
    """Sync all parameters to the device — call on connect."""
    send_crosshair(ser, cfg)
    send_hold_inset(ser, cfg)
    send_circle_d(ser, cfg)

# Maps a changed field key to the function that sends only that parameter group.
_FIELD_SENDER = {
    "ch_x":         send_crosshair,
    "ch_y":         send_crosshair,
    "hold_inset_x": send_hold_inset,
    "hold_inset_y": send_hold_inset,
    "circle_d":     send_circle_d,
}


def autosave(cfg):
    try:
        with open(CONFIG_FILE, "w") as f:
            json.dump(cfg, f, indent=2)
    except Exception:
        pass


def draw_oled_preview(win, row, col, cfg, width=64, height=32):
    """Draw a scaled-down ASCII preview of the OLED layout."""
    scale_x = width  / OLED_W
    scale_y = height / OLED_H

    cx = int(cfg["ch_x"] * scale_x)
    cy = int(cfg["ch_y"] * scale_y)
    ix = int(cfg["hold_inset_x"] * scale_x)
    iy = int(cfg["hold_inset_y"] * scale_y)
    fw = width  - 2 * ix
    fh = height - 2 * iy
    cd = max(1, int(cfg["circle_d"] * scale_x * 0.5))

    canvas = [[" "] * width for _ in range(height)]

    # Hold frame border
    for x in range(ix, ix + fw):
        if 0 <= iy < height:         canvas[iy][x]          = "─"
        if 0 <= iy + fh - 1 < height: canvas[iy + fh - 1][x] = "─"
    for y in range(iy, iy + fh):
        if 0 <= y < height:
            if 0 <= ix < width:          canvas[y][ix]          = "│"
            if 0 <= ix + fw - 1 < width: canvas[y][ix + fw - 1] = "│"
    for corner_y, corner_x in [(iy, ix), (iy, ix+fw-1), (iy+fh-1, ix), (iy+fh-1, ix+fw-1)]:
        if 0 <= corner_y < height and 0 <= corner_x < width:
            canvas[corner_y][corner_x] = "+"

    # Circle (rough) around the circle position — just mark center area
    for dy in range(-cd, cd + 1):
        for dx in range(-cd, cd + 1):
            dist = (dx*dx + dy*dy) ** 0.5
            if abs(dist - cd) < 0.8:
                py, px = cy + dy, cx + dx
                if 0 <= py < height and 0 <= px < width:
                    canvas[py][px] = "o"

    # Crosshair
    if 0 <= cy < height:
        for dx in range(-1, 2):
            px = cx + dx
            if 0 <= px < width:
                canvas[cy][px] = "+"
    if 0 <= cx < width:
        for dy in range(-1, 2):
            py = cy + dy
            if 0 <= py < height:
                canvas[py][cx] = "+"

    try:
        win.addstr(row, col, "┌" + "─" * width + "┐")
        for y, line in enumerate(canvas):
            win.addstr(row + 1 + y, col, "│" + "".join(line) + "│")
        win.addstr(row + 1 + height, col, "└" + "─" * width + "┘")
    except curses.error:
        pass


def run_tui(stdscr, ser, cfg):
    curses.curs_set(0)
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_BLACK, curses.COLOR_WHITE)  # selected row
    curses.init_pair(2, curses.COLOR_CYAN,   -1)                 # title
    curses.init_pair(3, curses.COLOR_GREEN,  -1)                 # ok / save
    curses.init_pair(4, curses.COLOR_YELLOW, -1)                 # info / hint
    curses.init_pair(5, curses.COLOR_RED,    -1)                 # warning
    stdscr.keypad(True)
    stdscr.timeout(50)

    # Sync device to loaded config immediately — avoids stale firmware defaults.
    send_all(ser, cfg)

    sel = 0
    status_msg = ""
    status_ticks = 0
    PREVIEW_W = 64
    PREVIEW_H = 32

    while True:
        stdscr.erase()
        rows, cols = stdscr.getmaxyx()

        # Title
        title = " Holosight Config Tool "
        stdscr.addstr(0, 0, title.center(cols, "═"), curses.color_pair(2) | curses.A_BOLD)

        # Connection status
        if ser and ser.is_open:
            conn = f" Connected: {ser.port} "
            stdscr.addstr(1, 0, conn, curses.color_pair(3))
        else:
            stdscr.addstr(1, 0, " No serial connection (preview only) ", curses.color_pair(5))

        # Hold frame size info
        hold_w = OLED_W - 2 * cfg["hold_inset_x"]
        hold_h = OLED_H - 2 * cfg["hold_inset_y"]
        info = f" Hold frame: {hold_w}×{hold_h} px  |  Circle: {cfg['circle_d']} px dia  |  OLED: {OLED_W}×{OLED_H}"
        stdscr.addstr(2, 0, info, curses.color_pair(4))
        stdscr.addstr(3, 0, "─" * min(cols, 70))

        # Fields
        for i, (key, label, vmin, vmax, _step) in enumerate(FIELDS):
            row = 4 + i * 2
            val = cfg[key]
            bar_w = 24
            frac = (val - vmin) / max(vmax - vmin, 1)
            fill = int(frac * bar_w)
            bar = "[" + "█" * fill + "░" * (bar_w - fill) + "]"
            line = f"  {label:<22} {val:>4}  {bar}  [{vmin}–{vmax}]"
            if i == sel:
                stdscr.addstr(row, 0, line[:cols - 1], curses.color_pair(1))
                hint = "  ← → to adjust  |  Shift+← → for ×10  |  ↑ ↓ to switch"
                stdscr.addstr(row + 1, 0, hint[:cols - 1], curses.color_pair(4))
            else:
                stdscr.addstr(row, 0, line[:cols - 1])

        divider_row = 4 + len(FIELDS) * 2 + 1
        stdscr.addstr(divider_row, 0, "─" * min(cols, 70))
        stdscr.addstr(divider_row + 1, 0,
                      " [r] Reset defaults   [q] Quit   (auto-saved on every change) ",
                      curses.color_pair(3))

        if status_msg and status_ticks > 0:
            stdscr.addstr(divider_row + 2, 0, f" {status_msg}", curses.color_pair(3))
            status_ticks -= 1

        # OLED preview (right side if terminal is wide enough)
        preview_col = 75
        if cols > preview_col + PREVIEW_W + 4:
            stdscr.addstr(1, preview_col, " OLED preview (scaled 1:2) ", curses.color_pair(2))
            draw_oled_preview(stdscr, 2, preview_col, cfg, PREVIEW_W, PREVIEW_H)

        stdscr.refresh()

        key = stdscr.getch()
        if key == -1:
            continue

        field_key, _, vmin, vmax, step = FIELDS[sel]
        changed_key = None

        if key == curses.KEY_UP:
            sel = (sel - 1) % len(FIELDS)
        elif key == curses.KEY_DOWN:
            sel = (sel + 1) % len(FIELDS)
        elif key == curses.KEY_RIGHT:
            cfg[field_key] = min(vmax, cfg[field_key] + step)
            changed_key = field_key
        elif key == curses.KEY_LEFT:
            cfg[field_key] = max(vmin, cfg[field_key] - step)
            changed_key = field_key
        elif key == curses.KEY_SRIGHT:  # Shift+Right
            cfg[field_key] = min(vmax, cfg[field_key] + step * 10)
            changed_key = field_key
        elif key == curses.KEY_SLEFT:   # Shift+Left
            cfg[field_key] = max(vmin, cfg[field_key] - step * 10)
            changed_key = field_key
        elif key == ord('r'):
            cfg.update(DEFAULTS)
            send_all(ser, cfg)
            autosave(cfg)
            status_msg = "Reset to defaults"
            status_ticks = 20
        elif key in (ord('q'), 27):
            break

        if changed_key:
            _FIELD_SENDER[changed_key](ser, cfg)  # send only the affected parameter
            autosave(cfg)

    return cfg


def open_serial(port, baud):
    import serial
    return serial.Serial(port, baud, timeout=0.1)


def auto_detect_port():
    try:
        import serial.tools.list_ports
        candidates = list(serial.tools.list_ports.comports())
        # Prefer USB JTAG / CDC ACM devices
        for p in candidates:
            desc = (p.description or "").lower()
            if "jtag" in desc or "acm" in desc or "esp" in desc or "cdc" in desc:
                return p.device
        if candidates:
            return candidates[0].device
    except Exception:
        pass
    return None


def main():
    parser = argparse.ArgumentParser(
        description="Holosight live config tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("-p", "--port", help="Serial port (e.g. /dev/ttyACM0)")
    parser.add_argument("-b", "--baud", type=int, default=115200)
    parser.add_argument("--no-serial", action="store_true",
                        help="Run without a device (preview only)")
    parser.add_argument("--reset", action="store_true",
                        help="Ignore saved config, start from defaults")
    args = parser.parse_args()

    cfg = dict(DEFAULTS)
    if not args.reset and os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE) as f:
                saved = json.load(f)
            cfg.update({k: saved[k] for k in DEFAULTS if k in saved})
            print(f"Loaded config from {CONFIG_FILE}")
        except Exception as e:
            print(f"Could not load saved config: {e}")

    ser = None
    if not args.no_serial:
        port = args.port or auto_detect_port()
        if port:
            try:
                ser = open_serial(port, args.baud)
                print(f"Connected to {port}")
            except Exception as e:
                print(f"Could not open {port}: {e}")
                print("Running without serial (preview only).")
        else:
            print("No serial port found. Run with --no-serial to suppress this warning.")

    try:
        result = curses.wrapper(run_tui, ser, cfg)
    finally:
        if ser:
            ser.close()

    print("\nFinal config:")
    for key, label, _, _, _ in FIELDS:
        print(f"  {label}: {result[key]}")

    hold_w = OLED_W - 2 * result["hold_inset_x"]
    hold_h = OLED_H - 2 * result["hold_inset_y"]
    print(f"  Hold frame size: {hold_w}×{hold_h} px")


if __name__ == "__main__":
    main()
