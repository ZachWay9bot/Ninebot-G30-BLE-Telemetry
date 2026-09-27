/*
  Ninebot MAX G30 BLE Telemetry v6.2 - DPC ON
  =================================
  ESP32-C3 / Arduino ESP32 BLE (BLEDevice.h)

  Erkenntnisse aus dem Hardware-Test:
  - Service: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
  - Write:   6e400002-b5a3-f393-e0a9-e50e24dcca9e
  - Notify:  6e400003-b5a3-f393-e0a9-e50e24dcca9e
  - Dieses Dashboard antwortet nur zuverlaessig bei BLE Write WITH response.
  - NinebotCrypto (legacy chained crypto) ist bestaetigt.
  - 0x5B-Antwort: BLE random/key + 14-byte Seriennummer.

  Ablauf:
  1) BLE verbinden + Notify aktivieren
  2) NinebotCrypto INIT 0x5B
  3) BLE-Random + Seriennummer aus Antwort uebernehmen
  4) PING/AUTH 0x5C mit 16 Byte App-Key
  5) Power-Taste bestaetigen, bis 0x5C01 empfangen wird
  6) PAIR 0x5D mit Seriennummer
  7) Nach erfolgreicher Authentifizierung NUR READ-ONLY:
       BMS 0x32 -> Battery %
       ESC 0x26 -> Speed
       BMS 0x33/34/35 -> Current/Voltage/Temperature
       ESC 0x29/0x2A -> Odometer

  Hinweis:
  Die Authentifizierung 0x5C/0x5D betrifft den BLE-Pairing/Auth-Zustand.
  Der Sketch schreibt KEINE Fahrparameter / Speed-Limits / ESC-Settings.
*/

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

#include <cstring>
#include <string>

// ============================================================
// BLE UUIDs
// ============================================================

static BLEUUID SVC_UUID   ("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID CHR_WRITE  ("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID CHR_NOTIFY ("6e400003-b5a3-f393-e0a9-e50e24dcca9e");
static BLEUUID CCCD_UUID  ((uint16_t)0x2902);

// ============================================================
// Ninebot IDs / commands
// ============================================================

static const uint8_t DEV_ESC   = 0x20;
static const uint8_t DEV_BLE   = 0x21;
static const uint8_t DEV_BATT  = 0x22;
static const uint8_t DEV_PHONE = 0x3E;

static const uint8_t CMD_READ     = 0x01;
static const uint8_t CMD_WRITE    = 0x03;
static const uint8_t CMD_READ_ACK = 0x04;

static const uint8_t CMD_INIT = 0x5B;
static const uint8_t CMD_PING = 0x5C;
static const uint8_t CMD_PAIR = 0x5D;

// Read-only G30 registers.
// Controller:
static const uint8_t REG_SPEED    = 0x26; // current speed, 0.1 km/h
static const uint8_t REG_ODO_LO   = 0x29; // total mileage low 16 bit, metres
static const uint8_t REG_ODO_HI   = 0x2A; // total mileage high 16 bit
// BMS:
static const uint8_t REG_BAT_PCT  = 0x32; // battery percent
static const uint8_t REG_BAT_CUR  = 0x33; // signed current, 0.01 A
static const uint8_t REG_BAT_VOLT = 0x34; // voltage, 0.01 V
static const uint8_t REG_BAT_TEMP = 0x35; // two temperature bytes, each offset +20

// Firmware switch requested for v6.2
static const uint8_t REG_DPC = 0x76;
static const uint8_t DPC_ON_VALUE[2] = { 0x01, 0x00 };

// ============================================================
// NinebotCrypto constants/state
// ============================================================

static const uint8_t FW_DATA[16] = {
  0x97, 0xCF, 0xB8, 0x02,
  0x84, 0x41, 0x43, 0xDE,
  0x56, 0x00, 0x2B, 0x3B,
  0x34, 0x78, 0x0A, 0x5D
};

uint8_t scooterName16[16] = {0};
uint8_t randomBleData[16] = {0};
uint8_t randomAppData[16] = {0};
uint8_t shaKey[16] = {0};

uint32_t msgIt = 0;

uint8_t scooterSerial[14] = {0};
bool haveInitData = false;
bool haveAppKey = false;

// ============================================================
// BLE state
// ============================================================

BLEAdvertisedDevice* foundDevice = nullptr;
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

volatile bool doConnect = false;
volatile bool isConnected = false;

// ============================================================
// Protocol state
// ============================================================

enum AuthState {
  AUTH_IDLE,
  AUTH_WAIT_INIT,
  AUTH_WAIT_PING,
  AUTH_WAIT_PAIR,
  AUTH_READY,
  AUTH_ERROR
};

volatile AuthState authState = AUTH_IDLE;

// IMPORTANT:
// Never call BLE writeValue() from the notify callback.
// The ESP32 BLE stack can deadlock because writeValue waits for a GATT event
// while the GATT callback that should release it is still running.
enum PendingTx {
  TX_NONE,
  TX_PING,
  TX_PAIR,
  TX_DPC_ON
};

volatile PendingTx pendingTx = TX_NONE;

bool dpcOnSent = false;

unsigned long authLastSend = 0;
unsigned long readySince = 0;

// only one telemetry request at a time
bool telemetryWaiting = false;
unsigned long telemetrySentAt = 0;
unsigned long lastTelemetry = 0;
uint8_t telemetryStep = 0;

uint16_t odoLow = 0;
uint16_t odoHigh = 0;
bool haveOdoLow = false;
bool haveOdoHigh = false;

// ============================================================
// RX reassembly
// ============================================================

uint8_t rxBuffer[192];
size_t rxLen = 0;
size_t rxExpected = 0;

// ============================================================
// Debug helpers
// ============================================================

void dumpHex(const char* prefix, const uint8_t* data, size_t len) {
  Serial.print(prefix);

  for (size_t i = 0; i < len; ++i) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);

    if (i + 1 < len) Serial.print(' ');
  }

  Serial.println();
}

