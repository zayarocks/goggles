#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN    4
#define NUMPIXELS  24

// ---- change these two on the second pair ----
#define MY_ID      2              // pair 2 gets 2
const char* AP_SSID = "goggles-2";        // pair 2 gets "goggles-2"

const char* AP_PASS = "lightsup123";
#define AP_CHANNEL 1
#define RSSI_NEAR  -99

Adafruit_NeoPixel px(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);

uint8_t  cr = 255, cg = 0, cb = 0;
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
  if (millis() - alertLast < 130) return;
  alertLast = millis();

  if (alertPhase % 2 == 0) {
    for (int i = 0; i < NUMPIXELS; i++)
      px.setPixelColor(i, px.gamma32(
        px.ColorHSV(alertPhase * 9000 + i * 65536L / NUMPIXELS)));
  } else {
    px.clear();
  }
  px.show();

  if (++alertPhase >= 6) {
    alerting = false;
    setPattern(cur);
  }
}

// ---- ESP-NOW ----------------------------------------------------

void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  Serial.print("rx id=");
  Serial.print(len ? data[0] : -1);
  Serial.print(" rssi=");
  Serial.println(info->rx_ctrl->rssi);

  if (len < 1 || data[0] == MY_ID) return;

  int rssi = info->rx_ctrl->rssi;
  lastRssi = rssi;
  if (rssi < RSSI_NEAR) return;

  present = true;
  lastHeard = millis();
}

void onEspNowSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  static uint8_t n = 0;
  if (++n % 10) return;                 // every 10th only
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "tx ok" : "tx FAIL");
}

void sendBeacon() {
  uint8_t payload[1] = { MY_ID };
  esp_now_send(broadcastMac, payload, sizeof(payload));
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
</style></head><body>
<h2>goggles</h2>
<input type="color" id="c" value="#ff0000" onchange="col()">
<div>
)HTML";

const char TAIL[] PROGMEM = R"HTML(
</div>
<div><button class="alert" onclick="fetch('/flash')">Test flash</button></div>
<p>Brightness</p>
<input type="range" min="0" max="120" value="40" id="b" oninput="br()">
<script>
 function col(){var v=document.getElementById('c').value;
   fetch('/set?r='+parseInt(v.substr(1,2),16)
        +'&g='+parseInt(v.substr(3,2),16)
        +'&b='+parseInt(v.substr(5,2),16));}
 function br(){fetch('/bright?v='+document.getElementById('b').value);}
 function m(x){fetch('/mode?m='+x);}
</script></body></html>
)HTML";

void handleRoot() {
  String h = FPSTR(HEAD);
  for (uint8_t i = 0; i < NUM_PATTERNS; i++)
    h += "<button onclick=\"m(" + String(i) + ")\">" + patterns[i].name + "</button>";
  h += FPSTR(TAIL);
  server.send(200, "text/html", h);
}

// ---- setup / loop -----------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.print("boot, MY_ID=");
  Serial.println(MY_ID);

  px.begin();
  px.setBrightness(bright);
  px.clear();
  px.show();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    esp_now_register_send_cb(onEspNowSent);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, broadcastMac, 6);
    peer.channel = AP_CHANNEL;
    peer.ifidx   = WIFI_IF_AP;
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
    s += "  (last rssi " + String(lastRssi) + ")";
    server.send(200, "text/plain", s);
  });

  server.begin();
  Serial.println("server started");

  setPattern(0);
}

void loop() {
  server.handleClient();

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
    Serial.print("flash, rssi ");
    Serial.println(lastRssi);
    triggerFlash();
  }

  if (alerting) alertFlash();
  else          patterns[cur].run();
}