#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <BleKeyboard.h>
#include <M5Chain.h>
#include <Preferences.h>

#if defined(CONFIG_BT_BLUEDROID_ENABLED)
#include <esp_gap_ble_api.h>
#endif

namespace {

constexpr uint8_t kAtomLedPin = 27;
constexpr uint8_t kAtomLedCount = 1;
constexpr uint8_t kAtomButtonPin = 39;

constexpr int kChainRxPin = 32;
constexpr int kChainTxPin = 26;
constexpr uint32_t kChainBaud = 115200;

constexpr uint8_t kMaxSensors = 4;
constexpr uint32_t kSensorScanIntervalMs = 3000;
constexpr uint32_t kSensorReadIntervalMs = 10;
constexpr uint32_t kSensorLedUpdateIntervalMs = 250;
constexpr uint32_t kLedBlinkIntervalMs = 250;
constexpr uint32_t kDiagnosticPrintIntervalMs = 2000;
constexpr uint32_t kDebounceMs = 20;
constexpr uint32_t kDoubleClickWindowMs = 250;
constexpr uint32_t kCalibrationHoldMs = 5000;
constexpr uint32_t kPairingResetHoldMs = 10000;
constexpr uint32_t kConnectedIdleLedDelayMs = 10000;
constexpr uint32_t kTacticalKeyRepeatFastMs = 35;
constexpr uint32_t kTacticalKeyRepeatSlowMs = 100;

constexpr int16_t kAxisMin = -100;
constexpr int16_t kAxisMax = 100;
constexpr int16_t kAxisDeadzone = 2;
constexpr float kAdcCountsPerDegree = 4096.0f / 360.0f;
constexpr float kDefaultTravelDegrees = 45.0f;
constexpr int16_t kTravelAdcCounts = static_cast<int16_t>(kDefaultTravelDegrees * kAdcCountsPerDegree + 0.5f);
constexpr float kTacticalDeadzoneDegrees = 30.0f;
constexpr int16_t kTacticalKeyDeadzone =
    static_cast<int16_t>((kTacticalDeadzoneDegrees * 100.0f / kDefaultTravelDegrees) + 0.5f);

constexpr bool kAxisInvert[kMaxSensors] = {
    false,  // A1 / boat 1
    false,  // A2 / boat 2
    false,  // A3 / boat 3
    false,  // A4 / boat 4
};

struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

constexpr Rgb kSensorBaseColors[kMaxSensors] = {
    {255, 0, 0},    // A1: red
    {0, 255, 0},    // A2: green
    {255, 255, 0},  // A3: yellow
    {0, 0, 255},    // A4: blue
};

struct TacticalKeyPair {
  uint8_t left;
  uint8_t right;
};

constexpr TacticalKeyPair kTacticalKeys[kMaxSensors] = {
    {KEY_RIGHT_ARROW, KEY_LEFT_ARROW},  // A1
    {'x', 'v'},                         // A2
    {0, 0},
    {0, 0},
};

enum class Mode {
  Normal,
  AngleCalibration,
  PairingReset,
};

enum class ButtonEvent {
  None,
  SingleClick,
  DoubleClick,
  Hold5s,
  Hold10s,
};

Adafruit_NeoPixel atomLed(kAtomLedCount, kAtomLedPin, NEO_GRB + NEO_KHZ800);
Chain M5Chain;
BleKeyboard bleKeyboard("Tactical Sailing Boats", "M5Stack", 100);
Preferences prefs;

Mode mode = Mode::Normal;
uint16_t sensorIds[kMaxSensors] = {0};
uint16_t sensorCenters[kMaxSensors] = {2048, 2048, 2048, 2048};
uint16_t sensorRaw[kMaxSensors] = {2048, 2048, 2048, 2048};
uint8_t sensorCount = 0;

bool lastBleConnected = false;
bool bleEverConnected = false;
uint32_t bleConnectedAtMs = 0;
uint32_t lastSensorScanMs = 0;
uint32_t lastSensorReadMs = 0;
uint32_t lastSensorLedUpdateMs = 0;
uint32_t lastBlinkMs = 0;
uint32_t lastCalibrationRescanMs = 0;
uint32_t lastDiagnosticPrintMs = 0;
uint32_t lastKeyRepeatMs[kMaxSensors] = {0};
int8_t lastKeyDirection[kMaxSensors] = {0};
bool blinkOn = false;
bool ledRefreshDue = true;
bool gameActive = false;

bool stableButtonPressed = false;
bool lastRawButtonPressed = false;
uint32_t lastButtonRawChangeMs = 0;
uint32_t buttonPressedAtMs = 0;
uint32_t lastClickMs = 0;
uint8_t clickCount = 0;
bool hold5Reported = false;
bool hold10Reported = false;

uint32_t color(Rgb rgb) {
  return atomLed.Color(rgb.r, rgb.g, rgb.b);
}

void setAtomLed(Rgb rgb) {
  atomLed.setPixelColor(0, color(rgb));
  atomLed.show();
}

bool setSensorLed(uint16_t id, Rgb rgb) {
  uint8_t operationStatus = 0;
  uint8_t payload[3] = {rgb.r, rgb.g, rgb.b};
  chain_status_t status = M5Chain.setRGBLight(id, 100, &operationStatus);
  if (status != CHAIN_OK || !operationStatus) {
    Serial.printf("Angle LED brightness failed id=%u status=%u op=%u\r\n", id, status, operationStatus);
    return false;
  }

  operationStatus = 0;
  status = M5Chain.setRGBValue(id, 0, 1, payload, sizeof(payload), &operationStatus);
  if (status != CHAIN_OK || !operationStatus) {
    Serial.printf("Angle LED color failed id=%u status=%u op=%u\r\n", id, status, operationStatus);
    return false;
  }

  return true;
}

void setAllSensorLeds(Rgb rgb) {
  for (uint8_t i = 0; i < sensorCount; ++i) {
    setSensorLed(sensorIds[i], rgb);
  }
}

void sensorLedSelfTest() {
  if (sensorCount == 0) {
    return;
  }

  setAllSensorLeds({255, 0, 0});
  delay(250);
  setAllSensorLeds({0, 255, 0});
  delay(250);
  setAllSensorLeds({0, 0, 255});
  delay(250);
  setAllSensorLeds({0, 0, 0});
}

int16_t circularDelta12Bit(uint16_t value, uint16_t center) {
  int16_t delta = static_cast<int16_t>(value) - static_cast<int16_t>(center);
  if (delta > 2048) {
    delta -= 4096;
  } else if (delta < -2048) {
    delta += 4096;
  }
  return delta;
}

int16_t rawToAxis(uint8_t index, uint16_t value) {
  int32_t delta = circularDelta12Bit(value, sensorCenters[index]);
  if (kAxisInvert[index]) {
    delta = -delta;
  }

  int32_t mapped = (delta * 100L) / kTravelAdcCounts;
  mapped = constrain(mapped, kAxisMin, kAxisMax);
  if (abs(mapped) <= kAxisDeadzone) {
    mapped = 0;
  }
  return static_cast<int16_t>(mapped);
}

Rgb scaleColor(Rgb base, uint8_t brightness) {
  return {
      static_cast<uint8_t>((static_cast<uint16_t>(base.r) * brightness) / 255),
      static_cast<uint8_t>((static_cast<uint16_t>(base.g) * brightness) / 255),
      static_cast<uint8_t>((static_cast<uint16_t>(base.b) * brightness) / 255),
  };
}

uint8_t axisValueToBrightness(int16_t axisValue) {
  int16_t magnitude = abs(axisValue);
  magnitude = constrain(magnitude, 0, 100);
  return static_cast<uint8_t>(255 - ((255 - 51) * magnitude) / 100);
}

void updateSensorStatusLeds() {
  for (uint8_t i = 0; i < sensorCount && i < kMaxSensors; ++i) {
    int16_t axisValue = rawToAxis(i, sensorRaw[i]);
    uint8_t brightness = axisValueToBrightness(axisValue);
    setSensorLed(sensorIds[i], scaleColor(kSensorBaseColors[i], brightness));
  }
}

void loadCalibration() {
  prefs.begin("angle", true);
  for (uint8_t i = 0; i < kMaxSensors; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "c%u", i);
    sensorCenters[i] = prefs.getUShort(key, 2048);
  }
  prefs.end();
}

