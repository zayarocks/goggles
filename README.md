# Secret Agent Goggle


## Phase 1
Control two 12-LED WS2812 rings using an ESP32-C3 SuperMini. Rings display a synchronized rotating rainbow that smoothly fades in and out, with brightness limited for USB-powered testing.

See [Kaleidoscope Eyes](learn.adafruit.com/kaleidoscope-eyes-neopixel-led-goggles-trinket-gemma) on Adafruit
*Warning:* This guide is from 2013. Some components, like the trinket, are no longer readily available. 

## Phase 2
Add a second pair of ESP32-C3 goggles that communicates wirelessly with the first. The original goggles act as the master, controlling the colors, patterns, and timing of any subsequent goggles. The second pair follows as the synchronized receiver.

