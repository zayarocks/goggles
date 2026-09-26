# Secret Agent Goggle

## Overview
This project is based on Adafruit’s [Kaleidoscope Eyes](learn.adafruit.com/kaleidoscope-eyes-neopixel-led-goggles-trinket-gemma) tutorial. Because the tutorial was originally published in 2013, some of its hardware, wiring instructions, and code are now outdated. This version modernizes the project using two WS2812 LED rings and an ESP32-C3 SuperMini.

![Demo](kaleidoscope_eyes.gif)


## Hardware
ESP32-C3 dev board — one per pair.
2x 12-pixel NeoPixel rings — WS2812B, wired in series, 24 pixels total
5V power — USB pack or LiPo. Not the 3V3 pin

### Wiring

```
ESP32-C3 GPIO4  ──►  Ring 1 DI
Ring 1 DO       ──►  Ring 2 DI
5V              ──►  both rings VCC
GND             ──►  both rings GND  AND  ESP32-C3 GND
```

The shared ground between the board and the LEDs is not optional. Without it
the data line has no voltage reference and the strand stays dark no matter what
else is correct.

Optional but recommended: a 300–500Ω resistor in the data line, and a 1000µF
capacitor across the power rails at the first pixel.