const char* stateName(AuthState s) {
  switch (s) {
    case AUTH_IDLE:      return "IDLE";
    case AUTH_WAIT_INIT: return "WAIT_INIT";
    case AUTH_WAIT_PING: return "WAIT_PING";
    case AUTH_WAIT_PAIR: return "WAIT_PAIR";
    case AUTH_READY:     return "READY";
    case AUTH_ERROR:     return "ERROR";
    default:             return "?";
  }
}

// ============================================================
// SHA1 / AES helpers
// ============================================================

bool aes128EcbEncrypt(const uint8_t key[16],
                      const uint8_t in[16],
                      uint8_t out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);

  int rc = mbedtls_aes_setkey_enc(&ctx, key, 128);

  if (rc == 0) {
    rc = mbedtls_aes_crypt_ecb(
      &ctx,
      MBEDTLS_AES_ENCRYPT,
      in,
      out
    );
  }

  mbedtls_aes_free(&ctx);
  return rc == 0;
}

void calcSha1Key(const uint8_t data1[16], const uint8_t data2[16]) {
  uint8_t input[32];
  uint8_t hash[20];

  memcpy(input,      data1, 16);
  memcpy(input + 16, data2, 16);

  mbedtls_sha1_context ctx;
  mbedtls_sha1_init(&ctx);
  mbedtls_sha1_starts_ret(&ctx);
  mbedtls_sha1_update_ret(&ctx, input, sizeof(input));
  mbedtls_sha1_finish_ret(&ctx, hash);
  mbedtls_sha1_free(&ctx);

  memcpy(shaKey, hash, 16);
}

// ============================================================
// NinebotCrypto: CryptoFirst
// Same keystream for each 16-byte block.
// ============================================================

bool cryptoFirst(const uint8_t* in, uint8_t* out, size_t len) {
  uint8_t stream[16];

  if (!aes128EcbEncrypt(shaKey, FW_DATA, stream)) {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    out[i] = in[i] ^ stream[i % 16];
  }

  return true;
}

// ============================================================
// NinebotCrypto: CryptoNext
// ============================================================

bool cryptoNext(const uint8_t* in,
                uint8_t* out,
                size_t len,
                uint32_t counter) {
  uint8_t aesEncData[16] = {0};

  aesEncData[0] = 1;
  aesEncData[1] = (uint8_t)((counter >> 24) & 0xFF);
  aesEncData[2] = (uint8_t)((counter >> 16) & 0xFF);
  aesEncData[3] = (uint8_t)((counter >>  8) & 0xFF);
  aesEncData[4] = (uint8_t)( counter        & 0xFF);

  // Protocol uses first 8 bytes of BLE random data here
  memcpy(aesEncData + 5, randomBleData, 8);

  aesEncData[15] = 0;

  size_t remaining = len;
  size_t offset = 0;

  while (remaining > 0) {
    aesEncData[15]++;

    uint8_t stream[16];
    if (!aes128EcbEncrypt(shaKey, aesEncData, stream)) {
      return false;
    }

    size_t blockLen = remaining <= 16 ? remaining : 16;

    for (size_t i = 0; i < blockLen; ++i) {
      out[offset + i] = in[offset + i] ^ stream[i];
    }

    remaining -= blockLen;
    offset += blockLen;
  }

  return true;
}

// ============================================================
// CRC First
// ============================================================

void calcCrcFirst(const uint8_t* payload,
                  size_t payloadLen,
                  uint8_t crcOut[2]) {
  uint32_t sum = 0;

  for (size_t i = 0; i < payloadLen; ++i) {
    sum += payload[i];
  }

  uint16_t crc = (uint16_t)(~sum);

  crcOut[0] = crc & 0xFF;
  crcOut[1] = (crc >> 8) & 0xFF;
}

// ============================================================
// CRC Next
// Independent implementation of the chained 4-byte authenticator.
// ============================================================

