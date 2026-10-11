// ESP32-2432S028 (CYD) v3
// Calculator -> secret combo -> PIN -> MENU:  PHOTO (live view + AI answer)
//                                              NOTES (cheat sheets from SD, uploaded via web)
//                                              ANSWERS (saved on SD)
//                                              WEB (network info, AP session)
// Panic: red X on every screen, physical button IO27<->GND, auto-return after idle
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>
#include <TFT_eSPI.h>
#include <vector>
#include <algorithm>

// ================== SETTINGS ==================
// Wi-Fi name/password, PIN, secret combo and web password are NOT stored in the code
// (so the firmware can be published). They live on the device and are changed
// on the web page (Settings). These are only the defaults:
#define DEF_COMBO       "1337"           // type it in the calculator and press "="
#define DEF_PIN         "2580"           // PIN on the lock screen
#define DEF_WEBPASS     "cyddisplay"     // web login password AND password of the setup access point (8+ chars)
#define PANIC_PIN       27               // button between IO27 and GND (CN1 connector)
#define IDLE_TIMEOUT_MS 90000UL          // auto-return to calculator

#define WEB_HOST        "sud"            // -> http://sud.local
#define WEB_USER        "admin"
#define AP_SSID         "SUD"            // own access point (only when started from the WEB screen)
// ==============================================

// Packet shared with the CAM board (keep identical on both sides)
struct Pkt { uint8_t type; uint8_t idx; uint8_t total; uint8_t len; char text[230]; };
enum { PKT_SHOT = 1, PKT_STATUS = 2, PKT_ANSWER = 3, PKT_IP = 4 };
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

TFT_eSPI tft;
Preferences prefs;

// Samsung-like palette (RGB565)
#define C_BG    0x0000
#define C_NUM   0x2945  // dark grey digits / list rows
#define C_FUNC  0xA514  // light grey (C, DEL, %, BACK)
#define C_OP    0xFC60  // orange operators
#define C_PRESS 0x6B4D  // pressed key

enum Screen { SCR_CALC, SCR_PASS, SCR_MENU, SCR_PHOTO, SCR_LIST, SCR_VIEW, SCR_WEB };
Screen screen = SCR_CALC;
uint32_t lastActivity = 0;
bool sdOk = false, apSession = false;
String cfgSsid, cfgWifiPass, cfgPin = DEF_PIN, cfgCombo = DEF_COMBO, cfgWebPass = DEF_WEBPASS;

// ---------- data from ESP-NOW ----------
char answer[1400];
char statusTxt[64] = "Ready";
volatile bool statusDirty = false, answerReady = false;
volatile uint8_t rxMask = 0;
volatile uint32_t lastRx = 0;
bool waiting = false;
uint32_t shotAt = 0;

// ---------- live preview (frames are fetched from the CAM over Wi-Fi) ----------
char camIp[16] = "";
volatile uint32_t lastIpRx = 0;
uint8_t frameBuf[24000];
volatile size_t frameLen = 0;
volatile bool frameReady = false, liveWanted = false;

// ---------- file list / viewer state ----------
std::vector<String> items;
String listPath = "/notes";
int page = 0;
String viewPath;
char *viewTxt = nullptr;
size_t viewLen = 0;
std::vector<size_t> pageStart;
int vpage = 0;
size_t nextStart = 0;

void freeView() {
  if (viewTxt) { free(viewTxt); viewTxt = nullptr; }
  viewLen = 0; pageStart.clear(); vpage = 0; nextStart = 0;
}

void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < 4) return;
  Pkt p{};
  memcpy(&p, data, min(len, (int)sizeof(p)));
  if (p.len > sizeof(p.text)) p.len = sizeof(p.text);
  lastRx = millis();
  if (p.type == PKT_IP) {
    size_t n = min((size_t)p.len, sizeof(camIp) - 1);
    memcpy(camIp, p.text, n);
    camIp[n] = 0;
    lastIpRx = millis();
  } else if (p.type == PKT_STATUS) {
    size_t n = min((size_t)p.len, sizeof(statusTxt) - 1);
    memcpy(statusTxt, p.text, n);
    statusTxt[n] = 0;
    statusDirty = true;
  } else if (p.type == PKT_ANSWER && p.total > 0 && p.total <= 6 && p.idx < p.total) {
    if (p.idx == 0) { memset(answer, 0, sizeof(answer)); rxMask = 0; }
    size_t off = (size_t)p.idx * sizeof(p.text);
    if (off + p.len < sizeof(answer)) memcpy(answer + off, p.text, p.len);
    rxMask |= (1 << p.idx);
    if (rxMask == (uint8_t)((1 << p.total) - 1)) answerReady = true;
  }
}

void sendShot() {
  Pkt p{};
  p.type = PKT_SHOT;
  esp_now_send(BCAST, (uint8_t *)&p, 4);
}

