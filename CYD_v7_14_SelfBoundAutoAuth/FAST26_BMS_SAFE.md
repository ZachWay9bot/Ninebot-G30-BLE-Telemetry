# v7.14 FAST26 BMS-SAFE

This test build fixes telemetry lag caused by missing or slow BMS replies.

## Scheduler
- ESC REG26 speed: target every 200 ms (5 Hz).
- Speed reply timeout: 180 ms.
- BMS current: 750 ms when BMS is online/probing.
- Slow BMS values: one every 2.5 s.
- ESC mode / odometer: one every 2.5 s.
- Optional reply timeout: 140 ms.
- After two consecutive BMS timeouts, BMS polling backs off for 30 s.
- Any valid BMS reply immediately clears the backoff.
- A late BMS reply cannot release a newer speed request; pending target/register are matched exactly.

## Console
The one-second SPD-DIAG line now includes:
- current source
- REG26 receive rate
- last/average speed RTT
- speed/BMS/other timeout counters
- late reply counter
- BMS state: PROBE / ONLINE / BACKOFF

Expected bench behavior with PSU and no answering BMS:
- first BMS probes may timeout,
- BMS switches to BACKOFF,
- REG26 remains near 5 Hz instead of collapsing toward ~1 Hz.