bool calcCrcNext(const uint8_t* plain,
                 size_t plainLen,
                 uint32_t counter,
                 uint8_t crcOut[4]) {
  if (plainLen < 3) return false;

  const size_t payloadLen = plainLen - 3;

  uint8_t aesEncData[16] = {0};
  aesEncData[0] = 89;
  aesEncData[1] = (uint8_t)((counter >> 24) & 0xFF);
  aesEncData[2] = (uint8_t)((counter >> 16) & 0xFF);
  aesEncData[3] = (uint8_t)((counter >>  8) & 0xFF);
  aesEncData[4] = (uint8_t)( counter        & 0xFF);

  memcpy(aesEncData + 5, randomBleData, 8);
  aesEncData[15] = (uint8_t)payloadLen;

  uint8_t chain[16];
  if (!aes128EcbEncrypt(shaKey, aesEncData, chain)) {
    return false;
  }

  // Mix 3-byte clear header into CBC-MAC chain
  uint8_t block[16] = {0};

  block[0] = plain[0] ^ chain[0];
  block[1] = plain[1] ^ chain[1];
  block[2] = plain[2] ^ chain[2];

  for (int i = 3; i < 16; ++i) {
    block[i] = chain[i];
  }

  if (!aes128EcbEncrypt(shaKey, block, chain)) {
    return false;
  }

  // Mix plaintext payload in blocks
  size_t remaining = payloadLen;
  size_t offset = 3;

  while (remaining > 0) {
    size_t blockLen = remaining <= 16 ? remaining : 16;

    memset(block, 0, sizeof(block));

    for (size_t i = 0; i < blockLen; ++i) {
      block[i] = plain[offset + i];
    }

    for (int i = 0; i < 16; ++i) {
      block[i] ^= chain[i];
    }

    if (!aes128EcbEncrypt(shaKey, block, chain)) {
      return false;
    }

    remaining -= blockLen;
    offset += blockLen;
  }

  // Final mask
  memset(aesEncData, 0, sizeof(aesEncData));

  aesEncData[0] = 1;
  aesEncData[1] = (uint8_t)((counter >> 24) & 0xFF);
  aesEncData[2] = (uint8_t)((counter >> 16) & 0xFF);
  aesEncData[3] = (uint8_t)((counter >>  8) & 0xFF);
  aesEncData[4] = (uint8_t)( counter        & 0xFF);

  memcpy(aesEncData + 5, randomBleData, 8);
  aesEncData[15] = 0;

  uint8_t mask[16];

  if (!aes128EcbEncrypt(shaKey, aesEncData, mask)) {
    return false;
  }

  for (int i = 0; i < 4; ++i) {
    crcOut[i] = mask[i] ^ chain[i];
  }

  return true;
}

// ============================================================
// Encrypt complete Ninebot packet
//
// Plain packet:
//   5A A5 LEN SRC DST CMD ARG DATA...
//
// Output:
//   clear header 3 bytes +
//   encrypted bytes from SRC onward +
//   six crypto trailer bytes
// ============================================================

size_t encryptPacket(const uint8_t* plain,
                     size_t plainLen,
                     uint8_t* out,
                     size_t outMax) {
  if (plainLen < 7) return 0;

  const size_t payloadLen = plainLen - 3;
  const size_t encLen = plainLen + 6;

  if (outMax < encLen) return 0;

  out[0] = plain[0];
  out[1] = plain[1];
  out[2] = plain[2];

  if (msgIt == 0) {
    uint8_t crc[2];

    calcCrcFirst(plain + 3, payloadLen, crc);

    if (!cryptoFirst(plain + 3, out + 3, payloadLen)) {
      return 0;
    }

    out[payloadLen + 3] = 0x00;
    out[payloadLen + 4] = 0x00;
    out[payloadLen + 5] = crc[0];
    out[payloadLen + 6] = crc[1];
    out[payloadLen + 7] = 0x00;
    out[payloadLen + 8] = 0x00;

    msgIt++;
  } else {
    msgIt++;

    uint8_t crc[4];

    if (!calcCrcNext(plain, plainLen, msgIt, crc)) {
      return 0;
    }

    if (!cryptoNext(plain + 3, out + 3, payloadLen, msgIt)) {
      return 0;
    }

    out[payloadLen + 3] = crc[0];
    out[payloadLen + 4] = crc[1];
    out[payloadLen + 5] = crc[2];
    out[payloadLen + 6] = crc[3];

    out[payloadLen + 7] = (uint8_t)((msgIt >> 8) & 0xFF);
    out[payloadLen + 8] = (uint8_t)( msgIt       & 0xFF);
  }

  // remember app random when sending 5C
  if (plainLen >= 23 &&
      plain[0] == 0x5A &&
      plain[1] == 0xA5 &&
      plain[2] == 0x10 &&
      plain[3] == DEV_PHONE &&
      plain[4] == DEV_BLE &&
      plain[5] == CMD_PING &&
      plain[6] == 0x00) {
    memcpy(randomAppData, plain + 7, 16);
    haveAppKey = true;
  }

  return encLen;
}

// ============================================================
// Decrypt complete encrypted Ninebot frame
// ============================================================