// ================== TOUCH (XPT2046 bit-banged, so the SD card can own the VSPI bus) ==================
#define T_CLK  25
#define T_MISO 39
#define T_MOSI 32
#define T_CS   33
#define T_IRQ  36

bool swapXY = false;
int cx0 = 300, cx1 = 3700, cy0 = 300, cy1 = 3700;  // raw values at screen x=20/300 and y=20/220

uint16_t xptRead(uint8_t cmd) {
  digitalWrite(T_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(T_MOSI, (cmd >> i) & 1);
    digitalWrite(T_CLK, HIGH);
    digitalWrite(T_CLK, LOW);
  }
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(T_CLK, HIGH);
    v = (v << 1) | (digitalRead(T_MISO) ? 1 : 0);
    digitalWrite(T_CLK, LOW);
  }
  digitalWrite(T_CS, HIGH);
  return (v >> 3) & 0x0FFF;
}

bool readRaw(int &rx, int &ry) {
  if (digitalRead(T_IRQ) != LOW) return false;
  xptRead(0x90);  // dummy conversion
  long sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < 5; i++) {  // average a few samples = no jitter
    int x = xptRead(0xD0), y = xptRead(0x90);
    if (x > 60 && x < 4030 && y > 60 && y < 4030) { sx += x; sy += y; n++; }
  }
  if (n < 3 || digitalRead(T_IRQ) != LOW) return false;
  rx = sx / n; ry = sy / n;
  return true;
}

// fires once per press (must release before the next one)
bool getTouch(int &x, int &y) {
  static bool down = false;
  int rx, ry;
  if (!readRaw(rx, ry)) { down = false; return false; }
  if (down) return false;
  down = true;
  int a = swapXY ? ry : rx, b = swapXY ? rx : ry;
  x = constrain(map(a, cx0, cx1, 20, 300), 0, 319);
  y = constrain(map(b, cy0, cy1, 20, 220), 0, 239);
  return true;
}

void waitRelease() {
  while (digitalRead(T_IRQ) == LOW) delay(10);
  delay(200);
}

void drawCross(int x, int y) {
  tft.drawLine(x - 12, y, x + 12, y, TFT_WHITE);
  tft.drawLine(x, y - 12, x, y + 12, TFT_WHITE);
  tft.drawCircle(x, y, 8, TFT_ORANGE);
}

// 3 taps: top-left, top-right, bottom-left. Detects swapped/inverted axes by itself.
void calibrate() {
  const int tx[3] = {20, 300, 20}, ty[3] = {20, 20, 220};
  int rx[3], ry[3];
  for (int i = 0; i < 3; i++) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Tap the cross", 160, 120, 2);
    drawCross(tx[i], ty[i]);
    while (!readRaw(rx[i], ry[i])) delay(10);
    waitRelease();
  }
  swapXY = abs(ry[1] - ry[0]) > abs(rx[1] - rx[0]);
  if (!swapXY) { cx0 = rx[0]; cx1 = rx[1]; cy0 = ry[0]; cy1 = ry[2]; }
  else         { cx0 = ry[0]; cx1 = ry[1]; cy0 = rx[0]; cy1 = rx[2]; }
  prefs.putBool("sw", swapXY);
  prefs.putInt("x0", cx0); prefs.putInt("x1", cx1);
  prefs.putInt("y0", cy0); prefs.putInt("y1", cy1);
  prefs.putBool("c3", true);
}

