// ---------------------------------------------------------------------------
// LED goggles with ESP-NOW proximity detection.
//
// Each pair broadcasts a beacon a few times a second. Every pair listens.
// When another pair's signal is strong enough to mean "they're close", both
// pairs flash ten times.
//
// REQUIRES Arduino ESP32 core 3.x or newer. 
// ---------------------------------------------------------------------------

#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
#define DATA_PIN     4
#define RING         12                 // LEDs in ONE ring
#define LED_COUNT    (RING * 2)         // both rings, chained
#define BUTTON_PIN   0                  // boot button, -1 to disable

#define BRIGHT_MAX   40                 // USB-safe ceiling
#define BRIGHT_MIN   4
#define FRAME_MS     20                 // 50 fps

// ---------------------------------------------------------------------------
// Proximity tuning - this is what you'll actually spend time adjusting
// ---------------------------------------------------------------------------
#define ESPNOW_CHANNEL   6      // ALL goggles must use the same channel
#define BEACON_MS        250    // how often we announce ourselves
#define RSSI_ENTER      -60     // dBm to count as "in range" (~2-4m)
#define RSSI_EXIT       -72     // must drop below this to re-arm (hysteresis)
#define PEER_TIMEOUT_MS  3000   // silence after which a peer is gone
#define COOLDOWN_MS      30000  // min gap between triggers from one peer
#define RSSI_SMOOTHING   3      // higher = steadier, slower to react

#define FLASH_PATTERN    4      // index into PATTERNS[] - the ten blinks

