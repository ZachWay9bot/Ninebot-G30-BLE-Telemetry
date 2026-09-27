# Release notes - v6.2 DPC ON

Test build based on the frozen v6.1 hardware-tested NinebotCrypto telemetry baseline.

## Added

After successful authentication reaches:

```text
0x5D01 / AUTH_READY
```

the sketch schedules one ESC write from normal `loop()` context:

```text
5A A5 02 3E 20 03 76 01 00
```

Decoded:

- destination: ESC `0x20`
- command: WRITE `0x03`
- register: `0x76`
- value: `0x0001`

The legacy unencrypted checksum for that logical frame would be `25 FF`. The v6.2 implementation does not append it manually because the established NinebotCrypto session generates the encrypted trailer, counter and authentication data.

## Callback safety

The write is not issued from the BLE notify callback. It is queued as `TX_DPC_ON` and transmitted from `loop()`, preserving the callback-safe architecture that fixed the earlier GATT deadlock.

## Status

- v6.1: frozen, hardware-tested baseline
- v6.2: test build, DPC ON write not yet marked hardware-confirmed