// ================== COMMON UI ==================
// Title bar (0..25) with the red PANIC X on the right
void drawTitle(const char *t) {
  tft.fillRect(0, 0, 258, 26, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(t, 8, 13, 2);
  tft.fillRect(258, 0, 62, 26, TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("X", 289, 13, 4);
}

void drawWrapped(const char *s, int x, int y, int w, int h) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  int lh = tft.fontHeight(2), cx = x, cy = y;
  String word;
  for (const char *p = s;; p++) {
    char c = *p;
    if (c == ' ' || c == '\n' || c == 0) {
      if (word.length()) {
        int ww = tft.textWidth(word, 2);
        if (cx + ww > x + w) { cx = x; cy += lh; }
        if (cy + lh > y + h) return;
        tft.drawString(word, cx, cy, 2);
        cx += ww + tft.textWidth(" ", 2);
        word = "";
      }
      if (c == '\n') { cx = x; cy += lh; }
      if (c == 0) break;
    } else if ((uint8_t)c >= 32 && (uint8_t)c < 127) word += c;  // ASCII only
  }
}

// ================== SD ==================
void sdInit() {
  SPI.begin(18, 19, 23, 5);  // the SD slot of the CYD
  sdOk = SD.begin(5, SPI, 20000000);
  if (sdOk) { SD.mkdir("/notes"); SD.mkdir("/answers"); }
}

String baseName(String n) {
  int sl = n.lastIndexOf('/');
  return sl >= 0 ? n.substring(sl + 1) : n;
}

std::vector<String> listDir(const char *dir, bool newestFirst) {
  std::vector<String> v;
  if (!sdOk) return v;
  File root = SD.open(dir);
  if (!root) return v;
  File f;
  while ((f = root.openNextFile())) {
    if (!f.isDirectory()) v.push_back(baseName(f.name()));
    f.close();
  }
  root.close();
  std::sort(v.begin(), v.end(), [](const String &a, const String &b) { return a < b; });
  if (newestFirst) std::reverse(v.begin(), v.end());
  return v;
}

String firstChars(const String &path, int n) {
  String s;
  File f = SD.open(path);
  if (!f) return s;
  while (f.available() && (int)s.length() < n) {
    char c = f.read();
    if (c == '\n' || c == '\r') c = ' ';
    if ((uint8_t)c >= 32 && (uint8_t)c < 127) s += c;
  }
  f.close();
  return s;
}

void saveAnswer() {
  if (!sdOk || !answer[0]) return;
  uint32_t n = prefs.getUInt("cnt", 0) + 1;
  prefs.putUInt("cnt", n);
  char path[32];
  snprintf(path, sizeof(path), "/answers/%05u.txt", (unsigned)n);
  File f = SD.open(path, FILE_WRITE);
  if (f) { f.print(answer); f.close(); }
}

// ================== KEYPAD ==================
#define KEY_Y0 50
#define KEY_W  80
#define ROW_H  38

typedef const char *const KeyRow[4];
KeyRow CALC_KEYS[5] = {
  {"C", "DEL", "%", "/"},
  {"7", "8", "9", "*"},
  {"4", "5", "6", "-"},
  {"1", "2", "3", "+"},
  {"00", "0", ".", "="}};
KeyRow PASS_KEYS[4] = {
  {"1", "2", "3", "DEL"},
  {"4", "5", "6", "CLR"},
  {"7", "8", "9", "EXIT"},
  {"", "0", "", "OK"}};

uint16_t keyBg(const char *k) {
  if (!k[0]) return C_BG;
  if (!k[1] && strchr("/*-+=", k[0])) return C_OP;
  if (!strcmp(k, "OK")) return C_OP;
  if (!strcmp(k, "C") || !strcmp(k, "DEL") || !strcmp(k, "%") || !strcmp(k, "CLR") || !strcmp(k, "EXIT")) return C_FUNC;
  return C_NUM;
}

void drawKey(KeyRow *keys, int r, int c, bool pressed) {
  const char *k = keys[r][c];
  if (!k[0]) return;
  int x = c * KEY_W, y = KEY_Y0 + r * ROW_H;
  uint16_t bg = pressed ? C_PRESS : keyBg(k);
  tft.fillRoundRect(x + 3, y + 3, KEY_W - 6, ROW_H - 6, 16, bg);
  uint16_t fg = (bg == C_FUNC) ? TFT_BLACK : TFT_WHITE;
  const char *lbl = k;
  if (!strcmp(k, "*")) lbl = "x";
  else if (!strcmp(k, "DEL")) lbl = "<";
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(lbl, x + KEY_W / 2, y + ROW_H / 2 + 1, 4);
}

void drawKeys(KeyRow *keys, int rows) {
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < 4; c++) drawKey(keys, r, c, false);
}

bool hitKey(int rows, int x, int y, int &r, int &c) {
  if (y < KEY_Y0) return false;
  r = (y - KEY_Y0) / ROW_H;
  c = x / KEY_W;
  return r < rows && c < 4;
}

void drawHeader(const String &mainTxt, const String &sub) {
  tft.fillRect(0, 0, 320, KEY_Y0, C_BG);
  tft.setTextDatum(MR_DATUM);
  if (sub.length()) { tft.setTextColor(0x8410, C_BG); tft.drawString(sub, 312, 10, 2); }
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.drawString(mainTxt, 312, 32, 4);
}

// ================== CALCULATOR ==================
String expr, lastExpr;
bool fresh = false;  // true right after "=" (a digit starts a new expression)
#define MAXLEN 18

bool isOp(char c) { return c == '+' || c == '-' || c == '*' || c == '/'; }

const char *ep;
double parseNumber() {
  bool neg = false;
  if (*ep == '-') { neg = true; ep++; }
  double v = 0;
  while (*ep >= '0' && *ep <= '9') v = v * 10 + (*ep++ - '0');
  if (*ep == '.') {
    ep++;
    double f = 0.1;
    while (*ep >= '0' && *ep <= '9') { v += (*ep++ - '0') * f; f *= 0.1; }
  }
  while (*ep == '%') { v /= 100.0; ep++; }
  return neg ? -v : v;
}
double parseTerm() {
  double v = parseNumber();
  while (*ep == '*' || *ep == '/') {
    char o = *ep++;
    double r = parseNumber();
    if (o == '*') v *= r;
    else v = (r != 0) ? v / r : NAN;
  }
  return v;
}
double parseExpr() {
  double v = parseTerm();
  while (*ep == '+' || *ep == '-') {
    char o = *ep++;
    double r = parseTerm();
    v = (o == '+') ? v + r : v - r;
  }
  return v;
}
String evalStr(const String &s) {
  ep = s.c_str();
  double v = parseExpr();
  if (isnan(v) || isinf(v)) return "Error";
  char b[24];
  snprintf(b, sizeof(b), "%.8g", v);
  return String(b);
}

