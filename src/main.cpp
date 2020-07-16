#include <Arduino.h>
#include <ArduinoLog.h>
#include <ArduinoNvs.h>
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

#define NVS_COMPASS_CALIB_MAG_MIN_X "c.cal.m.min.x"
#define NVS_COMPASS_CALIB_MAG_MIN_Y "c.cal.m.min.y"
#define NVS_COMPASS_CALIB_MAG_MIN_Z "c.cal.m.min.z"
#define NVS_COMPASS_CALIB_MAG_MAX_X "c.cal.m.max.x"
#define NVS_COMPASS_CALIB_MAG_MAX_Y "c.cal.m.max.y"
#define NVS_COMPASS_CALIB_MAG_MAX_Z "c.cal.m.max.z"

CRGB leds[NUM_LEDS];

LSM303 compass;
#define DEFAULT_HEADING_CALIBRATION_OFFSET 30
#define DEFAULT_MAGNETIC_DECLINATION 6

#define BLE_DEVICE_NAME "Hatlight"
#define SERVICE_UUID "f344b002-83b5-4f2d-8b47-43b633299c8f"
#define BLE_CHAR_MODE_UUID "47dcc51e-f45d-4e33-964d-ec998b1f2700"
#define BLE_CHAR_COLOR_GENERAL_UUID "cd6aaefa-29d8-42ae-bd8c-fd4f654e7c66"
#define BLE_CHAR_NAV_COMPASS_TARGET_BEARING_UUID \
    "c749ff77-6401-48cd-b739-cfad6eba6f01"
#define BLE_CHAR_CALIBRATE_COMPASS_UUID "bc5939f5-ce5c-450f-870f-876e92d52d89"
#define BLE_CHAR_COMPASS_OFFSET_UUID "903379c1-af1d-4962-8eb9-dec102357e1b"
#define BLE_CHAR_MAGNETIC_DECLINATION_UUID \
    "d887e381-54e1-4e2b-bdf5-258b84f8c28f"

BLECharacteristic *bCharMode;
BLECharacteristic *bCharColorGeneral;
BLECharacteristic *bCharNavCompassTargetBearing;
BLECharacteristic *bCharCalibrateCompass;
#define CALIBRATE_COMPASS_TIME_MS 60000  // 1 minute
unsigned long calibrateBegin = CALIBRATE_COMPASS_TIME_MS;
// TODO: Also save this in flash
BLECharacteristic *bCharCompassOffset;
BLECharacteristic *bCharMagneticDeclination;