void saveCalibrationFromCurrentAngles() {
  prefs.begin("angle", false);
  for (uint8_t i = 0; i < sensorCount && i < kMaxSensors; ++i) {
    sensorCenters[i] = sensorRaw[i];
    char key[8];
    snprintf(key, sizeof(key), "c%u", i);
    prefs.putUShort(key, sensorCenters[i]);
  }
  prefs.end();
}

void printSensorScanResult(const char *reason) {
  Serial.printf("%s: angle sensor count=%u\r\n", reason, sensorCount);
  Serial.println("Tactical Sailing key mode: A1=Left/Right, A2=V/X");
  for (uint8_t i = 0; i < sensorCount; ++i) {
    Serial.printf("  angle[%u] id=%u raw=%u\r\n", i, sensorIds[i], sensorRaw[i]);
  }
}

void scanSensors(bool verbose = false) {
  uint8_t previousCount = sensorCount;

  if (!M5Chain.isDeviceConnected(3, 50)) {
    sensorCount = 0;
    if (verbose) {
      Serial.println("Chain scan: no device response");
    } else if (previousCount != sensorCount) {
      Serial.println("Chain sensors disconnected");
    }
    return;
  }

  uint16_t deviceCount = 0;
  chain_status_t status = M5Chain.getDeviceNum(&deviceCount, 300);
  if (status != CHAIN_OK || deviceCount == 0) {
    sensorCount = 0;
    if (verbose) {
      Serial.printf("Chain scan: getDeviceNum failed status=%u count=%u\r\n", status, deviceCount);
    } else if (previousCount != sensorCount) {
      Serial.printf("No chain devices found status=%u\r\n", status);
    }
    return;
  }

  device_list_t deviceList;
  device_info_t devices[16];
  deviceList.count = min<uint16_t>(deviceCount, 16);
  deviceList.devices = devices;

  if (!M5Chain.getDeviceList(&deviceList, 500)) {
    sensorCount = 0;
    if (verbose) {
      Serial.printf("Chain scan: getDeviceList failed, deviceCount=%u\r\n", deviceCount);
    } else if (previousCount != sensorCount) {
      Serial.println("Failed to get chain device list");
    }
    return;
  }

  sensorCount = 0;
  for (uint16_t i = 0; i < deviceList.count && sensorCount < kMaxSensors; ++i) {
    if (deviceList.devices[i].device_type == CHAIN_ANGLE_TYPE_CODE) {
      sensorIds[sensorCount++] = deviceList.devices[i].id;
    }
  }

  if (verbose || previousCount != sensorCount) {
    Serial.printf("Angle sensors found: %u / chain devices: %u\r\n", sensorCount, deviceList.count);
    Serial.println("Tactical Sailing key mode: A1=Left/Right, A2=V/X");
    for (uint8_t i = 0; i < sensorCount; ++i) {
      Serial.printf("  angle[%u] id=%u\r\n", i, sensorIds[i]);
    }
    ledRefreshDue = true;
  }
}