String curNumber() {
  int i = (int)expr.length() - 1;
  while (i >= 0 && !isOp(expr[i])) i--;
  return expr.substring(i + 1);
}

void drawCalc() {
  tft.fillScreen(C_BG);
  drawHeader(expr, lastExpr);
  drawKeys(CALC_KEYS, 5);
}

String pass;
int wrongTries = 0;

void goCalc() {  // reset everything, back to a plain calculator
  memset(answer, 0, sizeof(answer));
  strcpy(statusTxt, "Ready");
  pass = ""; expr = ""; lastExpr = "";
  fresh = false; waiting = false; liveWanted = false;
  rxMask = 0; answerReady = false; statusDirty = false;
  freeView(); items.clear();
  screen = SCR_CALC;
}

void calcPress(const char *k) {
  if (!strcmp(k, "C")) { expr = ""; lastExpr = ""; fresh = false; return; }
  if (!strcmp(k, "DEL")) {
    if (fresh) { expr = ""; fresh = false; }
    else if (expr.length()) expr.remove(expr.length() - 1);
    return;
  }
  if (!strcmp(k, "=")) {
    if (expr == cfgCombo) { expr = ""; pass = ""; wrongTries = 0; screen = SCR_PASS; return; }
    if (!expr.length()) return;
    String r = evalStr(expr);
    if (r == "Error") { lastExpr = "Error"; expr = ""; fresh = false; }
    else { lastExpr = expr + " ="; expr = r; fresh = true; }
    return;
  }
  size_t n = expr.length();
  char last = n ? expr[n - 1] : 0;
  if (!k[1] && isOp(k[0])) {  // + - * /
    fresh = false;
    if (!n) { if (k[0] == '-') expr = "-"; }
    else if (isOp(last)) { if (n == 1) expr = (k[0] == '-') ? "-" : ""; else expr.setCharAt(n - 1, k[0]); }
    else if (n < MAXLEN) expr += k;
    return;
  }
  if (!strcmp(k, "%")) { if (n && isdigit(last) && n < MAXLEN) expr += '%'; return; }
  // digits, "00", "."
  if (fresh) { expr = ""; lastExpr = ""; fresh = false; n = 0; last = 0; }
  if (last == '%' || n >= MAXLEN - 1) return;
  String cn = curNumber();
  if (!strcmp(k, ".")) {
    if (cn.indexOf('.') >= 0) return;
    expr += cn.length() ? "." : "0.";
  } else if (!strcmp(k, "00")) {
    if (!cn.length() || cn == "0") return;
    expr += "00";
  } else {
    if (cn == "0") expr.setCharAt(n - 1, k[0]);
    else expr += k;
  }
}

// ================== PIN SCREEN ==================
String stars() { String s; for (size_t i = 0; i < pass.length(); i++) s += '*'; return s; }

void drawPass() {
  tft.fillScreen(C_BG);
  drawHeader(stars(), "PIN");
  drawKeys(PASS_KEYS, 4);
}

void passPress(const char *k) {
  if (!strcmp(k, "DEL")) { if (pass.length()) pass.remove(pass.length() - 1); }
  else if (!strcmp(k, "CLR")) pass = "";
  else if (!strcmp(k, "EXIT")) { goCalc(); }
  else if (!strcmp(k, "OK")) {
    if (pass == cfgPin) { pass = ""; wrongTries = 0; screen = SCR_MENU; return; }
    pass = "";
    if (++wrongTries >= 3) { wrongTries = 0; goCalc(); }
  } else if (pass.length() < 12) pass += k;
}


// ================== MENU ==================
struct Tile { int x, y, w, h; const char *label; uint16_t col; };
const Tile TILES[4] = {
  {4, 32, 154, 96, "PHOTO", 0x03E0},
  {162, 32, 154, 96, "NOTES", 0x2D7F},
  {4, 134, 154, 96, "ANSWERS", 0x7A9F},
  {162, 134, 154, 96, "WEB", C_OP}};

void drawMenu() {
  tft.fillScreen(C_BG);
  drawTitle("MENU");
  tft.setTextDatum(MC_DATUM);
  for (int i = 0; i < 4; i++) {
    const Tile &t = TILES[i];
    tft.fillRoundRect(t.x, t.y, t.w, t.h, 14, t.col);
    tft.setTextColor(TFT_WHITE, t.col);
    tft.drawString(t.label, t.x + t.w / 2, t.y + t.h / 2, 4);
  }
}

