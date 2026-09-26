// ---------------------------------------------------------------------------
// goggles.ino - ONE file, flash it to every pair unchanged.
//
// Identity comes from the chip's own MAC address at runtime, so there is no
// MY_ID and no per-board SSID to edit.
//
// Runs AP + STA at once:
//   - joins the UNO Q hub ("goggles-hub") for the dashboard, if it's there
//   - always serves its own control page at http://192.168.4.1
//   - ESP-NOW peer-to-peer proximity flashing either way
//
// No hub? It falls back to a fixed channel and everything still works.
//
// REQUIRES Arduino ESP32 core 3.x. Board: ESP32S3 Dev Module.
// ---------------------------------------------------------------------------

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN    4
#define NUMPIXELS  24

// ---- hub ---------------------------------------------------------
const char* HUB_SSID = "goggles-hub";
const char* HUB_PASS = "gogglesgoggles";
#define HUB_CHANNEL    11        // the UNO Q hotspot's channel; also the
                                 // fallback when no hub is found. Keep it
                                 // OFF your router's channel. {1, 6, 11}
#define JOIN_TIMEOUT   8000      // ms before giving up and going solo
#define CONTROL_PORT   4210      // hub -> us
#define TELEMETRY_PORT 4211      // us -> hub
#define TELEMETRY_MS   2000

// ---- our own AP --------------------------------------------------
const char* AP_PASS = "lightsup123";

// ---- proximity ---------------------------------------------------
// NOTE: your old value of -99 meant "always near" - nothing is below it, so
// every packet counted as a peer arriving. -65 is a starting guess. Watch
// /status or the hub dashboard and set it from what you actually measure.
#define RSSI_NEAR  -65

#define ALERT_BLINKS   10        // was 3 (6 phases). 10 blinks = 20 phases.
#define ALERT_PHASE_MS 130

// Which pattern runs at boot. Index into patterns[] below:
// 0 Listening  1 Solid  2 Rainbow  3 Comet
// 4 Breathe    5 Twinkle  6 Alternate  7 Off
#define DEFAULT_PATTERN 2        // Rainbow swirl

Adafruit_NeoPixel px(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);
WiFiUDP   udp;

char     apName[20];
uint8_t  myMac[6];
bool     hubConnected = false;

uint8_t  cr = 0, cg = 255, cb = 0;   // default colour: green
uint8_t  bright = 40;
uint8_t  cur = 0;
uint16_t step = 0;
uint32_t lastFrame = 0;

bool     alerting   = false;
uint8_t  alertPhase = 0;
uint32_t alertLast  = 0;

const uint32_t BEACON_MS = 400;
const uint32_t AWAY_MS   = 6000;
const uint32_t REPEAT_MS = 10000;

uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint32_t lastBeacon = 0;
uint32_t lastAlert  = 0;
volatile uint32_t lastHeard = 0;
volatile bool present = false;
volatile int  lastRssi = -99;
volatile uint8_t peerMac[6] = {0};

// Magic byte so we ignore stray ESP-NOW traffic that isn't ours.
#define MAGIC 0x67

// ---- helpers ----------------------------------------------------

bool frameDue(uint16_t ms) {
  if (millis() - lastFrame < ms) return false;
  lastFrame = millis();
  return true;
}

uint32_t fade(uint32_t c, uint8_t amt) {
  uint8_t r = c >> 16, g = c >> 8, b = c;
  return px.Color((r * amt) >> 8, (g * amt) >> 8, (b * amt) >> 8);
}

