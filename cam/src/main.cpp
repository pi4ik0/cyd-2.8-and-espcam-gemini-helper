// ESP32-CAM (AI-Thinker, OV2640) v2
// Waits for "SHOT" over ESP-NOW, takes a photo, asks Google Gemini (free API key),
// sends the text answer back to the CYD
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_camera.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"

// ================== SETTINGS ==================
// Wi-Fi name/password and the Gemini API key are NOT in the code.
// First start (or no connection for 20 s): the CAM opens its own network "SUD-CAM"
// (password cyddisplay). Connect with a phone, open http://192.168.4.1 and fill the form.
// "gemini-flash-latest" always points to the current Flash model, so it does not
// break when Google retires old names. You can pin an exact model name instead.
#define MODEL     "gemini-flash-latest"
// Latin text only: the default TFT fonts have no Cyrillic
#define PROMPT "Solve the task or answer the question shown in the photo. Reply in English: the final answer first, then a one-line reason. Max 350 characters, no markdown."
// ==============================================

struct Pkt { uint8_t type; uint8_t idx; uint8_t total; uint8_t len; char text[230]; };
enum { PKT_SHOT = 1, PKT_STATUS = 2, PKT_ANSWER = 3, PKT_IP = 4 };
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

volatile bool shotRequested = false;
WebServer server(80);
String cfgSsid, cfgPass, apiKey;
Preferences prefs;
bool portalActive = false;

// Live view: the CYD downloads one small JPEG per request
void handleJpg() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { server.send(500, "text/plain", "cam"); return; }
  server.send_P(200, "image/jpeg", (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len >= 1 && data[0] == PKT_SHOT) shotRequested = true;
}

void sendPkt(uint8_t type, uint8_t idx, uint8_t total, const char *t, size_t n) {
  Pkt p{};
  p.type = type; p.idx = idx; p.total = total; p.len = n;
  memcpy(p.text, t, n);
  esp_now_send(BCAST, (uint8_t *)&p, 4 + n);
}

void sendStatus(const char *s) {
  sendPkt(PKT_STATUS, 0, 1, s, min(strlen(s), (size_t)60));
}

void sendAnswer(const String &s) {
  size_t n = s.length();
  if (n > 6 * 230) n = 6 * 230;
  uint8_t total = max<size_t>(1, (n + 229) / 230);
  for (uint8_t i = 0; i < total; i++) {
    size_t off = (size_t)i * 230;
    sendPkt(PKT_ANSWER, i, total, s.c_str() + off, min<size_t>(230, n - off));
    delay(30);
  }
}

// on=true: small 240x176 live view; on=false: 800x600 for the AI
void setPreview(bool on) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_framesize(s, on ? FRAMESIZE_HQVGA : FRAMESIZE_SVGA);
  s->set_quality(s, on ? 14 : 10);
  delay(200);
}

bool initCamera() {
  camera_config_t c{};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = 5;  c.pin_d1 = 18; c.pin_d2 = 19; c.pin_d3 = 21;
  c.pin_d4 = 36; c.pin_d5 = 39; c.pin_d6 = 34; c.pin_d7 = 35;
  c.pin_xclk = 0; c.pin_pclk = 22; c.pin_vsync = 25; c.pin_href = 23;
  c.pin_sccb_sda = 26; c.pin_sccb_scl = 27;
  c.pin_pwdn = 32; c.pin_reset = -1;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_SVGA;  // 800x600, enough to read text
  c.jpeg_quality = 10;            // lower number = better quality
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&c) != ESP_OK) return false;
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    // If the picture is upside down / mirrored in your case, change these:
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);
  }
  setPreview(true);
  return true;
}

