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

### Behaviour as built

- Mapping: **X -> inner (button 1)**, **Y -> outer (button 2)**.
- **One button is serviced at a time.** Only a press that starts from fully
  released fires; any wander mid-hold is ignored until you let go, and nothing
  is queued while a fire is pending or the radio is busy.
- **X+Y fires nothing by default** (`comboEnabled`, persisted, off). When
  enabled it fires **outer, then inner** (operator preference, overriding the
  "1 then 2" wording above).
- `A0` is sampled at ~40 Hz, smoothed (EMA), and classified against four
  thresholds. A level must hold for `settleMs` (140 ms) before it acts.
- The physical buttons ignore the web gate-enable toggle ("buttons always
  work") but have their own `enabled` flag (`/api/buttons/config`).
- Both the button and web paths converge on the same `gateEnqueueFire()` ->
  `fireGate()`, so the transmit is identical; only the preconditions differ.

### Measured levels (this hardware, 220R ladder)

Settled values read back from the `/api/buttons` event ring:

| Level | Settled raw |
|-------|-------------|
| none  | 717-736 |
| Y     | 603 |
| X     | 400-447 |
| X+Y   | 385 |

Shipped thresholds: `thNoneY 640`, `thYX 500`, `thXXy 200`, `thFloor 150`.
Boundaries are biased so an ambiguous reading falls to the safer side — a
missed press is an annoyance, a false one actuates a gate.

**X and X+Y are only ~17 counts apart**, which the ADC cannot resolve reliably;
the event log showed a single held X press sliding `none->x @403` then
`x->xy @385`, and chattering `x->y @581` from switch contact resistance. Since
the combo is off, `thXXy` is set to 200 so the X+Y band folds into X — a
drifting X press still fires inner rather than silently doing nothing.

**To get a usable X+Y combo**, rebuild the ladder with larger values (~10k,
same ratios). Identical divider ratios give identical target voltages, while
making the switch contact resistance (tens of ohms) negligible and cutting the
always-on current ~50x. Then recalibrate and `buttons set --combo`.

### Calibrating

The firmware logs every settle transition (serial `DBG` + the `/api/buttons`
`events` ring and per-level `observed` min/max), so the real landing values can
be read back without a serial cable:

```
tools/rfprobe.py --host <ip> buttons clear
tools/rfprobe.py --host <ip> buttons calibrate none   # nothing held
tools/rfprobe.py --host <ip> buttons calibrate x      # hold X
tools/rfprobe.py --host <ip> buttons calibrate y      # hold Y
tools/rfprobe.py --host <ip> buttons calibrate xy     # hold both
tools/rfprobe.py --host <ip> buttons                  # confirm ranges are separated
```

After the fourth capture the thresholds are set to the midpoints and saved to
`/buttons.bin` (`"BTNS"` v1). Individual values can also be set by hand:
`buttons set --x-xy 200 --no-combo`.