Adafruit_NeoPixel rings(LED_COUNT, DATA_PIN, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

inline void setBoth(uint8_t i, uint32_t c) {
  rings.setPixelColor(i, c);
  rings.setPixelColor(i + RING, c);
}

inline void setRing(uint8_t ring, uint8_t i, uint32_t c) {
  rings.setPixelColor(i + (ring * RING), c);
}

inline uint8_t scaleBright(uint8_t v) {
  return BRIGHT_MIN + (((uint16_t)v * (BRIGHT_MAX - BRIGHT_MIN)) >> 8);
}

// ---------------------------------------------------------------------------
// Patterns. Draw one frame and return - no delay(), no show().
// ---------------------------------------------------------------------------

void rainbowBreath(uint16_t frame) {
  rings.setBrightness(scaleBright(rings.sine8(frame * 2)));
  for (uint8_t i = 0; i < RING; i++) {
    uint16_t hue = (frame * 80) + ((uint32_t)i * 65536 / RING);
    setBoth(i, rings.gamma32(rings.ColorHSV(hue)));
  }
}

void comet(uint16_t frame) {
  rings.setBrightness(BRIGHT_MAX);
  uint8_t head = (frame / 3) % RING;
  for (uint8_t i = 0; i < RING; i++) {
    uint8_t dist = (head + RING - i) % RING;
    uint8_t v = (dist < 6) ? (255 >> dist) : 0;
    setBoth(i, rings.gamma32(rings.ColorHSV(frame * 60, 220, v)));
  }
}

void duoBreath(uint16_t frame) {
  rings.setBrightness(BRIGHT_MAX);
  uint8_t a = rings.sine8(frame * 2);
  uint8_t b = 255 - a;
  uint32_t left  = rings.gamma32(rings.ColorHSV(43000, 255, scaleBright(a) * 6));
  uint32_t right = rings.gamma32(rings.ColorHSV(10000, 255, scaleBright(b) * 6));
  for (uint8_t i = 0; i < RING; i++) {
    setRing(0, i, left);
    setRing(1, i, right);
  }
}

void emberPulse(uint16_t frame) {
  rings.setBrightness(scaleBright(rings.sine8(frame)));
  for (uint8_t i = 0; i < RING; i++) {
    setBoth(i, rings.gamma32(rings.ColorHSV(5000, 255, 255)));
  }
}

// --- messages: play once, then hand back to the base pattern ---

#define BLINK_FRAMES 200        // 10 blinks x 0.4s at 50fps
void blinkTen(uint16_t frame) {
  rings.setBrightness(BRIGHT_MAX);
  bool on = ((frame / 10) % 2) == 0;
  uint32_t c = on ? rings.Color(255, 255, 255) : 0;
  for (uint8_t i = 0; i < LED_COUNT; i++) rings.setPixelColor(i, c);
}

#define ALTERNATE_FRAMES 100
void alternate(uint16_t frame) {
  rings.setBrightness(BRIGHT_MAX);
  bool flip = ((frame / 8) % 2) == 0;
  for (uint8_t i = 0; i < RING; i++) {
    setRing(0, i, flip ? rings.Color(0, 255, 180) : 0);
    setRing(1, i, flip ? 0 : rings.Color(255, 0, 120));
  }
}

// ---------------------------------------------------------------------------
// Pattern table. Add a pattern: write the function, add one line here.
// Keep the indices identical on every pair so "play 4" means the same thing.
// ---------------------------------------------------------------------------

typedef void (*PatternFn)(uint16_t frame);

struct Pattern {
  const char* name;
  PatternFn   fn;
  uint16_t    frames;   // 0 = loops forever, >0 = a message that ends
};

const Pattern PATTERNS[] = {
  { "rainbow",   rainbowBreath, 0                },   // 0
  { "comet",     comet,         0                },   // 1
  { "duo",       duoBreath,     0                },   // 2
  { "ember",     emberPulse,    0                },   // 3
  { "blink10",   blinkTen,      BLINK_FRAMES     },   // 4  <- proximity ping
  { "alternate", alternate,     ALTERNATE_FRAMES },   // 5
};
const uint8_t PATTERN_COUNT = sizeof(PATTERNS) / sizeof(PATTERNS[0]);

// ---------------------------------------------------------------------------
// Playback state
// ---------------------------------------------------------------------------

uint8_t  basePattern   = 0;
int8_t   activeMessage = -1;
uint16_t messageLeft   = 0;
uint16_t frameCounter  = 0;

void playMessage(uint8_t id) {
  if (id >= PATTERN_COUNT || PATTERNS[id].frames == 0) return;
  activeMessage = id;
  messageLeft   = PATTERNS[id].frames;
  frameCounter  = 0;
}

void setBasePattern(uint8_t id) {
  if (id >= PATTERN_COUNT || PATTERNS[id].frames != 0) return;
  basePattern  = id;
  frameCounter = 0;
}

void nextBasePattern() {
  uint8_t id = basePattern;
  do { id = (id + 1) % PATTERN_COUNT; } while (PATTERNS[id].frames != 0);
  setBasePattern(id);
}

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------

#define MAGIC       0x67
#define TYPE_BEACON 0
#define TYPE_FLASH  1

struct __attribute__((packed)) Packet {
  uint8_t magic;
  uint8_t type;
  uint8_t pattern;   // which message to play, for TYPE_FLASH
};

static const uint8_t BROADCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

// The receive callback runs in the Wi-Fi task, not our loop. Rather than
// touch shared state there, it drops everything into this ring buffer and
// loop() drains it. Avoids every race you'd otherwise have to reason about.
struct Event {
  uint8_t mac[6];
  int8_t  rssi;
  uint8_t type;
  uint8_t pattern;
};

#define EVENT_QUEUE 16
volatile Event   eventQueue[EVENT_QUEUE];
volatile uint8_t qHead = 0, qTail = 0;
portMUX_TYPE     qMux = portMUX_INITIALIZER_UNLOCKED;

void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(Packet)) return;
  const Packet* p = (const Packet*)data;
  if (p->magic != MAGIC) return;

  portENTER_CRITICAL_ISR(&qMux);
  uint8_t next = (qHead + 1) % EVENT_QUEUE;
  if (next != qTail) {                      // drop if full, never block
    memcpy((void*)eventQueue[qHead].mac, info->src_addr, 6);
    eventQueue[qHead].rssi    = info->rx_ctrl->rssi;
    eventQueue[qHead].type    = p->type;
    eventQueue[qHead].pattern = p->pattern;
    qHead = next;
  }
  portEXIT_CRITICAL_ISR(&qMux);
}

bool popEvent(Event& out) {
  bool got = false;
  portENTER_CRITICAL(&qMux);
  if (qTail != qHead) {
    memcpy(&out, (const void*)&eventQueue[qTail], sizeof(Event));
    qTail = (qTail + 1) % EVENT_QUEUE;
    got = true;
  }
  portEXIT_CRITICAL(&qMux);
  return got;
}

void broadcast(uint8_t type, uint8_t pattern) {
  Packet p = { MAGIC, type, pattern };
  esp_now_send(BROADCAST, (uint8_t*)&p, sizeof(p));
}

// ---------------------------------------------------------------------------
// Peer tracking
// ---------------------------------------------------------------------------

#define MAX_PEERS 8