void macToStr(const uint8_t* mac, char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---- patterns ---------------------------------------------------

void listening() {
  if (!frameDue(20)) return;

  if (present) {                    // peer in range: hold steady
    px.setBrightness(bright);
    px.fill(px.Color(cr, cg, cb));
    px.show();
    return;
  }

  float phase = (step % 200) / 200.0 * TWO_PI;
  float level = sin(phase - HALF_PI) * 0.5 + 0.5;
  px.setBrightness(bright * (0.15 + 0.85 * level));
  px.fill(px.Color(cr, cg, cb));
  px.show();
  step++;
}

void solid() {
  if (!frameDue(50)) return;
  px.fill(px.Color(cr, cg, cb));
  px.show();
}

void rainbow() {
  if (!frameDue(20)) return;
  for (int i = 0; i < NUMPIXELS; i++)
    px.setPixelColor(i, px.gamma32(px.ColorHSV(step * 512 + i * 65536L / NUMPIXELS)));
  px.show();
  step++;
}

void comet() {
  if (!frameDue(40)) return;
  for (int i = 0; i < NUMPIXELS; i++)
    px.setPixelColor(i, fade(px.getPixelColor(i), 150));
  px.setPixelColor(step % NUMPIXELS, px.Color(cr, cg, cb));
  px.show();
  step++;
}

void breathe() {
  if (!frameDue(20)) return;
  float phase = (step % 180) / 180.0 * TWO_PI;
  px.setBrightness((sin(phase - HALF_PI) * 0.5 + 0.5) * bright);
  px.fill(px.Color(cr, cg, cb));
  px.show();
  step++;
}

void twinkle() {
  if (!frameDue(60)) return;
  for (int i = 0; i < NUMPIXELS; i++)
    px.setPixelColor(i, fade(px.getPixelColor(i), 205));
  px.setPixelColor(random(NUMPIXELS), px.Color(cr, cg, cb));
  px.show();
}

void alternate() {
  if (!frameDue(400)) return;
  px.clear();
  uint8_t half = NUMPIXELS / 2;
  for (uint8_t i = 0; i < half; i++)
    px.setPixelColor((step % 2 ? i + half : i), px.Color(cr, cg, cb));
  px.show();
  step++;
}

void blank() {
  if (!frameDue(200)) return;
  px.clear();
  px.show();
}

// ---- the table --------------------------------------------------

struct Pattern {
  const char* name;
  void (*run)();
};

const Pattern patterns[] = {
  { "Listening", listening },
  { "Solid",     solid     },
  { "Rainbow",   rainbow   },
  { "Comet",     comet     },
  { "Breathe",   breathe   },
  { "Twinkle",   twinkle   },
  { "Alternate", alternate },
  { "Off",       blank     },
};
const uint8_t NUM_PATTERNS = sizeof(patterns) / sizeof(patterns[0]);

void setPattern(uint8_t p) {
  cur  = p % NUM_PATTERNS;
  step = 0;
  lastFrame = 0;
  px.setBrightness(bright);
  px.clear();
}

// ---- alert flash ------------------------------------------------

void triggerFlash() {
  alerting   = true;
  alertPhase = 0;
  alertLast  = 0;
  px.setBrightness(bright);
}

void alertFlash() {
  if (millis() - alertLast < ALERT_PHASE_MS) return;
  alertLast = millis();

  if (alertPhase % 2 == 0) {
    for (int i = 0; i < NUMPIXELS; i++)
      px.setPixelColor(i, px.gamma32(
        px.ColorHSV(alertPhase * 9000 + i * 65536L / NUMPIXELS)));
  } else {
    px.clear();
  }
  px.show();

  if (++alertPhase >= ALERT_BLINKS * 2) {
    alerting = false;
    px.setBrightness(bright);
  }
}

// ---- telemetry to the hub ---------------------------------------

void sendToHub(const char* line) {
  if (!hubConnected) return;
  udp.beginPacket(WiFi.gatewayIP(), TELEMETRY_PORT);   // hub == our gateway
  udp.print(line);
  udp.endPacket();
}

// ---- ESP-NOW ----------------------------------------------------

void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < 2 || data[0] != MAGIC) return;

  int rssi = info->rx_ctrl->rssi;
  lastRssi = rssi;
  memcpy((void*)peerMac, info->src_addr, 6);

  // data[1] is the command: 0 = beacon, 1 = "flash now"
  if (data[1] == 1) {
    if (rssi >= RSSI_NEAR - 10) triggerFlash();   // slack so the told-to-flash
    return;                                       // side isn't over-strict
  }

  if (rssi < RSSI_NEAR) return;

  bool wasPresent = present;
  present   = true;
  lastHeard = millis();

  // Signal strength between two heads isn't symmetric - whoever notices
  // first tells the other, so they flash together instead of seconds apart.
  if (!wasPresent) {
    uint8_t payload[2] = { MAGIC, 1 };
    esp_now_send(broadcastMac, payload, sizeof(payload));
  }
}

void onEspNowSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  static uint8_t n = 0;
  if (++n % 10) return;                 // every 10th only
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "tx ok" : "tx FAIL");
}

void sendBeacon() {
  uint8_t payload[2] = { MAGIC, 0 };
  esp_now_send(broadcastMac, payload, sizeof(payload));
}

// ---- commands from the hub --------------------------------------
//   P<n>       set pattern
//   F          flash now
//   B<n>       brightness
//   C<rrggbb>  colour

void pollHub() {
  int size = udp.parsePacket();
  if (size <= 0) return;

  char buf[32] = {0};
  int n = udp.read(buf, sizeof(buf) - 1);
  if (n < 1) return;

  switch (buf[0]) {
    case 'P': setPattern(atoi(buf + 1)); break;
    case 'F': triggerFlash();            break;
    case 'B': bright = constrain(atoi(buf + 1), 0, 120);
              px.setBrightness(bright);  break;
    case 'C': if (n >= 7) {
                char h[3] = {0};
                h[0] = buf[1]; h[1] = buf[2]; cr = strtol(h, NULL, 16);
                h[0] = buf[3]; h[1] = buf[4]; cg = strtol(h, NULL, 16);
                h[0] = buf[5]; h[1] = buf[6]; cb = strtol(h, NULL, 16);
              }
              break;
  }
  Serial.printf("hub: %s\n", buf);
}

// ---- web page ---------------------------------------------------

