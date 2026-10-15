# Pairing, credential persistence and AutoAuth

This document describes the legacy NinebotCrypto authentication used by the current `v7.14 Self-Bound AutoAuth` CYD build.

## Roles and transport

The dashboard talks to the stock Ninebot BLE dashboard over the Nordic-UART-style service and uses the legacy NinebotCrypto framing/session counters. BLE/GATT writes are deferred out of notification callbacks.

The current working fallback presents the client as `0x3E` (`PHONE`) because that path is known to accept the physical power-button confirmation on the tested G30 setup.

## First pairing

1. Connect to the scooter and subscribe to notifications.
2. Send `0x5B INIT`.
3. Receive the 16-byte BLE/session challenge and scooter serial.
4. Generate a fresh 16-byte dashboard key.
5. Send `0x5C PING` carrying that key.
6. While the scooter replies `0x5C00`, wait for physical confirmation and resend at a controlled interval.
7. Press the scooter power button once.
8. When `0x5C01` arrives, derive the session key from the dashboard key and BLE key.
9. Send `0x5D PAIR` with the scooter serial.
10. Continue only after `0x5D01`.

```text
5B -> fresh key -> 5C -> 5C00 -> power button -> 5C01
   -> SHA1(key || BLE_key) -> 5D(serial) -> 5D01 -> READY
```

### Persistence rule

The dashboard stores a self-bound credential only after `0x5D01`. It stores both:

- the accepted 16-byte dashboard key
- the exact scooter serial it belongs to

A partially completed pairing, timeout or `0x5C00` alone is not enough to save a key.

## v7.14 Self-Bound AutoAuth

On later boots, if the stored serial matches the scooter returned by INIT, v7.14 tries the bound-client reconnect before starting another pairing request:

```text
5B INIT
  -> load stored key for exact serial
  -> SHA1(stored_key || BLE_key)
  -> direct 5D(serial)
  -> 5D01
  -> READY
```

This intentionally skips `0x5C`. Skipping `0x5C` is the part that avoids requesting a fresh physical power-button confirmation.

### Failure handling

A stored credential is treated as disposable, not sacred. If the direct `0x5D` path is rejected or times out:

1. delete the self-bound credential,
2. disconnect BLE,
3. reset crypto/session state,
4. reconnect,
5. perform one normal first-pairing flow with a fresh key and power-button confirmation.

This avoids crypto-counter drift and avoids an endless loop with a rejected key.

## Optional official Segway app key

The dashboard can also store a real 16-byte Segway app key (`<SERIAL>_decrypt`). When present, it has priority over the self-bound dashboard key and uses the direct app-style authentication path.

Serial commands:

```text
APPKEY?
APPKEY=<32 hex characters>
APPKEY=CLEAR
```

The temporary random key generated during first pairing is not automatically the same thing as the official Segway app key.

## Crypto counters

The direct-auth path must start from a clean session/counter state after INIT. A failed encrypted probe must not be followed by normal auth in the same desynchronized session. If a direct attempt fails, the implementation disconnects and starts a fresh BLE/Crypto session before fallback.

Late or duplicate packets whose RX counter is not newer than the local counter may appear during aggressive retries. They are not used to advance authentication.

## AutoConnect is separate from AutoAuth

AutoConnect means the ESP32 finds and connects to the stored target scooter automatically when it becomes visible. AutoAuth means authentication completes without requesting another power-button confirmation. They are separate state machines.

The target name is learned only after successful authentication and can be managed through:

```text
TARGET?
TARGET=CLEAR
```

## DPC after READY

Only after READY does the dashboard write DPC:

```text
ESC 0x20 / CMD 0x03 / REG 0x76 / DATA 01 00
```

It then reads register `0x76` back and expects `0x0001`. DPC is not part of the pairing handshake itself.

## Validation status

- Normal `5B -> 5C -> physical button -> 5C01 -> 5D -> 5D01` G30 path: hardware-tested.
- DPC `0x76 = 1` plus readback: hardware-tested in the current dashboard test flow.
- Target-locked AutoConnect: hardware-tested in the current dashboard test flow.
- v7.14 self-bound direct `5D` reconnect: current buttonless AutoAuth path; acceptance may vary with BLE/dashboard firmware and is therefore documented separately from the known-good fallback.