#define MODE_BLANK 1
#define MODE_SET_COLOR_FILL 2
#define MODE_NAVIGATION_COMPASS_TARGET 3

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
        } else if (pCharacteristic == bCharCalibrateCompass) {
            Log.verbose("bCharCalibrateCompass");
            if (pCharacteristic->getValue()[0] == 1) {
                // Reset calibration
                compass.m_min = _calib_place_min;
                compass.m_max = _calib_place_max;
                calibrateBegin = millis();
                Log.notice("Begining calibration at %d millis, for next %d ms",
                           calibrateBegin, CALIBRATE_COMPASS_TIME_MS);
            } else {
                Log.notice("Stop calibration");
            }
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
    head += bCharMagneticDeclination->getValue()[0];
    head -= bCharCompassOffset->getValue()[0];
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

    // TODO: This could possible be reason for inaccurate heading
    // I notieced that when heading back, led flips from left to right
    // correctly, but center led is not always correct not-good numbers mapping
    // could be the cause of this
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

    Log.verbose("Init NVS...");
    bool nvsOk = NVS.begin();
    if (nvsOk) {
        Log.trace("NVS working");
    } else {
        Log.error("NVS not working!");
    }

    Log.verbose("Reading compass calibration from NVS...");
    int minX = NVS.getInt(NVS_COMPASS_CALIB_MAG_MIN_X);
    int minY = NVS.getInt(NVS_COMPASS_CALIB_MAG_MIN_Y);
    int minZ = NVS.getInt(NVS_COMPASS_CALIB_MAG_MIN_Z);

    int maxX = NVS.getInt(NVS_COMPASS_CALIB_MAG_MAX_X);
    int maxY = NVS.getInt(NVS_COMPASS_CALIB_MAG_MAX_Y);
    int maxZ = NVS.getInt(NVS_COMPASS_CALIB_MAG_MAX_Z);

    // Yes, I know this can be done with !minX, but this shows more clear what i
    // want to do here
    if (minX == 0 || minY == 0 || minZ == 0 || maxX == 0 || maxY == 0 ||
        maxZ == 0) {
        Log.warning(
            "Compass calibration from NVS returned 0 in at least one val - "
            "probably not saved yet. Falling back to stock...");
        compass.m_min = _janusz_calibration_min;
        compass.m_max = _janusz_calibration_max;
    } else {
        Log.verbose("Compass calibration from NVS:");
        Log.verbose("Min X Y Z: %d %d %d", minX, minY, minZ);
        Log.verbose("Max X Y Z: %d %d %d", maxX, maxY, maxZ);
        compass.m_min = {minX, minY, minZ};
        compass.m_max = {maxX, maxY, maxZ};
    }

    FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);

    Log.trace("Init Bluetooth...");
    BLEDevice::init(BLE_DEVICE_NAME);
    BLEServer *bServer = BLEDevice::createServer();
    BLEService *bService = bServer->createService(SERVICE_UUID);

    Log.verbose("Setting bCharMode...");
    bCharMode = bService->createCharacteristic(
        BLE_CHAR_MODE_UUID, BLECharacteristic::PROPERTY_READ |
                                BLECharacteristic::PROPERTY_WRITE |
                                BLECharacteristic::PROPERTY_NOTIFY);
    bCharMode->setCallbacks(new MyCharCallbacks());
    int blank = MODE_BLANK;
    bCharMode->setValue(blank);

    Log.verbose("Setting bCharColorGeneral...");
    bCharColorGeneral = bService->createCharacteristic(
        BLE_CHAR_COLOR_GENERAL_UUID, BLECharacteristic::PROPERTY_READ |
                                         BLECharacteristic::PROPERTY_WRITE |
                                         BLECharacteristic::PROPERTY_NOTIFY);
    bCharColorGeneral->setCallbacks(new MyCharCallbacks());
    char white[3] = {255, 255, 255};
    bCharColorGeneral->setValue(white);  // White 24-bit color code

    Log.verbose("Setting bCharNavCompass...");
    bCharNavCompassTargetBearing = bService->createCharacteristic(
        BLE_CHAR_NAV_COMPASS_TARGET_BEARING_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY);
    bCharNavCompassTargetBearing->setCallbacks(new MyCharCallbacks());

    Log.verbose("Setting bCharCalibrateCompass...");
    bCharCalibrateCompass = bService->createCharacteristic(
        BLE_CHAR_CALIBRATE_COMPASS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY);
    bCharCalibrateCompass->setCallbacks(new MyCharCallbacks());
    int calib = 0;
    bCharCalibrateCompass->setValue(calib);

    Log.verbose("Setting bCharCompassOffset...");
    bCharCompassOffset = bService->createCharacteristic(
        BLE_CHAR_COMPASS_OFFSET_UUID, BLECharacteristic::PROPERTY_READ |
                                          BLECharacteristic::PROPERTY_WRITE |
                                          BLECharacteristic::PROPERTY_NOTIFY);
    int off = DEFAULT_HEADING_CALIBRATION_OFFSET;
    bCharCompassOffset->setValue(off);

    Log.verbose("Setting bCharMagneticDeclination...");
    bCharMagneticDeclination = bService->createCharacteristic(
        BLE_CHAR_MAGNETIC_DECLINATION_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY);
    int dec = DEFAULT_MAGNETIC_DECLINATION;
    bCharMagneticDeclination->setValue(dec);

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

    // Calibration
    if (bCharCalibrateCompass->getValue()[0] == 1) {
        if (millis() < calibrateBegin + CALIBRATE_COMPASS_TIME_MS) {
            lightAllLeds(CRGB::Black);
            leds[0] = CRGB::Green;
            leds[NUM_LEDS - 1] = CRGB::Blue;
            FastLED.show();
            compass.read();
            autoCalibrate();
            // Don't execute anything else meanwhile
            return;
        } else {
            // TODO: Save calibration to file
            Log.notice("End of compass magnet calibration");
            Log.verbose("m_min: %d, %d, %d", compass.m_min.x, compass.m_min.y,
                        compass.m_min.z);
            Log.verbose("m_max: %d, %d, %d", compass.m_max.x, compass.m_max.y,
                        compass.m_max.z);
            int no = 0;
            bCharCalibrateCompass->setValue(no);

            // bool minXOk = NVS.setInt(NVS_COMPASS_CALIB_MAG_MIN_X, compass.m_min.x);
            // bool minYOk = NVS.setInt(NVS_COMPASS_CALIB_MAG_MIN_Y, compass.m_min.y);
            // bool minZOk = NVS.setInt(NVS_COMPASS_CALIB_MAG_MIN_Z, compass.m_min.z);

            // bool maxXOk = NVS.setInt(NVS_COMPASS_CALIB_MAG_MAX_X, compass.m_max.x);
            // bool maxYOk = NVS.setInt(NVS_COMPASS_CALIB_MAG_MAX_Y, compass.m_max.y);
            // bool maxZOk = NVS.setInt(NVS_COMPASS_CALIB_MAG_MAX_Z, compass.m_max.z);

            Log.trace("Saving calibration in NVS...");
            int ok = 0;
            ok += NVS.setInt(NVS_COMPASS_CALIB_MAG_MIN_X, compass.m_min.x);
            ok += NVS.setInt(NVS_COMPASS_CALIB_MAG_MIN_Y, compass.m_min.y);
            ok += NVS.setInt(NVS_COMPASS_CALIB_MAG_MIN_Z, compass.m_min.z);

            ok += NVS.setInt(NVS_COMPASS_CALIB_MAG_MAX_X, compass.m_max.x);
            ok += NVS.setInt(NVS_COMPASS_CALIB_MAG_MAX_Y, compass.m_max.y);
            ok += NVS.setInt(NVS_COMPASS_CALIB_MAG_MAX_Z, compass.m_max.z);
            int failed = 6-ok;
            if(failed>0){
                Log.error("There was %d failed calibration saves!", failed);
            }
        }
    }

    switch (mode) {
        case MODE_BLANK:
            lightAllLeds(CRGB::Black);
            // Log.verbose("head: %F", getHeadingAzimuth());
            break;
        case MODE_SET_COLOR_FILL: {
            // TODO: put this char-to-CRGB into function
            CRGB color = CRGB(bCharColorGeneral->getValue()[0],
                              bCharColorGeneral->getValue()[1],
                              bCharColorGeneral->getValue()[2]);
            lightAllLeds(color);
            break;
        }
        case MODE_NAVIGATION_COMPASS_TARGET: {
            lightOneLed(
                targetAzimuthToLed(bCharNavCompassTargetBearing->getValue()[0]),
                CRGB(bCharColorGeneral->getValue()[0],
                     bCharColorGeneral->getValue()[1],
                     bCharColorGeneral->getValue()[2]));
        }
        default:
            break;
    }
}
