#include <Arduino.h>
#include <ArduinoLog.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <FastLED.h>
#include <LSM303.h>
#include <Wire.h>

#define PIN_MAG_PLUS 23
#define PIN_MAG_MINUS 19
#define PIN_MAG_SDA 17
#define PIN_MAG_SCL 16
#define PIN_LED 4
#define NUM_LEDS 7

CRGB leds[NUM_LEDS];

LSM303 compass;

#define BLE_DEVICE_NAME "Hatlight"
#define SERVICE_UUID "f344b002-83b5-4f2d-8b47-43b633299c8f"
#define BLE_CHAR_MODE_UUID "47dcc51e-f45d-4e33-964d-ec998b1f2700"
#define BLE_CHAR_COLOR_GENERAL_UUID "cd6aaefa-29d8-42ae-bd8c-fd4f654e7c66"
#define BLE_CHAR_NAV_COMPASS_TARGET_BEARING_UUID \
    "c749ff77-6401-48cd-b739-cfad6eba6f01"

BLECharacteristic *bCharMode;
BLECharacteristic *bCharColorGeneral;
BLECharacteristic *bCharNavCompassTargetBearing;

#define MODE_BLANK 1
#define MODE_SET_COLOR_FILL 2
#define MODE_NAVIGATION_COMPASS_TARGET 3


class MyCharCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        Log.verbose("Ble characteristic onWrite callback");
        if (pCharacteristic == bCharMode) {
            Log.verbose("bCharMode");
            Log.verbose("Mode: %d", pCharacteristic->getValue()[0]);
        } else if (pCharacteristic == bCharColorGeneral) {
            Log.verbose("bCharColorGeneral");
            Log.verbose(
                "New color: R=%d G=%d B=%d", pCharacteristic->getValue()[0],
                pCharacteristic->getValue()[1], pCharacteristic->getValue()[2]);
        } else if (pCharacteristic == bCharNavCompassTargetBearing) {
            Log.verbose("bCharNavCompassTargetBearing");
            Log.verbose("New heading: %d", pCharacteristic->getValue()[0]);
        } else {
            Log.warning("Unknown Ble characteristic onWrite callback!");
        }
    }
};

/*
Calibrate compass
Run this function in loop and spin hat in all directions to get min/max
magnetometer values In future, it would be nice to save those values in eeprom
and only update them once in a while
*/

void lightOneLed(int ledIndex, CRGB oneColor = CRGB::White,
                 CRGB restColor = CRGB::Black) {
    for (int x = 0; x < NUM_LEDS; x++) {
        if (x == ledIndex) {
            leds[x] = oneColor;
            continue;
        }
        leds[x] = restColor;
    }
    FastLED.show();
}

void lightAllLeds(CRGB color = CRGB::White) {
    for (int x = 0; x < NUM_LEDS; x++) {
        leds[x] = color;
    }
    FastLED.show();
}

LSM303::vector<int16_t> _janusz_calibration_min = {-578, -586, -425},
                        _janusz_calibration_max = {613, 608, 542};
LSM303::vector<int16_t> _calib_place_min = {32767, 32767, 32767},
                        _calib_place_max = {-32768, -32768, -32768};
void autoCalibrate() {
    compass.m_min.x = min(compass.m_min.x, compass.m.x);
    compass.m_min.y = min(compass.m_min.y, compass.m.y);
    compass.m_min.z = min(compass.m_min.z, compass.m.z);

    compass.m_max.x = max(compass.m_max.x, compass.m.x);
    compass.m_max.y = max(compass.m_max.y, compass.m.y);
    compass.m_max.z = max(compass.m_max.z, compass.m.z);
}

// This is TODO in future, if I every finally find method to get true heading
// that just FUCKING WORKS
// ...
// few tears and mental breakdowns later:
// Does this...
// Does this just FUCKING WORK??????
float getHeadingAzimuth() {
    compass.read();
    // This is basically copy of what is in the library, but cut off few things

    LSM303::vector<int32_t> temp_m = {compass.m.x, compass.m.y, compass.m.z};
    // subtract offset (average of min and max) from magnetometer readings
    temp_m.x -= ((int32_t)compass.m_min.x + compass.m_max.x) / 2;
    temp_m.y -= ((int32_t)compass.m_min.y + compass.m_max.y) / 2;
    temp_m.z -= ((int32_t)compass.m_min.z + compass.m_max.z) / 2;

    LSM303::vector<float> E;
    LSM303::vector_cross(&temp_m, &compass.a, &E);

    float head = atan2(E.y, E.z) * 180 / PI;
    head += 180;
    if (head < 0) {
        head += 360;
    }

    return head;
}