void loadList(const char *dir) {
  listPath = dir;
  items = listDir(dir, !strcmp(dir, "/answers"));  // newest answers first
  page = 0;
}

void menuTouch(int x, int y) {
  for (int i = 0; i < 4; i++) {
    const Tile &t = TILES[i];
    if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) {
      if (i == 0) screen = SCR_PHOTO;
      else if (i == 1) { loadList("/notes"); screen = SCR_LIST; }
      else if (i == 2) { loadList("/answers"); screen = SCR_LIST; }
      else screen = SCR_WEB;
      return;
    }
  }
}

// ================== FILE LIST ==================
#define PER_PAGE 5

String answerLabel(const String &name) {
  return "#" + String(atoi(name.c_str())) + "  " + firstChars("/answers/" + name, 28);
}

void drawList() {
  tft.fillScreen(C_BG);
  drawTitle(listPath == "/answers" ? "ANSWERS" : "NOTES");
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, C_BG);
  if (!sdOk) tft.drawString("No SD card", 10, 110, 4);
  else if (items.empty()) tft.drawString("Empty", 10, 110, 4);
  else {
    for (int i = 0; i < PER_PAGE; i++) {
      int idx = page * PER_PAGE + i;
      if (idx >= (int)items.size()) break;
      int y = 30 + i * 34;
      tft.fillRoundRect(2, y, 316, 32, 8, C_NUM);
      tft.setTextColor(TFT_WHITE, C_NUM);
      String label = (listPath == "/answers") ? answerLabel(items[idx]) : items[idx];
      if (label.length() > 36) label = label.substring(0, 36);
      tft.drawString(label, 10, y + 16, 2);
    }
  }
  tft.setTextDatum(MC_DATUM);
  tft.fillRoundRect(2, 202, 100, 36, 10, C_FUNC);
  tft.setTextColor(TFT_BLACK, C_FUNC);
  tft.drawString("BACK", 52, 220, 2);
  tft.fillRoundRect(108, 202, 100, 36, 10, C_NUM);
  tft.setTextColor(TFT_WHITE, C_NUM);
  tft.drawString("<", 158, 220, 4);
  tft.fillRoundRect(214, 202, 104, 36, 10, C_NUM);
  tft.drawString(">", 266, 220, 4);
}

// ================== FILE VIEWER (JPEG or text) ==================
bool isJpg(const String &p) {
  String l = p;
  l.toLowerCase();
  return l.endsWith(".jpg") || l.endsWith(".jpeg");
}

void drawPanicCorner() {
  tft.fillRect(280, 0, 40, 26, TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("X", 300, 13, 4);
}

void showMessage(const char *m) {
  tft.fillScreen(C_BG);
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(m, 160, 120, 2);
  drawPanicCorner();
}

void drawImageFile() {
  File f = SD.open(viewPath);
  if (!f) { showMessage("Cannot open file"); return; }
  size_t n = f.size();
  if (n == 0 || n > 120000) { f.close(); showMessage("File too large"); return; }
  uint8_t *buf = (uint8_t *)malloc(n);
  if (!buf) { f.close(); showMessage("Out of memory"); return; }
  f.read(buf, n);
  f.close();
  uint16_t w = 0, h = 0;
  if (TJpgDec.getJpgSize(&w, &h, buf, n) == JDR_OK) {
    int sc = 1;
    while ((w / sc > 320 || h / sc > 240) && sc < 8) sc *= 2;
    tft.fillScreen(TFT_BLACK);
    TJpgDec.setJpgScale(sc);
    TJpgDec.drawJpg((320 - w / sc) / 2, (240 - h / sc) / 2, buf, n);
    TJpgDec.setJpgScale(1);
    drawPanicCorner();
  } else {
    showMessage("Not a baseline JPEG");
  }
  free(buf);
}

bool loadText() {
  File f = SD.open(viewPath);
  if (!f) return false;
  size_t n = min((size_t)f.size(), (size_t)8000);
  viewTxt = (char *)malloc(n + 1);
  if (!viewTxt) { f.close(); return false; }
  viewLen = f.read((uint8_t *)viewTxt, n);
  f.close();
  viewTxt[viewLen] = 0;
  pageStart.clear();
  pageStart.push_back(0);
  vpage = 0;
  return true;
}

// Lays out one page of text from `start`; returns where the next page starts (viewLen = end)
size_t layoutText(size_t start, bool draw) {
  const int X0 = 6, Y0 = 6, RIGHT = 314, BOTTOM = 238;
  int lh = tft.fontHeight(2), cx = X0, cy = Y0;
  String word;
  size_t wordStart = start;
  for (size_t i = start;; i++) {
    char c = i < viewLen ? viewTxt[i] : 0;
    bool brk = (c == ' ' || c == '\n' || c == '\r' || c == 0);
    if (!brk) {
      if ((uint8_t)c >= 32 && (uint8_t)c < 127) {
        if (!word.length()) wordStart = i;
        word += c;
      }
      continue;
    }
    if (word.length()) {
      int ww = tft.textWidth(word, 2);
      if (cx + ww > RIGHT) { cx = X0; cy += lh; }
      if (cy + lh > BOTTOM) return wordStart;
      if (draw) tft.drawString(word, cx, cy, 2);
      cx += ww + tft.textWidth(" ", 2);
      word = "";
    }
    if (c == '\n') { cx = X0; cy += lh; }
    if (c == 0) return viewLen;
  }
}

void drawTextPage() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  nextStart = layoutText(pageStart[vpage], true);
  drawPanicCorner();
}

