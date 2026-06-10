#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <M5Chain.h>

constexpr uint8_t kAtomLedPin = 27;
constexpr uint8_t kAtomLedCount = 1;
constexpr uint8_t kAtomButtonPin = 39;

constexpr int kChainRxPin = 32;
constexpr int kChainTxPin = 26;
constexpr uint32_t kChainBaud = 115200;

constexpr uint32_t kBreathStepIntervalMs = 30;
constexpr uint32_t kSensorScanIntervalMs = 3000;
constexpr uint32_t kSensorReadIntervalMs = 20;
constexpr int16_t kUnlockDegrees = 10;
constexpr int16_t kCountsPerUnlockDegrees = static_cast<int16_t>(4096L * kUnlockDegrees / 360L);

Adafruit_NeoPixel atomLed(kAtomLedCount, kAtomLedPin, NEO_GRB + NEO_KHZ800);
Chain M5Chain;

bool hasSensor = false;
uint16_t sensorId = 0;
uint16_t latestRawAngle = 2048;
uint16_t blueStartRawAngle = 2048;
bool blueLocked = false;
bool lastButtonPressed = false;

uint8_t breathBrightness = 0;
int8_t breathDirection = 1;
uint32_t lastBreathStepMs = 0;
uint32_t lastSensorScanMs = 0;
uint32_t lastSensorReadMs = 0;

void setAtomLed(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  atomLed.setBrightness(brightness);
  atomLed.setPixelColor(0, atomLed.Color(r, g, b));
  atomLed.show();
}

int16_t angleDelta12Bit(uint16_t current, uint16_t start) {
  int16_t delta = static_cast<int16_t>(current) - static_cast<int16_t>(start);
  if (delta > 2048) {
    delta -= 4096;
  } else if (delta < -2048) {
    delta += 4096;
  }
  return delta;
}

void scanSensor() {
  hasSensor = false;
  sensorId = 0;

  if (!M5Chain.isDeviceConnected(3, 50)) {
    return;
  }

  uint16_t deviceCount = 0;
  if (M5Chain.getDeviceNum(&deviceCount, 300) != CHAIN_OK || deviceCount == 0) {
    return;
  }

  device_info_t devices[16];
  device_list_t deviceList;
  deviceList.count = min<uint16_t>(deviceCount, 16);
  deviceList.devices = devices;

  if (!M5Chain.getDeviceList(&deviceList, 500)) {
    return;
  }

  for (uint16_t i = 0; i < deviceList.count; ++i) {
    if (deviceList.devices[i].device_type == CHAIN_ANGLE_TYPE_CODE) {
      sensorId = deviceList.devices[i].id;
      hasSensor = true;
      return;
    }
  }
}

void updateSensor() {
  if (!hasSensor) {
    return;
  }

  uint16_t rawAngle = latestRawAngle;
  if (M5Chain.getAngle12BitAdc(sensorId, &rawAngle) == CHAIN_OK) {
    latestRawAngle = rawAngle;
  }
}

void updateBreathingLed(uint32_t now) {
  if ((now - lastBreathStepMs) < kBreathStepIntervalMs) {
    return;
  }

  lastBreathStepMs = now;
  setAtomLed(150, 50, 80, breathBrightness);

  if (breathDirection > 0 && breathBrightness >= 100) {
    breathDirection = -1;
  } else if (breathDirection < 0 && breathBrightness == 0) {
    breathDirection = 1;
  }

  breathBrightness = static_cast<uint8_t>(breathBrightness + breathDirection);
}

void setup() {
  Serial.begin(115200);
  pinMode(kAtomButtonPin, INPUT);

  atomLed.begin();
  atomLed.clear();
  atomLed.show();

  M5Chain.begin(&Serial2, kChainBaud, kChainRxPin, kChainTxPin);
  delay(300);
  scanSensor();
  updateSensor();
}

void loop() {
  uint32_t now = millis();
  bool buttonPressed = digitalRead(kAtomButtonPin) == LOW;

  if ((now - lastSensorScanMs) >= kSensorScanIntervalMs) {
    lastSensorScanMs = now;
    scanSensor();
  }

  if ((now - lastSensorReadMs) >= kSensorReadIntervalMs) {
    lastSensorReadMs = now;
    updateSensor();
  }

  if (buttonPressed && !lastButtonPressed) {
    blueLocked = true;
    blueStartRawAngle = latestRawAngle;
    setAtomLed(100, 70, 180, 100);
  }
  lastButtonPressed = buttonPressed;

  if (blueLocked) {
    setAtomLed(100, 70, 180, 100);

    int16_t delta = angleDelta12Bit(latestRawAngle, blueStartRawAngle);
    if (abs(delta) >= kCountsPerUnlockDegrees) {
      blueLocked = false;
    }
  } else {
    updateBreathingLed(now);
  }

  delay(2);
}
