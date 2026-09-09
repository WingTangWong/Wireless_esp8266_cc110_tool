# Buttons Addon to the wemos d1 mini + C1101 module

The schematic added:

3V3 --- 200R --- A0 --- X --- Y --- 200R --- 200R --- GND

X --- Normally Open Momentary Switch --- 200R --- GND
Y --- Normally Open Momentary Switch --- 200R --- 200R --- 200R --- GND

The idea being that A0 currently will read a value which is not-zero and not-max.

When button X is pressed, this will change the value read at A0 to a new value x-val. When released, will reset value of A0 to default.
When button Y is pressed, this will change the value read at A0 to a new value y-val. When released, will reset value of A0 to default.
When both X and Y is pressed, this will change the value read at A0 to a new X & Y val. When released, will reset value of A0 to default.

Desired:

- The dashboard should reflect the state of the two buttons. Whether one or both are pressed.
- When button X is pressed, update the button and trigger the playback assigned to button 1.
- When button Y is pressed, update the button and trigger the playback assigned to button 2.
- When both buttons are pressed, update both buttons and trigger the playback assigned to button 1 and then button 2

---

## Implementation notes

Implemented in `src/main.cpp` (`serviceButtons()` + `/api/buttons*`). See the
"A0 two-button addon" section of `README.md` for the endpoints.

- Mapping: **X → inner (button 1)**, **Y → outer (button 2)**. When both are
  pressed together the fire order is **outer, then inner** (per operator request,
  overriding "1 then 2" above). Staggered presses fire each gate on its own edge.
- `A0` is sampled at ~40 Hz, smoothed (EMA), and classified against four
  thresholds. A level must hold for `settleMs` (default 140 ms) before it acts,
  which debounces the ladder and lets a near-simultaneous two-finger press land
  as `X+Y` directly.
- The physical buttons ignore the web gate-enable toggle ("buttons always work")
  but have their own `enabled` flag (`/api/buttons/config`).
- Thresholds are **calibrated on the real board** and persist to `/buttons.bin`.
  The firmware logs every settle transition (serial `DBG` + the `/api/buttons`
  `events` ring and per-level `observed` min/max) so the actual landing values
  can be read back without a serial cable:

  ```
  tools/rfprobe.py --host <ip> buttons clear
  tools/rfprobe.py --host <ip> buttons calibrate none   # nothing held
  tools/rfprobe.py --host <ip> buttons calibrate x      # hold X
  tools/rfprobe.py --host <ip> buttons calibrate y      # hold Y
  tools/rfprobe.py --host <ip> buttons calibrate xy     # hold both
  tools/rfprobe.py --host <ip> buttons                  # confirm ranges are separated
  ```

  After the fourth capture the thresholds are set to the midpoints and saved.
  Fixed defaults (`thNoneY=640, thYX=500, thXXy=398, thFloor=150`) are only a
  starting point derived from the nominal resistor values.