struct Peer {
  uint8_t  mac[6];
  int16_t  rssi;          // smoothed
  uint32_t lastSeen;
  uint32_t lastTrigger;
  bool     inRange;
  bool     used;
};

Peer peers[MAX_PEERS];

Peer* findPeer(const uint8_t* mac) {
  int free = -1;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].used && memcmp(peers[i].mac, mac, 6) == 0) return &peers[i];
    if (!peers[i].used && free < 0) free = i;
  }
  if (free < 0) return nullptr;             // table full, ignore stranger
  memset(&peers[free], 0, sizeof(Peer));
  memcpy(peers[free].mac, mac, 6);
  peers[free].used    = true;
  peers[free].rssi    = -100;
  peers[free].inRange = false;
  return &peers[free];
}

void triggerFlash(Peer* p, bool tellThem) {
  p->lastTrigger = millis();
  p->inRange     = true;
  playMessage(FLASH_PATTERN);
  if (tellThem) broadcast(TYPE_FLASH, FLASH_PATTERN);
  Serial.printf("FLASH  %02X:%02X  rssi %d\n", p->mac[4], p->mac[5], p->rssi);
}

void handleEvent(const Event& e) {
  Peer* p = findPeer(e.mac);
  if (!p) return;

  // Exponential smoothing - raw RSSI between two heads is very noisy.
  if (p->rssi == -100) p->rssi = e.rssi;
  else p->rssi = ((p->rssi * RSSI_SMOOTHING) + e.rssi) / (RSSI_SMOOTHING + 1);
  p->lastSeen = millis();

  bool cooled = (millis() - p->lastTrigger) > COOLDOWN_MS || p->lastTrigger == 0;

  if (e.type == TYPE_FLASH) {
    // Someone told us to flash. Honour it only if they're genuinely near,
    // so a distant pair can't set off the whole room.
    if (p->rssi > RSSI_EXIT && cooled) triggerFlash(p, false);
    return;
  }

  // Beacon. Fire on the rising edge into range, then tell them to fire too -
  // signal strength is rarely symmetric, so whoever notices first wins.
  if (!p->inRange && p->rssi > RSSI_ENTER && cooled) {
    triggerFlash(p, true);
  } else if (p->inRange && p->rssi < RSSI_EXIT) {
    p->inRange = false;                     // re-arm
  }
}

void expirePeers() {
  uint32_t now = millis();
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].used && now - peers[i].lastSeen > PEER_TIMEOUT_MS) {
      peers[i].inRange = false;
      peers[i].rssi    = -100;
    }
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  rings.begin();
  rings.setBrightness(BRIGHT_MAX);
  rings.clear();
  rings.show();

#if BUTTON_PIN >= 0
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif

  // Radio. No router involved - peers must simply agree on a channel.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init failed");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t bc = {};
  memcpy(bc.peer_addr, BROADCAST, 6);
  bc.channel = ESPNOW_CHANNEL;
  bc.encrypt = false;
  esp_now_add_peer(&bc);

  Serial.print("goggles up, mac ");
  Serial.println(WiFi.macAddress());
}

void loop() {
  // --- radio work happens every pass, not just on frame boundaries
  Event e;
  while (popEvent(e)) handleEvent(e);

  static uint32_t lastBeacon = 0;
  if (millis() - lastBeacon >= BEACON_MS) {
    lastBeacon = millis();
    broadcast(TYPE_BEACON, 0);
    expirePeers();
  }

  // --- render at a fixed frame rate
  static uint32_t lastFrame = 0;
  if (millis() - lastFrame < FRAME_MS) return;
  lastFrame = millis();

#if BUTTON_PIN >= 0
  static bool     wasDown = false;
  static uint32_t downAt  = 0;
  bool isDown = (digitalRead(BUTTON_PIN) == LOW);
  if (isDown && !wasDown) {
    downAt = millis();
  } else if (!isDown && wasDown) {
    if (millis() - downAt > 600) { broadcast(TYPE_FLASH, FLASH_PATTERN);
                                   playMessage(FLASH_PATTERN); }
    else                         { nextBasePattern(); }
  }
  wasDown = isDown;
#endif

  uint8_t id = (activeMessage >= 0) ? (uint8_t)activeMessage : basePattern;
  PATTERNS[id].fn(frameCounter);
  rings.show();

  frameCounter++;

  if (activeMessage >= 0 && --messageLeft == 0) {
    activeMessage = -1;
    frameCounter  = 0;
  }
}