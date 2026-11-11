# v7.14 CMD64 Speed Primary Experiment

Purpose: use the stock dashboard speed path when BLE117 forwards it to the phone session.

Behavior:
- Passive ESC->BLE CMD64 payload is interpreted as [state][battery][light][beep][speed][fault].
- CMD64 status[4] becomes the preferred displayed speed.
- ESC register 0x26 remains a compatibility fallback only.
- Freshness window: 1200 ms.
- While CMD64 is fresh, the existing REG_SPEED slots in the 100 ms poll table are left idle.
  This removes about half of the active app reads from the shared Yellow-Wire bus.
- If CMD64 is not forwarded or stops arriving for >1.2 s, original REG26 polling resumes automatically.
- Authentication, DPC, BMS reads, odometer, mode and UI rendering are otherwise unchanged.

Important:
This branch intentionally tests whether stock BLE117 forwards internal CMD64 frames into the authenticated phone session.
If it does not, the dashboard will automatically stay on REG26 fallback and behavior remains compatible.