void drawView() {
  if (isJpg(viewPath)) { drawImageFile(); return; }
  if (!viewTxt && !loadText()) { showMessage("Cannot open file"); return; }
  drawTextPage();
}

void openView(const String &path) {
  freeView();
  viewPath = path;
  screen = SCR_VIEW;
}

void viewTouch(int x, int y) {
  if (isJpg(viewPath)) { freeView(); screen = SCR_LIST; return; }  // tap = back
  if (x >= 107) {  // right part: next page
    if (nextStart >= viewLen) { freeView(); screen = SCR_LIST; }
    else {
      if ((int)pageStart.size() <= vpage + 1) pageStart.push_back(nextStart);
      vpage++;
      drawTextPage();
    }
  } else {  // left part: previous page
    if (vpage == 0) { freeView(); screen = SCR_LIST; }
    else { vpage--; drawTextPage(); }
  }
}

void listTouch(int x, int y) {
  if (y >= 202) {
    if (x < 104) screen = SCR_MENU;
    else if (x < 212) { if (page > 0) { page--; drawList(); } }
    else { if ((page + 1) * PER_PAGE < (int)items.size()) { page++; drawList(); } }
    return;
  }
  if (y < 30 || !sdOk) return;
  int i = (y - 30) / 34, idx = page * PER_PAGE + i;
  if (i >= PER_PAGE || idx >= (int)items.size()) return;
  openView(listPath + "/" + items[idx]);
}

// ================== WEB INFO SCREEN ==================
void drawWebScreen() {
  tft.fillScreen(C_BG);
  drawTitle("WEB");
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.setTextDatum(TL_DATUM);
  int y = 34;
  String s = (WiFi.status() == WL_CONNECTED) ? "Net: " + WiFi.SSID() + "  " + WiFi.localIP().toString()
                                              : String(cfgSsid.length() ? "Net: not connected" : "Net: not set up (use Settings)");
  tft.drawString(s, 6, y, 2); y += 22;
  s = apSession ? String("AP: ON   ") + AP_SSID + " / " + cfgWebPass : String("AP: OFF");
  tft.drawString(s, 6, y, 2); y += 22;
  tft.drawString(String("Open: http://") + WEB_HOST + ".local", 6, y, 2); y += 22;
  if (apSession) { tft.drawString("or: http://" + WiFi.softAPIP().toString(), 6, y, 2); y += 22; }
  tft.drawString(String("Login: ") + WEB_USER + " / " + cfgWebPass, 6, y, 2); y += 22;
  tft.drawString(sdOk ? "SD card: OK" : "SD card: NOT FOUND", 6, y, 2);

  tft.setTextDatum(MC_DATUM);
  tft.fillRoundRect(2, 196, 200, 40, 10, apSession ? TFT_MAROON : C_OP);
  tft.setTextColor(TFT_WHITE, apSession ? TFT_MAROON : C_OP);
  tft.drawString(apSession ? "AP OFF (restart)" : "AP ON (restart)", 102, 216, 2);
  tft.fillRoundRect(208, 196, 110, 40, 10, C_FUNC);
  tft.setTextColor(TFT_BLACK, C_FUNC);
  tft.drawString("BACK", 263, 216, 2);
}

void webScreenTouch(int x, int y) {
  if (y < 196) return;
  if (x < 205) {  // the AP lives for one session: flag it and restart
    prefs.putBool("ap", !apSession);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Restarting...", 160, 120, 4);
    delay(400);
    ESP.restart();
  } else {
    screen = SCR_MENU;
  }
}

// ================== PHOTO SCREEN ==================
// LIVE view: picture 240x176 on the left, big camera-style shutter on the right
// ANSWER view: title bar + text + MENU / LIVE / SHOT
enum View { V_LIVE, V_ANSWER };
View view = V_LIVE;
int liveKnown = -1;

bool camAlive() { return camIp[0] && millis() - lastIpRx < 15000; }