void readSensors() {
  uint8_t readCount = gameActive ? min<uint8_t>(sensorCount, 2) : sensorCount;
  for (uint8_t i = 0; i < readCount; ++i) {
    uint16_t value = sensorRaw[i];
    if (M5Chain.getAngle12BitAdc(sensorIds[i], &value, 10) == CHAIN_OK) {
      sensorRaw[i] = value;
    }
  }
}

uint32_t axisValueToKeyRepeatInterval(int16_t axisValue) {
  int16_t magnitude = abs(axisValue);
  magnitude = constrain(magnitude, kTacticalKeyDeadzone, 100);
  return map(magnitude, kTacticalKeyDeadzone, 100, kTacticalKeyRepeatSlowMs, kTacticalKeyRepeatFastMs);
}

void sendTacticalSailingKeys() {
  if (!gameActive || !bleKeyboard.isConnected()) {
    return;
  }

  uint32_t now = millis();
  for (uint8_t i = 0; i < sensorCount && i < 2; ++i) {
    int16_t axisValue = rawToAxis(i, sensorRaw[i]);
    int8_t direction = 0;
    if (axisValue <= -kTacticalKeyDeadzone) {
      direction = -1;
    } else if (axisValue >= kTacticalKeyDeadzone) {
      direction = 1;
    }

    if (direction == 0) {
      lastKeyDirection[i] = 0;
      continue;
    }

    uint32_t repeatInterval = axisValueToKeyRepeatInterval(axisValue);
    if (direction != lastKeyDirection[i] || (now - lastKeyRepeatMs[i]) >= repeatInterval) {
      TacticalKeyPair keys = kTacticalKeys[i];
      uint8_t key = direction < 0 ? keys.left : keys.right;
      if (key == 0) {
        continue;
      }
      bleKeyboard.write(key);
      lastKeyRepeatMs[i] = now;
      lastKeyDirection[i] = direction;
    }
  }
}

