#include "led.h"
#include <Adafruit_NeoPixel.h>

namespace {
const int RGB_PIN = 38;
const uint8_t RGB_ORDER = NEO_RGB;
Adafruit_NeoPixel rgbLed(1, RGB_PIN, RGB_ORDER + NEO_KHZ800);
}

void initLed() {
  rgbLed.begin();
  rgbLed.show();
  setRgbColor(64, 0, 0);
}

void setRgbColor(uint8_t r, uint8_t g, uint8_t b) {
  rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
  rgbLed.show();
}