size_t decryptPacket(const uint8_t* enc,
                     size_t encLen,
                     uint8_t* plain,
                     size_t plainMax,
                     uint32_t& packetCounter) {
  if (encLen < 13) return 0;
  if (enc[0] != 0x5A || enc[1] != 0xA5) return 0;

  const size_t payloadLen = encLen - 9;
  const size_t plainLen = encLen - 6;

  if (plainMax < plainLen) return 0;

  plain[0] = enc[0];
  plain[1] = enc[1];
  plain[2] = enc[2];

  uint32_t newMsgIt = msgIt;

  if ((newMsgIt & 0x00008000UL) > 0 &&
      ((enc[encLen - 2] >> 7) == 0)) {
    newMsgIt += 0x00010000UL;
  }

  newMsgIt =
      (newMsgIt & 0xFFFF0000UL) +
      ((uint32_t)enc[encLen - 2] << 8) +
      (uint32_t)enc[encLen - 1];

  packetCounter = newMsgIt;

  if (newMsgIt == 0) {
    if (!cryptoFirst(enc + 3, plain + 3, payloadLen)) {
      return 0;
    }

    // Initial 0x5B reply contains BLE random + serial
    if (plainLen >= 37 &&
        plain[2] == 0x1E &&
        plain[3] == DEV_BLE &&
        plain[4] == DEV_PHONE &&
        plain[5] == CMD_INIT) {

      memcpy(randomBleData, plain + 7, 16);
      memcpy(scooterSerial, plain + 23, 14);

      // New stage key = SHA1(name16 || randomBleData)
      calcSha1Key(scooterName16, randomBleData);

      haveInitData = true;
    }
  }
  else if (newMsgIt > msgIt) {
    if (!cryptoNext(enc + 3, plain + 3, payloadLen, newMsgIt)) {
      return 0;
    }

    // 5C01 switches to SHA1(appRandom || bleRandom)
    if (plainLen >= 7 &&
        plain[2] == 0x00 &&
        plain[3] == DEV_BLE &&
        plain[4] == DEV_PHONE &&
        plain[5] == CMD_PING &&
        plain[6] == 0x01 &&
        haveAppKey) {

      calcSha1Key(randomAppData, randomBleData);
      Serial.println("Crypto-Key auf APP_KEY + BLE_KEY umgeschaltet.");
    }

    msgIt = newMsgIt;
  }
  else {
    Serial.printf(
      "WARN: RX Counter %lu ist nicht > lokalem Counter %lu; Paket nicht entschluesselt.\n",
      (unsigned long)newMsgIt,
      (unsigned long)msgIt
    );

    return 0;
  }

  return plainLen;
}

// ============================================================
// BLE send: ALWAYS write with response.
// Fragment to 20-byte BLE UART chunks.
// ============================================================

bool bleWriteFrame(const uint8_t* data, size_t len) {
  if (!pWriteChar || !isConnected) return false;

  Serial.printf(
    "BLE TX encrypted (%u B), msgIt=%lu\n",
    (unsigned)len,
    (unsigned long)msgIt
  );

  dumpHex("TX ENC: ", data, len);

  // Nordic UART on this scooter uses ATT MTU 23 -> 20 payload bytes.
  // IMPORTANT: this function is only called from loop()/normal task context,
  // never from notifyCallback().
  size_t offset = 0;

  while (offset < len) {
    size_t chunk = len - offset;
    if (chunk > 20) chunk = 20;

    Serial.printf(
      "  TX chunk %u..%u WITH response\n",
      (unsigned)offset,
      (unsigned)(offset + chunk - 1)
    );

    pWriteChar->writeValue(
      (uint8_t*)(data + offset),
      chunk,
      true
    );

    Serial.println("  writeValue -> gesendet");

    offset += chunk;
    delay(20);
  }

  return true;
}

// ============================================================
// Generic plain packet builder + encrypted sender
// ============================================================

bool sendNinebotPacket(uint8_t dst,
                       uint8_t cmd,
                       uint8_t arg,
                       const uint8_t* data,
                       uint8_t dataLen) {
  uint8_t plain[96];
  uint8_t enc[112];

  const size_t plainLen = 7 + dataLen;

  plain[0] = 0x5A;
  plain[1] = 0xA5;
  plain[2] = dataLen;
  plain[3] = DEV_PHONE;
  plain[4] = dst;
  plain[5] = cmd;
  plain[6] = arg;

  if (dataLen > 0 && data) {
    memcpy(plain + 7, data, dataLen);
  }

  Serial.println();
  dumpHex("TX PLAIN: ", plain, plainLen);

  size_t encLen = encryptPacket(
    plain,
    plainLen,
    enc,
    sizeof(enc)
  );

  if (encLen == 0) {
    Serial.println("FEHLER: encryptPacket() fehlgeschlagen.");
    return false;
  }

  return bleWriteFrame(enc, encLen);
}

// ============================================================
// Authentication messages
// ============================================================

void createAppKey() {
  for (int i = 0; i < 16; ++i) {
    randomAppData[i] = (uint8_t)(esp_random() & 0xFF);
  }

  haveAppKey = true;

  dumpHex("APP random/key: ", randomAppData, 16);
}

void sendInit() {
  Serial.println();
  Serial.println("=== AUTH 1/3: INIT 0x5B ===");

  authState = AUTH_WAIT_INIT;
  authLastSend = millis();

  sendNinebotPacket(
    DEV_BLE,
    CMD_INIT,
    0x00,
    nullptr,
    0
  );
}