// Background task: downloads JPEG frames from the CAM (no drawing here)
void netTask(void *) {
  WiFiClient client;
  HTTPClient http;
  for (;;) {
    if (!liveWanted || frameReady || !camAlive() || WiFi.status() != WL_CONNECTED) {
      vTaskDelay(15 / portTICK_PERIOD_MS);
      continue;
    }
    char ip[16];
    strncpy(ip, camIp, 15);
    ip[15] = 0;
    String url = String("http://") + ip + "/cam.jpg";
    http.setTimeout(2000);
    if (http.begin(client, url)) {
      int code = http.GET();
      if (code == 200) {
        int len = http.getSize();
        if (len > 0 && len <= (int)sizeof(frameBuf)) {
          WiFiClient *st = http.getStreamPtr();
          int got = 0;
          uint32_t t0 = millis();
          while (got < len && millis() - t0 < 2000) {
            int a = st->available();
            if (a) got += st->readBytes(frameBuf + got, min(a, len - got));
            else vTaskDelay(1);
          }
          if (got == len) { frameLen = len; frameReady = true; }
        }
      } else {
        vTaskDelay(300 / portTICK_PERIOD_MS);
      }
      http.end();
    }
    vTaskDelay(1);
  }
}

bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

void drawStatus() {
  if (view == V_LIVE) {
    tft.fillRect(0, 176, 240, 24, TFT_BLACK);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(statusTxt, 6, 188, 2);
  } else {
    drawTitle(statusTxt);
  }
}

void drawShutter(bool pressed) {
  tft.fillRect(240, 40, 80, 200, TFT_BLACK);
  tft.drawCircle(280, 140, 38, TFT_WHITE);
  tft.drawCircle(280, 140, 37, TFT_WHITE);
  tft.fillCircle(280, 140, 32, pressed ? 0x8410 : TFT_WHITE);
}

void enterLive() {
  view = V_LIVE;
  liveKnown = -1;
  frameReady = false;
  liveWanted = true;
  TJpgDec.setJpgScale(1);
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(240, 0, 80, 40, TFT_RED);  // PANIC
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("X", 280, 20, 4);
  drawShutter(false);
  tft.fillRoundRect(2, 202, 110, 36, 10, C_NUM);
  tft.setTextColor(TFT_WHITE, C_NUM);
  tft.drawString("MENU", 57, 220, 4);
  drawStatus();
}

void drawAnswer() {
  tft.fillRect(0, 26, 320, 172, TFT_BLACK);
  drawWrapped(answer, 6, 30, 308, 166);
}