const char HEAD[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Goggles</title><style>
 body{background:#0e0e10;color:#eee;font-family:system-ui,sans-serif;
      text-align:center;padding:24px;margin:0}
 h2{font-weight:500}
 button{font-size:16px;padding:13px 20px;margin:5px;border:0;
        border-radius:10px;background:#2a2a2f;color:#eee}
 button:active{background:#3d3d44}
 .alert{background:#4a2a5f}
 input[type=color]{width:130px;height:64px;border:0;background:none}
 input[type=range]{width:88%;height:38px}
 p{color:#888;font-size:14px;margin-top:24px}
 #s{color:#7a7a84;font-family:ui-monospace,monospace;font-size:13px}
</style></head><body>
<h2>goggles</h2>
<input type="color" id="c" value="#00ff00" onchange="col()">
<div>
)HTML";

const char TAIL[] PROGMEM = R"HTML(
</div>
<div><button class="alert" onclick="fetch('/flash')">Test flash</button></div>
<p>Brightness</p>
<input type="range" min="0" max="120" value="40" id="b" oninput="br()">
<p id="s">&nbsp;</p>
<script>
 function col(){var v=document.getElementById('c').value;
   fetch('/set?r='+parseInt(v.substr(1,2),16)
        +'&g='+parseInt(v.substr(3,2),16)
        +'&b='+parseInt(v.substr(5,2),16));}
 function br(){fetch('/bright?v='+document.getElementById('b').value);}
 function m(x){fetch('/mode?m='+x);}
 async function st(){
   try{document.getElementById('s').textContent =
     await (await fetch('/status')).text();}catch(e){}
 }
 st(); setInterval(st, 1000);
</script></body></html>
)HTML";

void handleRoot() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send_P(200, "text/html", HEAD);

  for (uint8_t i = 0; i < NUM_PATTERNS; i++) {
    String b = "<button onclick=\"m(" + String(i) + ")\">" + patterns[i].name + "</button>";
    server.sendContent(b);
  }

  server.sendContent_P(TAIL);
  server.sendContent("");
}

// ---- setup / loop -----------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();

  px.begin();
  px.setBrightness(bright);
  px.clear();
  px.show();

  // --- identity from our own MAC. This is what makes one file work on
  // --- every board: nothing here is hand-edited per device.
  WiFi.mode(WIFI_AP_STA);
  WiFi.macAddress(myMac);
  sprintf(apName, "goggles-%02X%02X", myMac[4], myMac[5]);
  Serial.printf("boot, I am %s\n", apName);

  // --- try the hub. Joining it also fixes our channel, and in AP_STA mode
  // --- our own AP follows along, so ESP-NOW stays on one channel.
  WiFi.begin(HUB_SSID, HUB_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < JOIN_TIMEOUT)
    delay(100);

  uint8_t chan = HUB_CHANNEL;
  if (WiFi.status() == WL_CONNECTED) {
    hubConnected = true;
    chan = WiFi.channel();
    udp.begin(CONTROL_PORT);
    Serial.print("hub found, ip ");
    Serial.print(WiFi.localIP());
    Serial.printf(" channel %d\n", chan);
  } else {
    WiFi.disconnect();
    esp_wifi_set_channel(HUB_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.println("no hub - standalone");
  }

  WiFi.softAP(apName, AP_PASS, chan);
  WiFi.setTxPower(WIFI_POWER_11dBm);   // deliberately short range: compresses
                                       // RSSI so proximity actually gates
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  // Station power-save naps the radio between beacons and silently drops
  // incoming ESP-NOW packets. Symptom is detection that works *sometimes*.
  WiFi.setSleep(false);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    esp_now_register_send_cb(onEspNowSent);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, broadcastMac, 6);
    peer.channel = 0;                                  // 0 = current channel
    peer.ifidx   = hubConnected ? WIFI_IF_STA : WIFI_IF_AP;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) == ESP_OK) Serial.println("esp-now ready");
    else                                   Serial.println("add_peer FAILED");
  } else {
    Serial.println("esp-now init FAILED");
  }

  server.on("/", handleRoot);

  server.on("/set", []() {
    cr = server.arg("r").toInt();
    cg = server.arg("g").toInt();
    cb = server.arg("b").toInt();
    server.send(200, "text/plain", "ok");
  });

  server.on("/bright", []() {
    bright = server.arg("v").toInt();
    px.setBrightness(bright);
    server.send(200, "text/plain", "ok");
  });

  server.on("/mode", []() {
    setPattern(server.arg("m").toInt());
    server.send(200, "text/plain", "ok");
  });

  server.on("/flash", []() {
    triggerFlash();
    server.send(200, "text/plain", "ok");
  });

  server.on("/status", []() {
    String s = present ? "nearby" : "alone";
    s += "  rssi " + String(lastRssi) + "  (fires at " + String(RSSI_NEAR) + ")";
    s += hubConnected ? "  [hub]" : "  [solo]";
    server.send(200, "text/plain", s);
  });

  server.begin();
  Serial.println("server started");

  setPattern(DEFAULT_PATTERN);
}

void loop() {
  server.handleClient();
  if (hubConnected) pollHub();

  if (millis() - lastBeacon > BEACON_MS) {
    lastBeacon = millis();
    sendBeacon();
  }

  if (present && millis() - lastHeard > AWAY_MS) {
    present = false;
    Serial.println("peer gone");
  }

  if (present && millis() - lastAlert > REPEAT_MS) {
    lastAlert = millis();
    Serial.printf("flash, rssi %d\n", lastRssi);
    triggerFlash();
  }

  static uint32_t lastTelemetry = 0;
  if (hubConnected && millis() - lastTelemetry > TELEMETRY_MS) {
    lastTelemetry = millis();
    char self[18], other[18], line[96];
    macToStr(myMac, self);
    macToStr((const uint8_t*)peerMac, other);
    sprintf(line, "HELLO %s %d %d %02X%02X%02X", self, cur, bright, cr, cg, cb);
    sendToHub(line);
    if (lastRssi > -99) {
      sprintf(line, "PEER %s %s %d %d", self, other, lastRssi, present ? 1 : 0);
      sendToHub(line);
    }
  }

  if (alerting) alertFlash();
  else          patterns[cur].run();
}