void sendPing() {
  Serial.println();
  Serial.println("=== AUTH 2/3: PING 0x5C ===");

  authState = AUTH_WAIT_PING;
  authLastSend = millis();

  sendNinebotPacket(
    DEV_BLE,
    CMD_PING,
    0x00,
    randomAppData,
    16
  );
}

void sendPair() {
  Serial.println();
  Serial.println("=== AUTH 3/3: PAIR 0x5D ===");

  authState = AUTH_WAIT_PAIR;
  authLastSend = millis();

  sendNinebotPacket(
    DEV_BLE,
    CMD_PAIR,
    0x00,
    scooterSerial,
    14
  );
}

// ============================================================
// Telemetry read
// ============================================================

void sendRead(uint8_t target, uint8_t reg, uint8_t readLen) {
  uint8_t payload[1] = { readLen };

  telemetryWaiting = true;
  telemetrySentAt = millis();

  sendNinebotPacket(
    target,
    CMD_READ,
    reg,
    payload,
    1
  );
}

bool sendDpcOn() {
  Serial.println();
  Serial.println("=== DPC SWITCH: ON ===");
  Serial.println("Logical frame: 5A A5 02 3E 20 03 76 01 00");
  Serial.println("Legacy checksum would be: 25 FF");
  Serial.println("Sending through active NinebotCrypto session ...");

  bool ok = sendNinebotPacket(
    DEV_ESC,
    CMD_WRITE,
    REG_DPC,
    DPC_ON_VALUE,
    sizeof(DPC_ON_VALUE)
  );

  if (ok) {
    dpcOnSent = true;
    lastTelemetry = millis();
    Serial.println("*** DPC ON write sent once. ***");
  } else {
    Serial.println("*** DPC ON write FAILED. ***");
  }

  return ok;
}

uint32_t littleEndianValue(const uint8_t* data, size_t len) {
  uint32_t value = 0;

  if (len > 4) len = 4;

  for (size_t i = 0; i < len; ++i) {
    value |= ((uint32_t)data[i]) << (8 * i);
  }

  return value;
}

void handleTelemetryReply(const uint8_t* plain, size_t len) {
  if (len < 7) return;

  const uint8_t src = plain[3];
  const uint8_t dst = plain[4];
  const uint8_t cmd = plain[5];
  const uint8_t reg = plain[6];
  const uint8_t dataLen = plain[2];

  if (dst != DEV_PHONE || cmd != CMD_READ_ACK) return;
  if (len < 7 + dataLen) return;

  const uint8_t* data = plain + 7;
  telemetryWaiting = false;

  if (src == DEV_ESC && reg == REG_SPEED && dataLen >= 2) {
    int16_t raw = (int16_t)littleEndianValue(data, 2);
    Serial.printf(">>> SPEED: %.1f km/h\n", raw / 10.0f);
    return;
  }

  if (src == DEV_ESC && reg == REG_ODO_LO && dataLen >= 2) {
    odoLow = (uint16_t)littleEndianValue(data, 2);
    haveOdoLow = true;

    if (haveOdoHigh) {
      uint32_t metres = ((uint32_t)odoHigh << 16) | odoLow;
      Serial.printf(">>> ODO: %.3f km\n", metres / 1000.0f);
    }
    return;
  }

  if (src == DEV_ESC && reg == REG_ODO_HI && dataLen >= 2) {
    odoHigh = (uint16_t)littleEndianValue(data, 2);
    haveOdoHigh = true;

    if (haveOdoLow) {
      uint32_t metres = ((uint32_t)odoHigh << 16) | odoLow;
      Serial.printf(">>> ODO: %.3f km\n", metres / 1000.0f);
    }
    return;
  }

  if (src == DEV_BATT && reg == REG_BAT_PCT && dataLen >= 2) {
    uint16_t pct = (uint16_t)littleEndianValue(data, 2);
    Serial.printf(">>> AKKU: %u %%\n", (unsigned)pct);
    return;
  }

  if (src == DEV_BATT && reg == REG_BAT_CUR && dataLen >= 2) {
    int16_t raw = (int16_t)littleEndianValue(data, 2);
    Serial.printf(">>> BAT CURRENT: %.2f A\n", raw / 100.0f);
    return;
  }

  if (src == DEV_BATT && reg == REG_BAT_VOLT && dataLen >= 2) {
    uint16_t raw = (uint16_t)littleEndianValue(data, 2);
    Serial.printf(">>> BAT VOLTAGE: %.2f V\n", raw / 100.0f);
    return;
  }

  if (src == DEV_BATT && reg == REG_BAT_TEMP && dataLen >= 2) {
    int t1 = (int)data[0] - 20;
    int t2 = (int)data[1] - 20;
    Serial.printf(">>> BAT TEMP: %d / %d C\n", t1, t2);
    return;
  }

  Serial.printf(">>> READ src=0x%02X reg=0x%02X: ", src, reg);
  dumpHex("", data, dataLen);
}

// ============================================================
// Plain decrypted packet handler
// ============================================================

