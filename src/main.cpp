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

float _headingFiltered = 0;
LSM303 compass;
#define DEFAULT_HEADING_CALIBRATION_OFFSET 20
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
#define BLE_CHAR_COLOR_INDIVIDUAL_UUID "f4b9e311-fee0-4b8a-b677-edd617e79ee2"
#define BLE_CHAR_POWER_OFF_MINUTES_UUID "55b57383-7678-4a66-9d94-baa7dbc7b489"

// Possibly add this as other char + saved in NVS?
unsigned long lastCharEvent = 0;
#define LAST_CHAR_EVENT_SLEEP_TIMEOUT 10 * 60 * 1000  // 10 minutes
BLECharacteristic *bCharMode;
BLECharacteristic *bCharColorGeneral;
BLECharacteristic *bCharNavCompassTargetBearing;
unsigned long charNavCompassLastUpdate = 0;
#define CHAR_NAV_COMPASS_NO_UPDATE_WARNING_TIME 15 * 1000
BLECharacteristic *bCharCalibrateCompass;
#define CALIBRATE_COMPASS_TIME_MS 60000  // 1 minute
unsigned long calibrateBegin = CALIBRATE_COMPASS_TIME_MS;
BLECharacteristic *bCharCompassOffset;
BLECharacteristic *bCharMagneticDeclination;
// This is series of R, G, B colors of individual leds
// 0R, 0G, 0B, 1R, 1G, 1B etc
BLECharacteristic *bCharColorIndividual;
BLECharacteristic *bCharPowerOffMinutes;

#define MODE_BLANK 1
#define MODE_SET_COLOR_FILL 2
#define MODE_NAVIGATION_COMPASS_TARGET 3
#define MODE_SET_COLORS_INDIVIDUAL 4

/*
Calibrate compass
Run this function in loop and spin hat in all directions to get min/max
magnetometer values In future, it would be nice to save those values in eeprom
and only update them once in a while
*/
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

void powerOff(int minutes = 0) {
    Log.notice("Going to deep sleep forever...");
    BLEDevice::deinit(true);
    lightOneLed(NUM_LEDS / 2, CRGB(126, 0, 255));  // Purple
    delay(3000);
    FastLED.clear(true);
    if (minutes != 0) {
        esp_sleep_enable_timer_wakeup(minutes * 60 * 1000 * 1000);
    }
    esp_deep_sleep_start();
    Log.fatal("THIS SHOULD NEVER RUN - DEEP SLEEP DOESN'T WORK!!!");
}

class MyCharCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        lastCharEvent = millis();
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
            charNavCompassLastUpdate = millis();
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
                // WARN: Stopping calibarion by writing to this char externally
                // will not save callibration values!
                Log.notice("Stop calibration");
            }
        } else if (pCharacteristic == bCharCompassOffset) {
            Log.verbose("bCharCompassOffset");
        } else if (pCharacteristic == bCharMagneticDeclination) {
            Log.verbose("bCharMagneticDeclination");
        } else if (pCharacteristic == bCharColorIndividual) {
            Log.verbose("bCharColorIndividual");
        } else if (pCharacteristic == bCharPowerOffMinutes) {
            Log.verbose("bCharPowerOffMinutes");
            int minutes = pCharacteristic->getValue()[0];
            if (minutes == 0) {
                Log.verbose("0 minutes set - no sleep then :)");
            } else if (minutes == 255) {
                Log.verbose("255 minutes set - so sleep forever...");
                powerOff();
            } else {
                Log.verbose("%d minutes set - going to sleep for such time...");
                powerOff(minutes);
            }
        } else {
            Log.warning("Unknown Ble characteristic onWrite callback!");
        }
    }

    void onRead(BLECharacteristic *pCharacteristic) {
        lastCharEvent = millis();
    }
    void onNotify(BLECharacteristic *pCharacteristic) {
        lastCharEvent = millis();
    }
    void onStatus(BLECharacteristic *pCharacteristic, Status s, uint32_t code) {
        lastCharEvent = millis();
    }
};

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

    // This is low pass filter or some shit, i don't know
    //
    // Heading is more taken from previous heading, and new values need some
    // time to take over this makes output more stable, but also makes the whole
    // thing kinda "float" The faster loop goes, faster the new value takes
    // over, so i will set it to be really hard because in our case, loop isn't
    // stopped by any delay
    _headingFiltered = _headingFiltered * 0.95 + head * 0.05;

    return _headingFiltered;
}

// This DOES NOT work with negative values!!!
int segmentMap(int value, int min1, int max1, int min2, int max2) {
    // Limit input just in case
    if (value < min1) value = min1;
    if (value > max1) value = max1;

    double inputRange = max1 - min1;
    double outputRange = max2 - min2 + 1;

    // How much one segment aka. "step" is, in input range to output
    // For example, if your input is 0-1000 and out is 0-10, one "step" is 10+1
    double segmentSize = inputRange / outputRange;
    int segment = double(value) / segmentSize;
    if (segment > max2) segment = max2;  // Fuck it
    return segment;
}

