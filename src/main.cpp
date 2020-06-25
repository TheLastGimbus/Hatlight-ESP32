#include <Arduino.h>
#include <LSM303.h>

#define PIN_MAG_PLUS 23
#define PIN_MAG_MINUS 19
#define PIN_MAG_SDA 17
#define PIN_MAG_SCL 16
#define PIN_LED 4
#define NUM_LEDS 7

LSM303 compass;

void setup() {
    Serial.begin(115200);
    pinMode(PIN_MAG_MINUS, OUTPUT);
    pinMode(PIN_MAG_PLUS, OUTPUT);
    digitalWrite(PIN_MAG_MINUS, 0);
    digitalWrite(PIN_MAG_PLUS, 1);
    delay(1);

    compass.init(LSM303::device_DLHC);
    compass.enableDefault();
}

void loop() {}