void handlePlainPacket(const uint8_t* plain,
                       size_t len,
                       uint32_t counter) {
  Serial.println();
  Serial.printf(
    "RX decrypted, counter=%lu, local msgIt=%lu\n",
    (unsigned long)counter,
    (unsigned long)msgIt
  );

  dumpHex("RX PLAIN: ", plain, len);

  if (len < 7) return;

  uint8_t src = plain[3];
  uint8_t dst = plain[4];
  uint8_t cmd = plain[5];
  uint8_t arg = plain[6];

  Serial.printf(
    "SRC=0x%02X DST=0x%02X CMD=0x%02X ARG=0x%02X LEN=%u\n",
    src,
    dst,
    cmd,
    arg,
    plain[2]
  );

  // ----------------------------------------------------------
  // INIT response
  // ----------------------------------------------------------
  if (src == DEV_BLE &&
      dst == DEV_PHONE &&
      cmd == CMD_INIT &&
      haveInitData) {

    Serial.println();
    Serial.println("*** NinebotCrypto INIT OK ***");

    dumpHex("BLE random/key: ", randomBleData, 16);

    char serialText[15] = {0};
    memcpy(serialText, scooterSerial, 14);

    Serial.print("Scooter S/N: ");
    Serial.println(serialText);

    Serial.printf("0x5B ARG = 0x%02X\n", arg);

    // 0x5B ARG (00/01) only reports whether the scooter was already
    // paired to another app. NinebotCrypto documentation says to ignore it
    // for the protocol flow and continue with 0x5C.
    createAppKey();

    Serial.println("Plane 0x5C ausserhalb des Notify-Callbacks ein.");
    authState = AUTH_WAIT_PING;
    authLastSend = millis();
    pendingTx = TX_PING;

    return;
  }

  // ----------------------------------------------------------
  // PING/AUTH response
  // ----------------------------------------------------------
  if (src == DEV_BLE &&
      dst == DEV_PHONE &&
      cmd == CMD_PING) {

    if (arg == 0x01) {
      Serial.println();
      Serial.println("*** 0x5C01: Power/Auth bestaetigt ***");
      Serial.println("Plane 0x5D ausserhalb des Notify-Callbacks ein.");

      authState = AUTH_WAIT_PAIR;
      authLastSend = millis();
      pendingTx = TX_PAIR;
    }
    else {
      Serial.println();
      Serial.println("*** 0x5C00: Scooter wartet auf Pairing-Bestaetigung. ***");
      Serial.println(">>> JETZT die Power-Taste am Scooter kurz druecken. <<<");
      Serial.println("0x5C wird bis zur Bestaetigung erneut gesendet.");

      authState = AUTH_WAIT_PING;
      authLastSend = millis();
    }

    return;
  }

  // ----------------------------------------------------------
  // PAIR response
  // ----------------------------------------------------------
  if (src == DEV_BLE &&
      dst == DEV_PHONE &&
      cmd == CMD_PAIR) {

    if (arg == 0x01) {
      authState = AUTH_READY;
      readySince = millis();

      Serial.println();
      Serial.println("==============================================");
      Serial.println("*** AUTHENTIFIZIERUNG ERFOLGREICH - READY ***");
      Serial.println("==============================================");
      Serial.println("Starte read-only Telemetrie ...");

      telemetryWaiting = false;
      lastTelemetry = millis();
      telemetryStep = 0;

      // v6.2: schedule the requested write after the notify callback returns.
      dpcOnSent = false;
      pendingTx = TX_DPC_ON;
      Serial.println("DPC ON wurde fuer loop() vorgemerkt.");
    }
    else {
      Serial.printf("PAIR Antwort ARG=0x%02X; versuche erneut.\n", arg);
      authState = AUTH_WAIT_PAIR;
      authLastSend = millis();
    }

    return;
  }

  // ----------------------------------------------------------
  // Telemetry  // ----------------------------------------------------------
  handleTelemetryReply(plain, len);
}

// ============================================================
// Complete encrypted frame processor
// ============================================================

void processEncryptedFrame(const uint8_t* frame, size_t len) {
  Serial.println();
  dumpHex("RX ENC FULL: ", frame, len);

  uint8_t plain[192];
  uint32_t counter = 0;

  size_t plainLen = decryptPacket(
    frame,
    len,
    plain,
    sizeof(plain),
    counter
  );

  if (plainLen == 0) {
    Serial.println("RX konnte nicht entschluesselt werden.");
    return;
  }

  handlePlainPacket(plain, plainLen, counter);
}

// ============================================================
// Notify callback + fragment reassembly
// ============================================================

