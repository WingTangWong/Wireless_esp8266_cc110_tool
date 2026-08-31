# Saved sample format (`rf315_<name>.bin`)

Raw OOK captures saved via `/api/sample/save` are stored on LittleFS as
`rf315_<name>.bin`, where `<name>` is sanitized to `[A-Za-z0-9_-]`, max 24 chars
(`safeName` in `src/main.cpp`).

All fields are little-endian, structs are `#pragma pack(1)` (no padding).

## Header — 20 bytes

| Offset | Type       | Field        | Notes |
|-------:|------------|--------------|-------|
| 0      | `char[4]`  | `magic`      | `"315R"` |
| 4      | `uint8`    | `version`    | `1` |
| 5      | `uint8[3]` | `reserved`   | zero |
| 8      | `uint32`   | `frequencyHz`| capture centre frequency; must be 300–348 MHz on load |
| 12     | `uint32`   | `pulseCount` | number of `DiskPulse` records that follow; ≤ `MAX_PULSES` (3072) |
| 16     | `uint32`   | `durationUs` | total capture wall time, microseconds |

## Body — `pulseCount` × 5 bytes

Each `DiskPulse`:

| Offset | Type     | Field        | Notes |
|-------:|----------|--------------|-------|
| 0      | `uint32` | `durationUs` | how long the line was held |
| 4      | `uint8`  | `level`      | `1` = HIGH, `0` = LOW |

Consecutive same-level runs are merged (`normalizeCapture`) before saving, so the
levels strictly alternate in a well-formed file. On load the firmware
re-normalizes anyway.

## Loading rules (`loadSample`)

A file is rejected if: magic ≠ `315R`, `version` ≠ 1, `pulseCount` is 0 or
> `MAX_PULSES`, `frequencyHz` outside 300–348 MHz, or the body is short.

## Related

- `/api/capture/pulses` returns the *in-memory* capture as JSON
  `[[durationUs, level], …]` — same data, not the file.
- `tools/rfdecode.py` and `tests/` consume that JSON shape.
