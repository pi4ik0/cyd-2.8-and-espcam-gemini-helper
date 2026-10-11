// web.h - web interface of the CYD (included from main.cpp)
//   http://sud.local  (or http://192.168.4.1 when the access point is on)
//   login: WEB_USER / WEB_PASS
#pragma once
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

extern bool sdOk, apSession;

WebServer server(80);
DNSServer dns;
File upFile;
bool upOk = false, mdnsOn = false;

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SUD</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:640px;margin:0 auto;padding:12px}
h1{color:#e67e22}h2{margin:22px 0 8px;font-size:1.1em}
.row{display:flex;gap:8px;align-items:center;padding:6px 0;border-bottom:1px solid #333}
.row a{color:#7cf;flex:1;word-break:break-all}
button{font:inherit;background:#e67e22;color:#fff;border:0;border-radius:8px;padding:8px 14px}
input[type=text],textarea{font:inherit;width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #444;border-radius:8px;padding:8px}
small{color:#999}label{display:block;margin:8px 0;color:#bbb}
</style></head><body>
<h1>SUD</h1>

<h2>Upload files (images, .txt)</h2>
<input type="file" id="f" multiple accept="image/*,.txt,.md,text/plain"><br><br>
<button onclick="up()">Upload</button> <small id="st"></small>
<small><br>Images are converted to a baseline JPEG that fits the 320x240 screen.</small>

<h2>New text note</h2>
<input type="text" id="nn" placeholder="name (e.g. physics)"><br><br>
<textarea id="nt" rows="6" placeholder="text"></textarea>
<label><input type="checkbox" id="tr" checked> transliterate Russian/Ukrainian to Latin (the screen has no Cyrillic font)</label>
<button onclick="note()">Save note</button>

<h2>Settings</h2>
<small>Leave a field empty to keep the current value. The device restarts after saving.</small><br><br>
<input type="text" id="s_ssid" placeholder="Wi-Fi name (phone hotspot or home)"><br><br>
<input type="text" id="s_wp" placeholder="Wi-Fi password"><br><br>
<input type="text" id="s_pin" placeholder="PIN (3-12 digits)"><br><br>
<input type="text" id="s_combo" placeholder="Secret combo (3-10 digits)"><br><br>
<input type="text" id="s_web" placeholder="Web / AP password (8-32 characters)"><br><br>
<button onclick="saveCfg()">Save settings</button> <small id="cs"></small>

<h2>Notes</h2><div id="notes"></div>
<h2>Answers</h2><div id="ans"></div>

<script>
const M={а:'a',б:'b',в:'v',г:'g',ґ:'g',д:'d',е:'e',є:'ye',ё:'yo',ж:'zh',з:'z',и:'i',і:'i',ї:'yi',й:'y',к:'k',л:'l',м:'m',н:'n',о:'o',п:'p',р:'r',с:'s',т:'t',у:'u',ф:'f',х:'kh',ц:'ts',ч:'ch',ш:'sh',щ:'shch',ъ:'',ы:'y',ь:'',э:'e',ю:'yu',я:'ya'};
function tl(s){return s.replace(/[А-Яа-яЁёІіЇїЄєҐґ]/g,c=>{const l=c.toLowerCase();const r=M[l]??'';return c===l?r:r.charAt(0).toUpperCase()+r.slice(1)})}
const $=id=>document.getElementById(id);
function img2jpg(file){return new Promise((ok,no)=>{const u=URL.createObjectURL(file);const im=new Image();
im.onload=()=>{let w=im.width,h=im.height;const s=Math.min(1,320/w,240/h);w=Math.round(w*s);h=Math.round(h*s);
const c=document.createElement('canvas');c.width=w;c.height=h;const x=c.getContext('2d');x.fillStyle='#fff';x.fillRect(0,0,w,h);x.drawImage(im,0,0,w,h);
c.toBlob(b=>{URL.revokeObjectURL(u);ok(b)},'image/jpeg',0.92)};im.onerror=no;im.src=u})}
async function up(){const fs=[...$('f').files];for(const f of fs){$('st').textContent='uploading '+f.name+' ...';let b=f,n=f.name;
if(f.type.startsWith('image/')){b=await img2jpg(f);n=n.replace(/\.[^.]*$/,'')+'.jpg'}
const d=new FormData();d.append('file',b,n);await fetch('/upload',{method:'POST',body:d})}
$('st').textContent='done';$('f').value='';list()}
async function note(){let n=$('nn').value||'note',t=$('nt').value;if($('tr').checked){t=tl(t);n=tl(n)}
await fetch('/note',{method:'POST',body:new URLSearchParams({name:n,text:t})});$('nt').value='';list()}
function render(id,a){const e=$(id);e.innerHTML='';if(!a.length){e.innerHTML='<small>empty</small>';return}
a.forEach(x=>{const r=document.createElement('div');r.className='row';const l=document.createElement('a');
l.href='/f?p='+encodeURIComponent(x.p);l.target='_blank';l.textContent=x.n+'  ('+x.s+' B)';
const b=document.createElement('button');b.textContent='delete';
b.onclick=async()=>{if(confirm('Delete '+x.n+'?')){await fetch('/del?p='+encodeURIComponent(x.p),{method:'POST'});list()}};
r.append(l,b);e.append(r)})}
async function list(){try{const j=await(await fetch('/api/list')).json();render('notes',j.notes);render('ans',j.answers)}catch(e){}}
async function saveCfg(){const r=await fetch('/cfg',{method:'POST',body:new URLSearchParams({ssid:$('s_ssid').value,wpass:$('s_wp').value,pin:$('s_pin').value,combo:$('s_combo').value,webpw:$('s_web').value})});$('cs').textContent=await r.text()}
fetch('/api/cfg').then(r=>r.json()).then(j=>{$('s_ssid').placeholder='Wi-Fi name (now: '+(j.ssid||'not set')+')';$('s_combo').placeholder='Secret combo (now: '+j.combo+')'}).catch(()=>{});
list();
</script></body></html>)HTML";

#define NEED_AUTH  if (!server.authenticate(WEB_USER, cfgWebPass.c_str())) { server.requestAuthentication(); return; }

bool okPath(const String &p) {
  return (p.startsWith("/notes/") || p.startsWith("/answers/")) && p.indexOf("..") < 0;
}

String safeName(String n) {
  int sl = max(n.lastIndexOf('/'), n.lastIndexOf('\\'));
  if (sl >= 0) n = n.substring(sl + 1);
  String o;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    o += (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') ? c : '_';
  }
  int d = o.lastIndexOf('.');
  String ext = d >= 0 ? o.substring(d) : "";
  String base = d >= 0 ? o.substring(0, d) : o;
  if (base.length() > 30) base = base.substring(0, 30);
  if (!base.length()) base = "file";
  return base + ext;
}

void listJson(const char *dir, String &out) {
  out += "[";
  bool first = true;
  File root = SD.open(dir);
  if (root) {
    File f;
    while ((f = root.openNextFile())) {
      if (!f.isDirectory()) {
        String n = baseName(f.name());
        if (!first) out += ',';
        first = false;
        out += "{\"n\":\"" + n + "\",\"p\":\"" + String(dir) + "/" + n + "\",\"s\":" + String((unsigned)f.size()) + "}";
      }
      f.close();
    }
    root.close();
  }
  out += "]";
}

void handleRoot() {
  NEED_AUTH
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleList() {
  NEED_AUTH
  String out = "{\"notes\":";
  if (sdOk) listJson("/notes", out); else out += "[]";
  out += ",\"answers\":";
  if (sdOk) listJson("/answers", out); else out += "[]";
  out += "}";
  server.send(200, "application/json", out);
}

void handleFile() {
  NEED_AUTH
  String p = server.arg("p");
  if (!sdOk || !okPath(p) || !SD.exists(p)) { server.send(404, "text/plain", "not found"); return; }
  File f = SD.open(p);
  String low = p;
  low.toLowerCase();
  const char *ct = (low.endsWith(".jpg") || low.endsWith(".jpeg")) ? "image/jpeg" : "text/plain; charset=utf-8";
  server.streamFile(f, ct);
  f.close();
}

void handleDel() {
  NEED_AUTH
  String p = server.arg("p");
  if (sdOk && okPath(p) && SD.exists(p)) SD.remove(p);
  server.send(200, "text/plain", "ok");
}

void handleNote() {
  NEED_AUTH
  if (!sdOk) { server.send(500, "text/plain", "no SD"); return; }
  String name = safeName(server.arg("name"));
  if (!name.endsWith(".txt")) name += ".txt";
  String text = server.arg("text");
  if (text.length() > 8000) text = text.substring(0, 8000);
  String p = "/notes/" + name;
  SD.remove(p);
  File f = SD.open(p, FILE_WRITE);
  if (f) { f.print(text); f.close(); }
  server.send(200, "text/plain", "ok");
}

String jsonEsc(String v) {
  v.replace("\\", "\\\\");
  v.replace("\"", "\\\"");
  return v;
}

bool digitsOnly(const String &v) {
  if (!v.length()) return false;
  for (size_t i = 0; i < v.length(); i++) if (!isdigit((unsigned char)v[i])) return false;
  return true;
}

void handleCfgGet() {
  NEED_AUTH
  server.send(200, "application/json", "{\"ssid\":\"" + jsonEsc(cfgSsid) + "\",\"combo\":\"" + jsonEsc(cfgCombo) + "\"}");
}

void handleCfgPost() {
  NEED_AUTH
  String ssid = server.arg("ssid"), wp = server.arg("wpass"), pin = server.arg("pin");
  String combo = server.arg("combo"), webpw = server.arg("webpw");
  String err;
  if (pin.length() && !(digitsOnly(pin) && pin.length() >= 3 && pin.length() <= 12)) err = "PIN: 3-12 digits";
  else if (combo.length() && !(digitsOnly(combo) && combo.length() >= 3 && combo.length() <= 10)) err = "Combo: 3-10 digits";
  else if (webpw.length() && (webpw.length() < 8 || webpw.length() > 32)) err = "Web password: 8-32 characters";
  if (err.length()) { server.send(400, "text/plain", err); return; }
  if (ssid.length()) prefs.putString("ssid", ssid);
  if (wp.length()) prefs.putString("wpass", wp);
  if (pin.length()) prefs.putString("pin", pin);
  if (combo.length()) prefs.putString("combo", combo);
  if (webpw.length()) prefs.putString("webpw", webpw);
  server.send(200, "text/plain", "Saved. Restarting...");
  delay(600);
  ESP.restart();
}

void handleUploadDone() {
  NEED_AUTH
  server.send(200, "text/plain", "ok");
}

void handleUploadData() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upOk = sdOk && server.authenticate(WEB_USER, cfgWebPass.c_str());
    if (!upOk) return;
    String p = "/notes/" + safeName(u.filename);
    SD.remove(p);
    upFile = SD.open(p, FILE_WRITE);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upOk && upFile) upFile.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
    upOk = false;
  }
}

void webBegin() {
  const char *keys[] = {"Authorization"};
  server.collectHeaders(keys, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/list", HTTP_GET, handleList);
  server.on("/f", HTTP_GET, handleFile);
  server.on("/del", HTTP_POST, handleDel);
  server.on("/note", HTTP_POST, handleNote);
  server.on("/api/cfg", HTTP_GET, handleCfgGet);
  server.on("/cfg", HTTP_POST, handleCfgPost);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  // in AP mode every host name resolves to the CYD, so sud.local works on phones too
  if (apSession) dns.start(53, "*", WiFi.softAPIP());
}

void webLoop() {
  server.handleClient();
  if (apSession) dns.processNextRequest();
  if (!mdnsOn && (apSession || WiFi.status() == WL_CONNECTED)) {
    if (MDNS.begin(WEB_HOST)) { MDNS.addService("http", "tcp", 80); mdnsOn = true; }
  }
}