ButtonEvent updateButton() {
  bool rawPressed = digitalRead(kAtomButtonPin) == LOW;
  uint32_t now = millis();

  if (rawPressed != lastRawButtonPressed) {
    lastRawButtonPressed = rawPressed;
    lastButtonRawChangeMs = now;
  }

  if ((now - lastButtonRawChangeMs) < kDebounceMs || rawPressed == stableButtonPressed) {
    if (stableButtonPressed && !hold5Reported && (now - buttonPressedAtMs) >= kCalibrationHoldMs) {
      hold5Reported = true;
      return ButtonEvent::Hold5s;
    }
    if (stableButtonPressed && !hold10Reported && (now - buttonPressedAtMs) >= kPairingResetHoldMs) {
      hold10Reported = true;
      return ButtonEvent::Hold10s;
    }
    return ButtonEvent::None;
  }

  stableButtonPressed = rawPressed;
  if (stableButtonPressed) {
    buttonPressedAtMs = now;
    hold5Reported = false;
    hold10Reported = false;
    return ButtonEvent::None;
  }

  if (hold5Reported || hold10Reported) {
    clickCount = 0;
    return ButtonEvent::None;
  }

  if (mode == Mode::Normal) {
    clickCount = 0;
    return ButtonEvent::SingleClick;
  }

  if ((now - lastClickMs) > kDoubleClickWindowMs) {
    clickCount = 0;
  }
  lastClickMs = now;
  clickCount++;

  if (clickCount >= 2) {
    clickCount = 0;
    return ButtonEvent::DoubleClick;
  }

  return ButtonEvent::None;
}

void removeBleBonds() {
#if defined(CONFIG_BT_BLUEDROID_ENABLED)
  int deviceCount = esp_ble_get_bond_device_num();
  if (deviceCount <= 0) {
    return;
  }

  esp_ble_bond_dev_t *bondedDevices =
      static_cast<esp_ble_bond_dev_t *>(calloc(deviceCount, sizeof(esp_ble_bond_dev_t)));
  if (bondedDevices == nullptr) {
    return;
  }

  int listedDevices = deviceCount;
  if (esp_ble_get_bond_device_list(&listedDevices, bondedDevices) == ESP_OK) {
    for (int i = 0; i < listedDevices; ++i) {
      esp_ble_remove_bond_device(bondedDevices[i].bd_addr);
    }
  }

  free(bondedDevices);
#endif
}

void enterPairingReset() {
  mode = Mode::PairingReset;
  removeBleBonds();
  setAtomLed({80, 80, 80});
  delay(1000);
  ESP.restart();
}

void enterCalibrationMode() {
  Serial.println("Enter angle zero/calibration mode");
  mode = Mode::AngleCalibration;
  gameActive = false;
  scanSensors(true);
  Serial.printf("Calibration angle sensor count: %u\r\n", sensorCount);
  readSensors();
  lastBlinkMs = 0;
  lastCalibrationRescanMs = millis();
  blinkOn = false;
  ledRefreshDue = true;
}

void exitCalibrationMode() {
  Serial.println("Exit angle zero/calibration mode");
  saveCalibrationFromCurrentAngles();
  mode = Mode::Normal;
  setAtomLed({0, 80, 80});
  delay(700);
  ledRefreshDue = true;
}