// Take a photo, send it to Gemini, return the text of the answer
String askGemini() {
  if (!apiKey.length()) return "No API key set";
  setPreview(false);
  for (int i = 0; i < 3; i++) {  // let exposure settle, drop stale frames
    camera_fb_t *old = esp_camera_fb_get();
    if (old) esp_camera_fb_return(old);
    delay(120);
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { setPreview(true); return "Camera error"; }

  String pre = R"J({"contents":[{"parts":[{"inline_data":{"mime_type":"image/jpeg","data":")J";
  String suf = String(R"J("}},{"text":")J") + PROMPT + R"J("}]}],"generationConfig":{"maxOutputTokens":1500}})J";

  size_t cap = pre.length() + 4 * ((fb->len + 2) / 3) + 8 + suf.length();
  uint8_t *body = (uint8_t *)ps_malloc(cap);
  if (!body) { esp_camera_fb_return(fb); setPreview(true); return "Out of memory"; }

  memcpy(body, pre.c_str(), pre.length());
  size_t pos = pre.length(), olen = 0;
  mbedtls_base64_encode(body + pos, cap - pos, &olen, fb->buf, fb->len);
  pos += olen;
  esp_camera_fb_return(fb);  // free the frame before the network request
  setPreview(true);          // back to the small live view
  memcpy(body + pos, suf.c_str(), suf.length());
  pos += suf.length();

  sendStatus("Thinking...");

  WiFiClientSecure client;
  client.setInsecure();  // no certificate check (fine for a hobby project)
  HTTPClient http;
  http.setTimeout(45000);
  http.begin(client, "https://generativelanguage.googleapis.com/v1beta/models/" MODEL ":generateContent");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", apiKey);
  int code = http.POST(body, pos);
  free(body);

  if (code <= 0) { http.end(); return "Network error " + String(code); }
  String resp = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return "Bad reply (HTTP " + String(code) + ")";

  String text;
  JsonArray parts = doc["candidates"][0]["content"]["parts"].as<JsonArray>();
  for (JsonObject part : parts) text += part["text"] | "";
  text.trim();
  if (text.length()) return text;

  String err = doc["error"]["message"] | "";
  if (err.length()) return "API " + String(code) + ": " + err;
  String why = doc["candidates"][0]["finishReason"] | "empty";
  return "No text (" + why + ")";
}

// ---------- first-time setup portal ----------
String htmlEsc(String v) {
  v.replace("&", "&amp;"); v.replace("<", "&lt;"); v.replace("\"", "&quot;"); v.replace("'", "&#39;");
  return v;
}

void portalSave() {
  if (!portalActive) { server.send(404, "text/plain", "not found"); return; }
  String s = server.arg("ssid"), p = server.arg("pass"), k = server.arg("key");
  if (s.length()) prefs.putString("ssid", s);
  if (p.length()) prefs.putString("pass", p);
  if (k.length()) prefs.putString("key", k);
  server.send(200, "text/html", "<meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:sans-serif;padding:16px'>Saved. Restarting...");
  delay(600);
  ESP.restart();
}

void portalPage() {
  if (!portalActive) { server.send(404, "text/plain", "not found"); return; }
  String h = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
             "<body style='font-family:sans-serif;max-width:480px;margin:auto;padding:14px'>"
             "<h2>SUD CAM setup</h2><form method=post action=/save>"
             "Wi-Fi name (phone hotspot or home)<br><input name=ssid style='width:100%;padding:8px' value='" + htmlEsc(cfgSsid) + "'><br><br>"
             "Wi-Fi password<br><input name=pass style='width:100%;padding:8px'><br><br>"
             "Gemini API key (aistudio.google.com/apikey)<br><input name=key style='width:100%;padding:8px'><br><br>"
             "<button style='padding:10px 20px'>Save</button></form>"
             "<p>Leave a field empty to keep the saved value.<br>Status: " + String(apiKey.length() ? "key set" : "no key yet") + "</p>";
  server.send(200, "text/html", h);
}

// Own access point until the CAM is online (and has a key); the form saves and restarts
void runPortal() {
  portalActive = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("SUD-CAM", "cyddisplay");
  if (cfgSsid.length()) WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  server.on("/", HTTP_GET, portalPage);
  server.on("/save", HTTP_POST, portalSave);
  server.begin();
  // leave the portal only if we are online AND a key is stored
  while (!(WiFi.status() == WL_CONNECTED && apiKey.length())) {
    server.handleClient();
    delay(5);
  }
  portalActive = false;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

void setup() {
  Serial.begin(115200);
  if (!initCamera()) { Serial.println("Camera init failed"); delay(2000); ESP.restart(); }

  prefs.begin("cam", false);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  apiKey = prefs.getString("key", "");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (cfgSsid.length()) {
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(300);
  }
  if (WiFi.status() != WL_CONNECTED || !apiKey.length()) runPortal();
  Serial.println(WiFi.localIP());

  if (esp_now_init() != ESP_OK) { delay(2000); ESP.restart(); }
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  server.on("/cam.jpg", handleJpg);
  server.begin();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(1000); return; }
  server.handleClient();

  // tell the CYD where to fetch the live view (broadcast every 2 s)
  static uint32_t lastIp = 0;
  if (millis() - lastIp > 2000) {
    lastIp = millis();
    String ip = WiFi.localIP().toString();
    sendPkt(PKT_IP, 0, 1, ip.c_str(), ip.length());
  }

  if (shotRequested) {
    shotRequested = false;
    sendStatus("Taking photo...");
    String ans = askGemini();
    Serial.println(ans);
    sendAnswer(ans);
  }
  delay(1);
}