// This is just to show that the compass is fucking working
// this will be moved to some fancy class or some shit, i just want to show it
int targetAzimuthToLed(float targetAzimuth) {
    float current = getHeadingAzimuth();
    // Get the difference between where is current azimuth and where is target
    // - minus values are on left, plus'es are on right
    float diff = targetAzimuth - current;
    if (diff < -180) {
        diff += 360;
    }
    if (diff > 180) {
        diff -= 360;
    }
    // Limit the range to only the half that's in front of us
    float visibleRange = diff;
    if (visibleRange > 90) {
        visibleRange = 90;
    }
    if (visibleRange < -90) {
        visibleRange = -90;
    }

    // Map it to + values because I skipped to many math classes to make
    // segmentMap() function work with minus values
    float uRange = map(visibleRange, -90, 90, 0, 180);
    Log.verbose("t: %D  diff: %D   vR: %D   uR: %D", targetAzimuth, diff,
                visibleRange, uRange);

    // TODO: This could possible be reason for inaccurate heading
    // I notieced that when heading back, led flips from left to right
    // correctly, but center led is not always correct not-good numbers mapping
    // could be the cause of this
    // "Few, hours, later..."
    // Yup, yes it was - because it was just a map(). Now it's working :)
    return segmentMap(uRange, 0, 180, 0, 6);
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
    bCharCompassOffset->setCallbacks(new MyCharCallbacks());
    int off = DEFAULT_HEADING_CALIBRATION_OFFSET;
    bCharCompassOffset->setValue(off);

    Log.verbose("Setting bCharMagneticDeclination...");
    bCharMagneticDeclination = bService->createCharacteristic(
        BLE_CHAR_MAGNETIC_DECLINATION_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY);
    bCharMagneticDeclination->setCallbacks(new MyCharCallbacks());
    int dec = DEFAULT_MAGNETIC_DECLINATION;
    bCharMagneticDeclination->setValue(dec);

    Log.verbose("Setting bCharColorIndividual...");
    bCharColorIndividual = bService->createCharacteristic(
        BLE_CHAR_COLOR_INDIVIDUAL_UUID, BLECharacteristic::PROPERTY_READ |
                                            BLECharacteristic::PROPERTY_WRITE |
                                            BLECharacteristic::PROPERTY_NOTIFY);
    bCharColorIndividual->setCallbacks(new MyCharCallbacks());
    uint8_t colors[21] = {
        // A nice rainbow
        255, 0,   0,    //
        255, 217, 0,    //
        72,  255, 0,    //
        0,   255, 145,  //
        0,   145, 255,  //
        72,  0,   255,  //
        255, 0,   217   //
    };
    bCharColorIndividual->setValue(colors, size_t(21));

    Log.verbose("Setting bCharPowerOffMinutes...");
    bCharPowerOffMinutes = bService->createCharacteristic(
        BLE_CHAR_POWER_OFF_MINUTES_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY);
    bCharPowerOffMinutes->setCallbacks(new MyCharCallbacks());
    int zero = 0;
    bCharPowerOffMinutes->setValue(zero);

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

    if (millis() > lastCharEvent + LAST_CHAR_EVENT_SLEEP_TIMEOUT) {
        Log.notice("Time after last ble char was touched is above %dms",
                   LAST_CHAR_EVENT_SLEEP_TIMEOUT);
        powerOff();
    }

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

            Log.trace("Saving calibration in NVS...");
            int ok = 0;
            ok +=
                NVS.setInt(NVS_COMPASS_CALIB_MAG_MIN_X, compass.m_min.x, false);
            ok +=
                NVS.setInt(NVS_COMPASS_CALIB_MAG_MIN_Y, compass.m_min.y, false);
            ok +=
                NVS.setInt(NVS_COMPASS_CALIB_MAG_MIN_Z, compass.m_min.z, false);
            ok +=
                NVS.setInt(NVS_COMPASS_CALIB_MAG_MAX_X, compass.m_max.x, false);
            ok +=
                NVS.setInt(NVS_COMPASS_CALIB_MAG_MAX_Y, compass.m_max.y, false);
            ok +=
                NVS.setInt(NVS_COMPASS_CALIB_MAG_MAX_Z, compass.m_max.z, false);
            bool cOk = NVS.commit();
            int failed = 6 - ok;
            if (failed > 0) {
                Log.error("There was %d failed calibration saves!", failed);
            }
            if (!cOk) {
                Log.error("NVS.commit() returnet false!");
            }
        }
    }

    // Log.verbose("%d", touchRead(PIN_LED));
    switch (mode) {
        case MODE_BLANK:
            lightAllLeds(CRGB::Black);
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
            // azimuth can range between 0-360
            // 360 is more than 255, which is max for 8 bit byte
            // so we need to get two of them and combine them to one 16 bit
            // of fucking course this is from stackoverflow
            uint8_t byte1 = bCharNavCompassTargetBearing->getValue()[0];
            uint8_t byte2 = bCharNavCompassTargetBearing->getValue()[1];
            uint16_t combined = ((uint16_t)byte1 << 8) | byte2;
            lightOneLed(targetAzimuthToLed(combined),
                        CRGB(bCharColorGeneral->getValue()[0],
                             bCharColorGeneral->getValue()[1],
                             bCharColorGeneral->getValue()[2]));
            if (millis() > charNavCompassLastUpdate +
                               CHAR_NAV_COMPASS_NO_UPDATE_WARNING_TIME) {
                leds[0] = CRGB::DarkRed;
                leds[NUM_LEDS - 1] = CRGB::DarkRed;
                FastLED.show();
            }
            break;
        }
        case MODE_SET_COLORS_INDIVIDUAL: {
            for (int x = 0; x < NUM_LEDS; x++) {
                // Led index * 3 because every led takes 3 values
                int charX = x * 3;
                leds[x] = CRGB(bCharColorIndividual->getValue()[charX + 0],
                               bCharColorIndividual->getValue()[charX + 1],
                               bCharColorIndividual->getValue()[charX + 2]);
            }
            FastLED.show();
            break;
        }
        default:
            Log.error("Unknown error is set! Falling back to blank!");
            lightAllLeds(CRGB::Black);
            break;
    }
}
