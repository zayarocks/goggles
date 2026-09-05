# Secret Agent Goggle

## Overview
This project is based on Adafruit’s [Kaleidoscope Eyes](learn.adafruit.com/kaleidoscope-eyes-neopixel-led-goggles-trinket-gemma) tutorial. Because the tutorial was originally published in 2013, some of its hardware, wiring instructions, and code are now outdated. 


![Demo](kaleidoscope_eyes.gif)


## Hardware 
This version modernizes the project using two WS2812 LED rings and an ESP32-C3 SuperMini for each pair of goggles.


## Development Phases


### Phase 1
Control two 12-LED WS2812 rings using an ESP32-C3 SuperMini. Code consists of multiple sketches, each in their own directory, that can be swapped out for different effects. 
*NOTE*: Only one sketch will fit at a time. 

### Phase 2
Add a second pair of ESP32-C3 goggles that communicates wirelessly with the first. The original goggles act as the master, controlling the colors, patterns, and timing of any subsequent goggles. The second pair follows as the synchronized receiver.

### Phase 3
Phase 3 turns the synchronized lights into a visual communication system. Specific sequences of colors, flashes, and movement encode predefined messages that can be sent from one pair of goggles and displayed by the other.