void notifyCallback(BLERemoteCharacteristic* c,
                    uint8_t* data,
                    size_t length,
                    bool isNotify) {
  Serial.printf(
    "RX BLE chunk: %u B\n",
    (unsigned)length
  );

  dumpHex("  RX chunk: ", data, length);

  // A new encrypted Ninebot packet starts with 5A A5
  if (length >= 2 &&
      data[0] == 0x5A &&
      data[1] == 0xA5) {

    rxLen = 0;
    rxExpected = 0;
  }

  if (rxLen + length > sizeof(rxBuffer)) {
    Serial.println("RX buffer overflow -> reset.");
    rxLen = 0;
    rxExpected = 0;
    return;
  }

  memcpy(rxBuffer + rxLen, data, length);
  rxLen += length;

  if (rxLen >= 3 && rxExpected == 0) {
    // encrypted total = dataLen + 13
    rxExpected = (size_t)rxBuffer[2] + 13;

    Serial.printf(
      "RX erwartet insgesamt %u B\n",
      (unsigned)rxExpected
    );

    if (rxExpected > sizeof(rxBuffer)) {
      Serial.println("Unplausible RX-Laenge -> reset.");
      rxLen = 0;
      rxExpected = 0;
      return;
    }
  }

  if (rxExpected > 0 && rxLen >= rxExpected) {
    processEncryptedFrame(rxBuffer, rxExpected);

    size_t leftover = rxLen - rxExpected;

    if (leftover > 0) {
      memmove(
        rxBuffer,
        rxBuffer + rxExpected,
        leftover
      );
    }

    rxLen = leftover;
    rxExpected = 0;
  }
}

// ============================================================
// BLE callbacks
// ============================================================

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* c) override {
    isConnected = true;
    Serial.println("BLE verbunden.");
  }

  void onDisconnect(BLEClient* c) override {
    isConnected = false;

    pWriteChar = nullptr;
    pNotifyChar = nullptr;

    authState = AUTH_IDLE;
    pendingTx = TX_NONE;
    dpcOnSent = false;

    telemetryWaiting = false;

    rxLen = 0;
    rxExpected = 0;

    Serial.println("BLE getrennt.");
  }
};

class AdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    bool nameOk =
      d.haveName() &&
      d.getName().rfind("NB", 0) == 0;

    bool serviceOk =
      d.haveServiceUUID() &&
      d.isAdvertisingService(SVC_UUID);

    if (nameOk || serviceOk) {
      Serial.print("Scooter gefunden: ");
      Serial.println(d.toString().c_str());

      BLEDevice::getScan()->stop();

      if (foundDevice) {
        delete foundDevice;
      }

      foundDevice = new BLEAdvertisedDevice(d);
      doConnect = true;
    }
  }
};

// ============================================================
// Enable notifications
// ============================================================

void enableNotifications() {
  pNotifyChar->registerForNotify(notifyCallback);

  Serial.println("Notify callback registriert.");

  BLERemoteDescriptor* cccd =
    pNotifyChar->getDescriptor(CCCD_UUID);

  if (cccd) {
    uint8_t enable[2] = {0x01, 0x00};

    cccd->writeValue(
      enable,
      sizeof(enable),
      true
    );

    Serial.println("CCCD 0x2902 = 01 00.");
  }
  else {
    Serial.println(
      "CCCD 0x2902 nicht separat gefunden; registerForNotify aktiv."
    );
  }
}

// ============================================================
// Reset crypto session
// ============================================================

void resetCrypto(const std::string& name) {
  memset(scooterName16, 0, sizeof(scooterName16));
  memset(randomBleData, 0, sizeof(randomBleData));
  memset(randomAppData, 0, sizeof(randomAppData));
  memset(scooterSerial, 0, sizeof(scooterSerial));

  size_t n = name.length();
  if (n > 16) n = 16;

  memcpy(
    scooterName16,
    name.data(),
    n
  );

  calcSha1Key(
    scooterName16,
    FW_DATA
  );

  msgIt = 0;

  haveInitData = false;
  haveAppKey = false;
  pendingTx = TX_NONE;
  dpcOnSent = false;

  rxLen = 0;
  rxExpected = 0;

  dumpHex("Initial SHA1-Key: ", shaKey, 16);
}

// ============================================================
// Connect
// ============================================================

bool connectScooter() {
  if (!foundDevice) return false;

  pClient = BLEDevice::createClient();

  pClient->setClientCallbacks(
    new ClientCallbacks()
  );

  if (!pClient->connect(foundDevice)) {
    Serial.println("BLE Connect fehlgeschlagen.");
    return false;
  }

  BLERemoteService* svc =
    pClient->getService(SVC_UUID);

  if (!svc) {
    Serial.println("Nordic UART Service fehlt.");
    pClient->disconnect();
    return false;
  }

  pWriteChar =
    svc->getCharacteristic(CHR_WRITE);

  pNotifyChar =
    svc->getCharacteristic(CHR_NOTIFY);

  if (!pWriteChar || !pNotifyChar) {
    Serial.println("Write/Notify Characteristic fehlt.");
    pClient->disconnect();
    return false;
  }

  Serial.printf(
    "Write: write=%s writeNoResp=%s\n",
    pWriteChar->canWrite() ? "ja" : "nein",
    pWriteChar->canWriteNoResponse() ? "ja" : "nein"
  );

  Serial.printf(
    "Notify: notify=%s indicate=%s\n",
    pNotifyChar->canNotify() ? "ja" : "nein",
    pNotifyChar->canIndicate() ? "ja" : "nein"
  );

  enableNotifications();

  std::string name = foundDevice->getName();

  Serial.print("Crypto-Name: ");
  Serial.println(name.c_str());

  resetCrypto(name);

  delay(300);

  sendInit();

  return true;
}

