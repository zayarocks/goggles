#include <Adafruit_NeoPixel.h>

#define DATA_PIN 4
#define LEDS_PER_RING 12
#define LED_COUNT 24

Adafruit_NeoPixel rings(
  LED_COUNT,
  DATA_PIN,
  NEO_GRB + NEO_KHZ800
);

void setup() {
  rings.begin();
  rings.setBrightness(20);
  rings.clear();
  rings.show();
}

void loop() {
  static uint16_t movement = 0;

  for (int pixel = 0; pixel < LEDS_PER_RING; pixel++) {
    uint16_t color =
      movement + ((uint32_t)pixel * 65536 / LEDS_PER_RING);

    // Both rings show the same smooth rainbow
    rings.setPixelColor(
      pixel,
      rings.gamma32(rings.ColorHSV(color))
    );

    rings.setPixelColor(
      pixel + LEDS_PER_RING,
      rings.gamma32(rings.ColorHSV(color))
    );
  }

  rings.show();

  movement += 100;
  delay(25);
}