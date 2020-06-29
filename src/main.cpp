#include <Arduino.h>
#include <ArduinoLog.h>
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
    if (visibleRange > 90){
        visibleRange = 90;
    }
    if (visibleRange < -90){
        visibleRange = -90;
    }

    return map(visibleRange, -90, 90, 0, 6);
}

// This is for logger to end every log with \n
void _printNewline(Print* _logOutput) { _logOutput->print('\n'); }

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

    Log.notice("Setup done, ging to loop...");
}

void loop() {
    lightOneLed(targetAzimuthToLed(0));
}