// ============================================================
// Deferred TX
// ============================================================

void servicePendingTx() {
  if (!isConnected) return;

  PendingTx job = pendingTx;
  if (job == TX_NONE) return;

  // Clear BEFORE sending so a notification arriving during/after the write
  // cannot accidentally cause the same job to be sent twice.
  pendingTx = TX_NONE;

  switch (job) {
    case TX_PING:
      Serial.println();
      Serial.println("[deferred] Sende 0x5C jetzt aus loop().");
      sendPing();
      break;

    case TX_PAIR:
      Serial.println();
      Serial.println("[deferred] Sende 0x5D jetzt aus loop().");
      sendPair();
      break;

    case TX_DPC_ON:
      Serial.println();
      Serial.println("[deferred] Sende DPC ON jetzt aus loop().");
      sendDpcOn();
      break;

    default:
      break;
  }
}

// ============================================================
// Auth retry / telemetry scheduler
// ============================================================

void serviceAuth() {
  if (!isConnected) return;

  unsigned long now = millis();

  // If init reply was lost, retry from a fresh BLE session is safer
  // because the crypto iterator already advanced.
  if (authState == AUTH_WAIT_INIT &&
      now - authLastSend > 5000) {

    Serial.println();
    Serial.println("INIT Timeout -> trenne und starte Session neu.");

    pClient->disconnect();
    return;
  }

  // Re-send PING every second while waiting for power confirmation.
  // Note: every message uses the chained counter, as required.
  if (authState == AUTH_WAIT_PING &&
      now - authLastSend > 1000) {

    Serial.println(
      "Warte auf 0x5C01 / Power-Taste -> sende 0x5C erneut ..."
    );

    sendPing();
    return;
  }

  if (authState == AUTH_WAIT_PAIR &&
      now - authLastSend > 1500) {

    Serial.println(
      "Warte auf 0x5D01 -> sende Pair erneut ..."
    );

    sendPair();
    return;
  }
}

void serviceTelemetry() {
  if (!isConnected || authState != AUTH_READY) {
    return;
  }

  // Do not start telemetry until the one-shot DPC write was sent.
  if (!dpcOnSent) {
    return;
  }

  unsigned long now = millis();

  // Lost telemetry response -> release the scheduler.
  // Do not blindly retransmit immediately because chained crypto counters
  // are directional and every send advances the stream.
  if (telemetryWaiting) {
    if (now - telemetrySentAt > 3000) {
      Serial.println(
        "Telemetry Timeout; fahre mit naechstem Register fort."
      );

      telemetryWaiting = false;
    }
    else {
      return;
    }
  }

  if (now - lastTelemetry < 1500) {
    return;
  }

  lastTelemetry = now;

  switch (telemetryStep) {
    case 0:
      Serial.println();
      Serial.println("--- Read battery percent BMS 0x32 ---");
      sendRead(DEV_BATT, REG_BAT_PCT, 2);
      break;

    case 1:
      Serial.println();
      Serial.println("--- Read speed ESC 0x26 ---");
      sendRead(DEV_ESC, REG_SPEED, 2);
      break;

    case 2:
      Serial.println();
      Serial.println("--- Read battery voltage BMS 0x34 ---");
      sendRead(DEV_BATT, REG_BAT_VOLT, 2);
      break;

    case 3:
      Serial.println();
      Serial.println("--- Read battery current BMS 0x33 ---");
      sendRead(DEV_BATT, REG_BAT_CUR, 2);
      break;

    case 4:
      Serial.println();
      Serial.println("--- Read battery temperatures BMS 0x35 ---");
      sendRead(DEV_BATT, REG_BAT_TEMP, 2);
      break;

    case 5:
      Serial.println();
      Serial.println("--- Read odometer low ESC 0x29 ---");
      sendRead(DEV_ESC, REG_ODO_LO, 2);
      break;

    case 6:
      Serial.println();
      Serial.println("--- Read odometer high ESC 0x2A ---");
      sendRead(DEV_ESC, REG_ODO_HI, 2);
      break;
  }

  telemetryStep =
    (telemetryStep + 1) % 7;
}

// ============================================================
// Arduino
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Ninebot G30 BLE Telemetry v6.2");
  Serial.println("NinebotCrypto + callback-safe auth + read-only telemetry");
  Serial.println();

  BLEDevice::init("");

  BLEScan* scan =
    BLEDevice::getScan();

  scan->setAdvertisedDeviceCallbacks(
    new AdvertisedCallbacks()
  );

  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  Serial.println("Scanne nach Ninebot ...");

  scan->start(10, false);
}

void loop() {
  if (doConnect) {
    doConnect = false;

    if (!connectScooter()) {
      delay(1000);
    }
  }

  // Important: perform all GATT writes from normal loop context.
  servicePendingTx();
  serviceAuth();
  serviceTelemetry();

  if (!isConnected &&
      !doConnect &&
      authState == AUTH_IDLE) {

    Serial.println("Scanne erneut ...");

    BLEDevice::getScan()->start(
      5,
      false
    );

    BLEDevice::getScan()->clearResults();
  }

  delay(10);
}