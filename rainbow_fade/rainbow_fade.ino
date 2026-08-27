#include <Adafruit_NeoPixel.h>

#define DATA_PIN 4
#define LEDS_PER_RING 12
#define LED_COUNT 24

// Lower brightness
// Display a full rainbow
// Slowly rotate the colors
// Clearly fade brighter and darker together


Adafruit_NeoPixel rings(
  LED_COUNT,
  DATA_PIN,
  NEO_GRB + NEO_KHZ800
);

void setup() {
  rings.begin();
  rings.setBrightness(25);  // USB-safe overall limit
  rings.clear();
  rings.show();
}

void loop() {
  static uint16_t movement = 0;
  static int fade = 20;
  static int fadeDirection = 2;

  // Reverse direction at the top and bottom
  fade += fadeDirection;

  if (fade >= 255) {
    fade = 255;
    fadeDirection = -2;
  }

  if (fade <= 20) {
    fade = 20;
    fadeDirection = 2;
  }

  for (int pixel = 0; pixel < LEDS_PER_RING; pixel++) {
    uint16_t color =
      movement + ((uint32_t)pixel * 65536 / LEDS_PER_RING);

    uint32_t fadedColor = rings.gamma32(
      rings.ColorHSV(color, 255, fade)
    );

    rings.setPixelColor(pixel, fadedColor);
    rings.setPixelColor(pixel + LEDS_PER_RING, fadedColor);
  }

  rings.show();

  movement += 80;
  delay(20);
}