// This is just to show that the compass is fucking working
// this will be moved to some fancy class or some shit, i just want to show it
int targetAzimuthToLed(float targetAzimuth) {
    float current = getHeadingAzimuth();
    float diff = targetAzimuth - current;
    if (diff < -180) {
        diff += 360;
    }
    if (diff > 180) {
        diff -= 360;
    }
    float visibleRange = diff;
    if (visibleRange > 90) {
        visibleRange = 90;
    }
    if (visibleRange < -90) {
        visibleRange = -90;
    }

    return map(visibleRange, -90, 90, 0, 6);
}

// This is for logger to end every log with \n
void _printNewline(Print *_logOutput) { _logOutput->print('\n'); }

void setup() {
    Serial.begin(115200);
    Log.begin(LOG_LEVEL_VERBOSE, &Serial, true);
    Log.setSuffix(_printNewline);
    Log.notice("Initializing Hatlight...");
    Log.verbose("Init compass...");
    pinMode(PIN_MAG_MINUS, OUTPUT);
    pinMode(PIN_MAG_PLUS, OUTPUT);
    digitalWrite(PIN_MAG_MINUS, 0);
    digitalWrite(PIN_MAG_PLUS, 1);
    delay(10);

    Wire.begin(PIN_MAG_SDA, PIN_MAG_SCL);
    compass.init(LSM303::device_DLHC);
    compass.enableDefault();
    compass.read();
    if (compass.timeoutOccurred()) {
        Log.fatal("Compass not working, got timeout when trying to .read() !");
    } else {
        Log.trace("Compass working");
    }
    compass.m_max = _janusz_calibration_max;
    compass.m_min = _janusz_calibration_min;

    FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);

    Log.notice("Init Bluetooth...");
    BLEDevice::init(BLE_DEVICE_NAME);
    BLEServer *bServer = BLEDevice::createServer();
    BLEService *bService = bServer->createService(SERVICE_UUID);
    bCharMode = bService->createCharacteristic(
        BLE_CHAR_MODE_UUID, BLECharacteristic::PROPERTY_READ |
                                BLECharacteristic::PROPERTY_WRITE |
                                BLECharacteristic::PROPERTY_NOTIFY);
    bCharMode->setCallbacks(new MyCharCallbacks());
    bCharColorGeneral = bService->createCharacteristic(
        BLE_CHAR_COLOR_GENERAL_UUID, BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_WRITE |
                                         BLECharacteristic::PROPERTY_NOTIFY);
    bCharColorGeneral->setCallbacks(new MyCharCallbacks());
    bCharNavCompassTargetBearing = bService->createCharacteristic(
        BLE_CHAR_NAV_COMPASS_TARGET_BEARING_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY);
    bCharNavCompassTargetBearing->setCallbacks(new MyCharCallbacks());

    int blank = MODE_BLANK;
    bCharMode->setValue(blank);
    bService->start();
    Log.verbose("Star advertising...");
    BLEAdvertising *bAdvertising = BLEDevice::getAdvertising();
    bAdvertising->addServiceUUID(SERVICE_UUID);
    bAdvertising->setScanResponse(true);
    bAdvertising->setMinPreferred(0x06);  // functions that help with iPhone
    bAdvertising->setMinPreferred(0x12);
    bAdvertising->start();
    Log.verbose("Bluetooth working");

    Log.notice("Setup done, going to loop...");
}

void loop() {
    int mode = bCharMode->getValue()[0];
    switch (mode) {
        case MODE_BLANK:
            lightAllLeds(CRGB::Black);
            break;
        case MODE_SET_COLOR_FILL: {
            CRGB color = CRGB(bCharColorGeneral->getValue()[0],
                              bCharColorGeneral->getValue()[1],
                              bCharColorGeneral->getValue()[2]);
            lightAllLeds(color);
            break;
        }
        case MODE_NAVIGATION_COMPASS_TARGET: {
            lightOneLed(targetAzimuthToLed(
                bCharNavCompassTargetBearing->getValue()[0]));
        }
        default:
            break;
    }
}