void updateBleState() {
  bool connected = bleKeyboard.isConnected();
  if (connected != lastBleConnected) {
    Serial.printf("BLE state change: %s -> %s @%u\r\n",
                  lastBleConnected ? "connected" : "disconnected",
                  connected ? "connected" : "disconnected",
                  millis());
  }
  if (connected && !lastBleConnected) {
    bleEverConnected = true;
    bleConnectedAtMs = millis();
  }
  lastBleConnected = connected;
}

void updateLeds() {
  uint32_t now = millis();
  if ((now - lastBlinkMs) >= kLedBlinkIntervalMs) {
    lastBlinkMs = now;
    blinkOn = !blinkOn;
    ledRefreshDue = true;
  }

  if (mode == Mode::AngleCalibration) {
    setAtomLed(blinkOn ? Rgb{70, 0, 90} : Rgb{0, 0, 0});
    return;
  }

  if (gameActive) {
    setAtomLed({90, 0, 0});
    return;
  }

  if (!bleKeyboard.isConnected()) {
    setAtomLed(blinkOn ? Rgb{90, 55, 0} : Rgb{0, 0, 0});
    return;
  }

  if (bleEverConnected && (now - bleConnectedAtMs) >= kConnectedIdleLedDelayMs) {
    setAtomLed({0, 70, 35});
  } else {
    setAtomLed({0, 45, 90});
  }
}

void handleButtonEvent(ButtonEvent event) {
  switch (event) {
    case ButtonEvent::SingleClick:
      if (mode == Mode::Normal) {
        gameActive = !gameActive;
        memset(lastKeyDirection, 0, sizeof(lastKeyDirection));
        memset(lastKeyRepeatMs, 0, sizeof(lastKeyRepeatMs));
        Serial.printf("Game mode: %s\r\n", gameActive ? "ON" : "OFF");
      }
      break;

    case ButtonEvent::Hold5s:
      if (mode == Mode::Normal) {
        enterCalibrationMode();
      }
      break;

    case ButtonEvent::Hold10s:
      enterPairingReset();
      break;

    case ButtonEvent::DoubleClick:
      if (mode == Mode::AngleCalibration) {
        exitCalibrationMode();
      }
      break;

    case ButtonEvent::None:
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("Boat Angle Controller boot");
  Serial.printf("Chain UART: RX=%d TX=%d baud=%lu\r\n", kChainRxPin, kChainTxPin,
                static_cast<unsigned long>(kChainBaud));

  pinMode(kAtomButtonPin, INPUT);
  atomLed.begin();
  atomLed.setBrightness(80);
  setAtomLed({0, 0, 20});

  loadCalibration();

  M5Chain.begin(&Serial2, kChainBaud, kChainRxPin, kChainTxPin);
  delay(300);
  scanSensors(true);
  readSensors();
  printSensorScanResult("Boot scan");
  updateSensorStatusLeds();

  Serial.println("BLE keyboard init");
  bleKeyboard.begin();
}

void loop() {
  uint32_t now = millis();

  handleButtonEvent(updateButton());
  updateBleState();

  if (!gameActive && (now - lastSensorScanMs) >= kSensorScanIntervalMs) {
    lastSensorScanMs = now;
    scanSensors();
  }

  if (mode == Mode::AngleCalibration && sensorCount == 0 &&
      (now - lastCalibrationRescanMs) >= kSensorScanIntervalMs) {
    lastCalibrationRescanMs = now;
    scanSensors(true);
  }

  if (!gameActive && (now - lastDiagnosticPrintMs) >= kDiagnosticPrintIntervalMs) {
    lastDiagnosticPrintMs = now;
    Serial.printf("Diag: mode=%u game=%u button=%u angles=%u ble=%u\r\n", static_cast<unsigned>(mode), gameActive,
                  digitalRead(kAtomButtonPin) == LOW, sensorCount, bleKeyboard.isConnected());
  }

  if ((now - lastSensorReadMs) >= kSensorReadIntervalMs) {
    lastSensorReadMs = now;
    readSensors();
  }

  if (mode == Mode::Normal) {
    sendTacticalSailingKeys();
  }

  if (!gameActive && (now - lastSensorLedUpdateMs) >= kSensorLedUpdateIntervalMs) {
    lastSensorLedUpdateMs = now;
    updateSensorStatusLeds();
  }

  updateLeds();
  delay(2);
}