void enterAnswer() {
  view = V_ANSWER;
  liveWanted = false;
  tft.fillScreen(TFT_BLACK);
  drawStatus();
  drawAnswer();
  tft.setTextDatum(MC_DATUM);
  tft.fillRoundRect(2, 200, 100, 38, 10, C_NUM);
  tft.setTextColor(TFT_WHITE, C_NUM);
  tft.drawString("MENU", 52, 219, 2);
  tft.fillRoundRect(108, 200, 100, 38, 10, C_NUM);
  tft.drawString("LIVE", 158, 219, 2);
  tft.fillRoundRect(214, 200, 104, 38, 10, TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("SHOT", 266, 219, 2);
}

void startShot() {
  strcpy(statusTxt, "Sending...");
  answer[0] = 0;
  waiting = true;
  shotAt = millis();
  frameReady = false;
  enterAnswer();
  sendShot();
}

void photoTouch(int x, int y) {
  if (view == V_LIVE) {
    if (x >= 240 && y >= 40) { drawShutter(true); delay(80); startShot(); return; }  // whole right column
    if (x < 114 && y >= 200) screen = SCR_MENU;
  } else if (y >= 200) {
    if (x < 106) screen = SCR_MENU;
    else if (x < 212) enterLive();
    else startShot();
  }
}

// ================== WEB SERVER (separate file) ==================
#include "web.h"

// ================== TOUCH DISPATCH ==================
bool inPanic(int x, int y) {
  if (screen == SCR_PHOTO && view == V_LIVE) return x >= 240 && y < 40;
  if (screen == SCR_VIEW) return x >= 280 && y < 26;
  return x >= 258 && y < 26;
}

void handleTouch(int x, int y) {
  int r, c;
  if (screen != SCR_CALC && screen != SCR_PASS && inPanic(x, y)) { goCalc(); return; }
  switch (screen) {
    case SCR_CALC:
      if (!hitKey(5, x, y, r, c) || !CALC_KEYS[r][c][0]) return;
      drawKey(CALC_KEYS, r, c, true);
      delay(60);
      calcPress(CALC_KEYS[r][c]);
      if (screen == SCR_CALC) { drawKey(CALC_KEYS, r, c, false); drawHeader(expr, lastExpr); }
      break;
    case SCR_PASS:
      if (!hitKey(4, x, y, r, c) || !PASS_KEYS[r][c][0]) return;
      drawKey(PASS_KEYS, r, c, true);
      delay(60);
      passPress(PASS_KEYS[r][c]);
      if (screen == SCR_PASS) { drawKey(PASS_KEYS, r, c, false); drawHeader(stars(), "PIN"); }
      break;
    case SCR_MENU:  menuTouch(x, y); break;
    case SCR_PHOTO: photoTouch(x, y); break;
    case SCR_LIST:  listTouch(x, y); break;
    case SCR_VIEW:  viewTouch(x, y); break;
    case SCR_WEB:   webScreenTouch(x, y); break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PANIC_PIN, INPUT_PULLUP);
  pinMode(T_CS, OUTPUT);
  digitalWrite(T_CS, HIGH);
  pinMode(T_CLK, OUTPUT);
  pinMode(T_MOSI, OUTPUT);
  pinMode(T_MISO, INPUT);
  pinMode(T_IRQ, INPUT);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  sdInit();

  prefs.begin("touch", false);
  cfgSsid = prefs.getString("ssid", "");
  cfgWifiPass = prefs.getString("wpass", "");
  cfgPin = prefs.getString("pin", DEF_PIN);
  cfgCombo = prefs.getString("combo", DEF_COMBO);
  cfgWebPass = prefs.getString("webpw", DEF_WEBPASS);
  apSession = prefs.getBool("ap", false);
  if (apSession) prefs.putBool("ap", false);  // the access point lives for this session only
  bool have = prefs.getBool("c3", false);
  bool force = (digitalRead(T_IRQ) == LOW);   // touch the screen while powering on = recalibrate
  if (have) {
    swapXY = prefs.getBool("sw", false);
    cx0 = prefs.getInt("x0", cx0); cx1 = prefs.getInt("x1", cx1);
    cy0 = prefs.getInt("y0", cy0); cy1 = prefs.getInt("y1", cy1);
  }
  if (!have || force) calibrate();

  // Station: same hotspot/network as the CAM => same channel => ESP-NOW works
  WiFi.mode(apSession ? WIFI_AP_STA : WIFI_STA);
  if (apSession) WiFi.softAP(AP_SSID, cfgWebPass.c_str());
  WiFi.setSleep(false);
  if (cfgSsid.length()) WiFi.begin(cfgSsid.c_str(), cfgWifiPass.c_str());
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onRecv);
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
  webBegin();

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);
  xTaskCreatePinnedToCore(netTask, "net", 8192, NULL, 1, NULL, 0);
  lastActivity = millis();
  drawCalc();
}

void loop() {
  static Screen drawn = SCR_CALC;
  int x, y;
  webLoop();
  if (getTouch(x, y)) { lastActivity = millis(); handleTouch(x, y); }

  // physical panic button
  if (screen != SCR_CALC && digitalRead(PANIC_PIN) == LOW) {
    delay(30);
    if (digitalRead(PANIC_PIN) == LOW) goCalc();
  }
  // idle timeout
  if (screen != SCR_CALC && millis() - lastActivity > IDLE_TIMEOUT_MS) goCalc();

  if (screen != drawn) {
    if (drawn == SCR_PHOTO) liveWanted = false;
    drawn = screen;
    switch (screen) {
      case SCR_CALC:  drawCalc(); break;
      case SCR_PASS:  drawPass(); break;
      case SCR_MENU:  drawMenu(); break;
      case SCR_PHOTO: strcpy(statusTxt, "Ready"); answer[0] = 0; enterLive(); break;
      case SCR_LIST:  drawList(); break;
      case SCR_VIEW:  drawView(); break;
      case SCR_WEB:   drawWebScreen(); break;
    }
  }

  // an answer from the CAM: always save it to the SD card, show it if we are on the photo screen
  if (answerReady) {
    answerReady = false;
    waiting = false;
    saveAnswer();
    if (screen == SCR_PHOTO) {
      lastActivity = millis();
      strcpy(statusTxt, "Answer:");
      enterAnswer();
    }
  }

  if (screen == SCR_PHOTO) {
    if (statusDirty) { statusDirty = false; if (view == V_ANSWER) drawStatus(); }
    if (view == V_LIVE) {
      int k = camAlive();
      if (k != liveKnown) {
        liveKnown = k;
        strcpy(statusTxt, k ? "Live" : "Waiting for CAM...");
        drawStatus();
      }
      if (frameReady) {
        TJpgDec.setJpgScale(1);
        TJpgDec.drawJpg(0, 0, frameBuf, frameLen);
        frameReady = false;
      }
    }
    if (waiting && millis() - max((uint32_t)shotAt, (uint32_t)lastRx) > 45000) {
      waiting = false;
      strcpy(statusTxt, "No reply from CAM");
      drawStatus();
    }
  }
}
