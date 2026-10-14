#include "esp_camera.h"
#include <WiFi.h>
#include "FS.h"
#include "SD_MMC.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Wi-Fi Access Point Configuration
const char *ssid = "ESP32-CAM";
const char *password = "12345678"; // کم از کم 8 ہندسوں کا پاس ورڈ

// Camera Pin Configuration for AI-Thinker
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define FLASH_GPIO_NUM    4

// NEW: photos are stored in this folder on the SD card.
// A sub-folder is used because FAT16 cards limit the root folder to about 512 entries.
#define PHOTO_DIR         "/photos"
// NEW: how many of the newest photos the web page lists
#define GALLERY_MAX       48

// NEW: one entry of the gallery list
struct PhotoInfo {
  char name[24];
  uint32_t size;
};

// NEW: the web server and the web page are now built into this sketch (bottom of the file).
// app_httpd.cpp and camera_index.h from the CameraWebServer example are NOT needed.
// If they are still in the sketch folder, delete them or the build fails (duplicate startCameraServer).
void startCameraServer();

// NEW: shared state
bool sdReady = false;
SemaphoreHandle_t sdMutex = NULL;   // guards photo numbering and deletes
uint32_t photoNext = 0;             // number of the next photo file
uint32_t photoCount = 0;            // photos currently on the card
volatile bool flashOn = false;      // flash LED switched on from the web page
volatile bool flashOnSnap = false;  // fire the flash during Snapshot
volatile uint32_t autoSaveSec = 0;  // timelapse interval in seconds, 0 = off
uint32_t lastAutoSave = 0;

// NEW: file name without any folder part (File::name() differs between core versions)
const char *photoBaseName(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

// NEW: accepts only names like photo_000012.jpg and returns the number
bool parsePhotoName(const char *n, uint32_t *idx) {
  if(strncmp(n, "photo_", 6) != 0) return false;
  const char *p = n + 6;
  uint32_t v = 0;
  int digits = 0;
  while(*p >= '0' && *p <= '9'){
    v = v * 10 + (uint32_t)(*p - '0');
    p++;
    if(++digits > 9) return false;
  }
  if(digits == 0 || strcmp(p, ".jpg") != 0) return false;
  if(idx) *idx = v;
  return true;
}

bool validPhotoName(const char *n) {
  return strlen(n) < 24 && parsePhotoName(n, NULL);
}

// NEW: keeps the GALLERY_MAX highest-numbered photos in arr, sorted oldest to newest
void insertTop(PhotoInfo *arr, int *cnt, const char *name, uint32_t size) {
  int pos;
  if(*cnt < GALLERY_MAX){
    pos = (*cnt)++;
  } else {
    if(strcmp(name, arr[0].name) <= 0) return;   // older than everything already kept
    memmove(&arr[0], &arr[1], sizeof(PhotoInfo) * (GALLERY_MAX - 1));
    pos = GALLERY_MAX - 1;
  }
  strlcpy(arr[pos].name, name, sizeof(arr[pos].name));
  arr[pos].size = size;
  while(pos > 0 && strcmp(arr[pos - 1].name, arr[pos].name) > 0){
    PhotoInfo tmp = arr[pos - 1];
    arr[pos - 1] = arr[pos];
    arr[pos] = tmp;
    pos--;
  }
}

// NEW: creates the photo folder and continues numbering after the highest existing photo
void initPhotoStore() {
  if(!SD_MMC.exists(PHOTO_DIR)) SD_MMC.mkdir(PHOTO_DIR);
  File dir = SD_MMC.open(PHOTO_DIR);
  if(!dir || !dir.isDirectory()){
    Serial.println("Cannot open " PHOTO_DIR " on the SD card");
    sdReady = false;
    return;
  }
  File f = dir.openNextFile();
  while(f){
    uint32_t idx;
    if(!f.isDirectory() && parsePhotoName(photoBaseName(f.name()), &idx)){
      photoCount++;
      if(idx >= photoNext) photoNext = idx + 1;
    }
    f = dir.openNextFile();
  }
  dir.close();
  Serial.printf("SD card: %lu photos, next is photo_%06lu.jpg, %lu MB free\n",
                (unsigned long)photoCount, (unsigned long)photoNext,
                (unsigned long)((SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024ULL * 1024ULL)));
}

// NEW: saves one JPEG frame as /photos/photo_000000.jpg, photo_000001.jpg, ...
// Used by the Snapshot button and by the timelapse. Returns true on success and copies the
// file name (without folder) into outName when outName is not NULL.
bool saveFrameToSD(camera_fb_t *fb, char *outName, size_t outLen) {
  if(!sdReady || !fb || fb->format != PIXFORMAT_JPEG || !sdMutex) return false;
  if(xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) return false;

  bool ok = false;
  char name[24];
  char path[48];
  // find an unused number (normally the first try)
  for(int tries = 0; tries < 50; tries++){
    snprintf(name, sizeof(name), "photo_%06lu.jpg", (unsigned long)photoNext);
    snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
    if(!SD_MMC.exists(path)) break;
    photoNext++;
  }

  if(SD_MMC.exists(path)){
    Serial.println("Could not find a free photo number");
  } else {
    File file = SD_MMC.open(path, FILE_WRITE);
    if(!file){
      Serial.printf("Failed to open %s for writing\n", path);
    } else {
      size_t written = file.write(fb->buf, fb->len);
      file.close();
      if(written == fb->len){
        ok = true;
        photoNext++;
        photoCount++;
        if(outName) strlcpy(outName, name, outLen);
        Serial.printf("Saved %s (%u bytes)\n", path, (unsigned)fb->len);
      } else {
        Serial.printf("Short write to %s (card full?), removing it\n", path);
        SD_MMC.remove(path);
      }
    }
  }

  xSemaphoreGive(sdMutex);
  return ok;
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  sdMutex = xSemaphoreCreateMutex();

  // Initialize Micro SD Card
  // FIX: 1-bit mode. The default 4-bit mode claims GPIO4 (the flash LED) and
  // GPIO12/13 as data lines, so the LED lights up on every SD access.
  if(!SD_MMC.begin("/sdcard", true)){
    Serial.println("SD Card Mount Failed");
  } else {
    uint8_t cardType = SD_MMC.cardType();
    if(cardType == CARD_NONE){
      Serial.println("No SD Card attached");
    } else {
      sdReady = true; // NEW: photos are only saved when this is true
      Serial.println("SD Card Initialized Successfully");
      initPhotoStore();
    }
  }

  // Flash LED Pin Setup
  // FIX: moved after SD init so the SD driver can't leave GPIO4 in another state
  pinMode(FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_GPIO_NUM, LOW);

  // Camera Configuration
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_VGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if(config.pixel_format == PIXFORMAT_JPEG){
    if(psramFound()){
      // FIX: CAMERA_GRAB_LATEST only does anything with 2+ frame buffers
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // FIX: without PSRAM the frame buffer must live in DRAM, otherwise init fails
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  }

  // Init Camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    // FIX: don't fall through to an idle loop with no AP and no indication; retry via reboot
    delay(3000);
    ESP.restart();
  }

  // Start Wi-Fi Access Point
  WiFi.mode(WIFI_AP);
  if(!WiFi.softAP(ssid, password)){
    Serial.println("Failed to start Wi-Fi AP");
    delay(3000);
    ESP.restart();
  }
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);

  // Start Web Server
  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(myIP);
  Serial.println("' to connect");
}

void loop() {
  // پنگ یا دیگر بیک گراؤنڈ ٹاسک یہاں ہو سکتے ہیں

  // NEW: timelapse. Saves a frame every autoSaveSec seconds (set on the web page).
  // Runs on the ESP32 itself, so it keeps going when the browser is closed.
  uint32_t secs = autoSaveSec;
  if(secs > 0 && sdReady && millis() - lastAutoSave >= secs * 1000UL){
    lastAutoSave = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    if(fb){
      saveFrameToSD(fb, NULL, 0);
      esp_camera_fb_return(fb);
    }
  }
  delay(100);
}

// =====================================================================
// NEW: web server and web page (replaces app_httpd.cpp / camera_index.h)
//
//   /                 the web page
//   /stream  (port 81) live MJPEG stream
//   /snap             takes a photo, saves it to the SD card, returns the JPEG
//   /status           JSON with SD space, photo count, clients and current settings
//   /control?var=&val= change a setting
//   /list             JSON list of the newest photos on the SD card
//   /photo?name=      view (or &dl=1 download) a photo from the SD card
//   /delete?name=     delete one photo (or ?all=1 for every photo)
// =====================================================================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--line:#2a2f3a;--tx:#e8eaf0;--mut:#8b93a5;--acc:#4f9dff;--bad:#ff6b6b}
*{box-sizing:border-box}
[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;align-items:center;justify-content:space-between;gap:10px 16px;flex-wrap:wrap;padding:12px 16px;border-bottom:1px solid var(--line)}
h1{font-size:18px;margin:0}
.chips{display:flex;gap:8px;flex-wrap:wrap}
.chip{border:1px solid var(--line);border-radius:999px;padding:3px 10px;font-size:12px;color:var(--mut);background:var(--card)}
.chip b{color:var(--tx);font-weight:600}
main{max-width:1100px;margin:0 auto;padding:16px;display:grid;gap:16px;grid-template-columns:minmax(0,1fr)}
@media(min-width:900px){main{grid-template-columns:minmax(0,2fr) minmax(0,1fr)}.wide{grid-column:1/-1}}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px}
h2{font-size:13px;margin:0 0 10px;color:var(--mut);font-weight:600;letter-spacing:.05em;text-transform:uppercase}
h2 span{text-transform:none;letter-spacing:0;font-weight:400}
#view{position:relative;background:#000;border-radius:8px;overflow:hidden;aspect-ratio:4/3;display:flex;align-items:center;justify-content:center}
#view img{width:100%;height:100%;object-fit:contain}
#stream:not([src]){display:none}
#ph{position:absolute;color:var(--mut);font-size:14px}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px;align-items:center}
button,.btn{background:#232834;color:var(--tx);border:1px solid var(--line);border-radius:8px;padding:8px 14px;font:inherit;cursor:pointer;text-decoration:none}
button:hover,.btn:hover{border-color:var(--acc)}
button:disabled{opacity:.5;cursor:default}
.p{background:var(--acc);border-color:var(--acc);color:#04101f;font-weight:600}
.d{color:var(--bad)}
.last{display:flex;align-items:center;gap:10px;margin-top:10px;color:var(--mut);font-size:13px}
.last img{height:56px;border-radius:6px;border:1px solid var(--line);display:block}
.set{display:grid;grid-template-columns:104px minmax(0,1fr) 34px;gap:10px;align-items:center;font-size:14px}
.set label{color:var(--mut)}
.set select{grid-column:2/4;width:100%;background:#232834;color:var(--tx);border:1px solid var(--line);border-radius:8px;padding:7px 8px;font:inherit}
.set input[type=range]{width:100%;accent-color:var(--acc)}
.set input[type=checkbox]{grid-column:2/4;justify-self:start;width:20px;height:20px;accent-color:var(--acc)}
.hint{color:var(--mut);font-size:12px;margin:12px 0 0}
.file{display:flex;align-items:center;gap:12px;padding:9px 2px;border-top:1px solid var(--line);font-size:14px}
.file>a{flex:1;word-break:break-all}
.file>span{color:var(--mut);font-size:13px;white-space:nowrap}
.act{display:flex;gap:14px}
a{color:var(--acc);text-decoration:none}
a.bad{color:var(--bad)}
.empty{color:var(--mut);padding:14px 2px;font-size:14px}
#modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.82);align-items:center;justify-content:center;padding:16px;z-index:10}
#modal.open{display:flex}
#modal .box{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;max-width:min(96vw,900px);max-height:96vh;overflow:auto}
#modal img{display:block;max-width:100%;max-height:68vh;margin:0 auto;border-radius:8px}
#mname{flex:1;color:var(--mut);font-size:13px;word-break:break-all}
#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:#232834;border:1px solid var(--line);padding:9px 16px;border-radius:8px;font-size:14px;opacity:0;pointer-events:none;transition:opacity .2s;z-index:20}
#toast.show{opacity:1}
#toast.bad{border-color:var(--bad);color:var(--bad)}
</style>
</head>
<body>
<header>
  <h1>ESP32-CAM</h1>
  <div class="chips">
    <span class="chip" id="c_sd">SD <b>...</b></span>
    <span class="chip" id="c_ph">Photos <b>...</b></span>
    <span class="chip" id="c_cl">Clients <b>...</b></span>
    <span class="chip" id="c_up">Uptime <b>...</b></span>
  </div>
</header>
<main>
  <section class="card">
    <h2>Live view</h2>
    <div id="view"><span id="ph">Press Start stream</span><img id="stream" alt="Live camera stream"></div>
    <div class="row">
      <button class="p" id="bs" onclick="toggleStream()">Start stream</button>
      <button id="bsnap" onclick="snap()">Snapshot to SD</button>
      <button onclick="goFull()">Fullscreen</button>
    </div>
    <div class="last" id="last" hidden>
      <a id="lastlink" download="snapshot.jpg"><img id="lastimg" alt="Last snapshot"></a>
      <span id="lastname"></span>
    </div>
  </section>

  <section class="card">
    <h2>Settings</h2>
    <div class="set">
      <label for="framesize">Resolution</label>
      <select id="framesize">
        <option value="5">QVGA 320x240</option>
        <option value="6">CIF 400x296</option>
        <option value="8">VGA 640x480</option>
        <option value="9">SVGA 800x600</option>
        <option value="10">XGA 1024x768</option>
        <option value="11">HD 1280x720</option>
        <option value="12">SXGA 1280x1024</option>
        <option value="13">UXGA 1600x1200</option>
      </select>
      <label for="quality">JPEG quality</label><input type="range" id="quality" min="10" max="63"><span id="quality_v"></span>
      <label for="brightness">Brightness</label><input type="range" id="brightness" min="-2" max="2"><span id="brightness_v"></span>
      <label for="contrast">Contrast</label><input type="range" id="contrast" min="-2" max="2"><span id="contrast_v"></span>
      <label for="saturation">Saturation</label><input type="range" id="saturation" min="-2" max="2"><span id="saturation_v"></span>
      <label for="hmirror">Mirror</label><input type="checkbox" id="hmirror">
      <label for="vflip">Flip</label><input type="checkbox" id="vflip">
      <label for="flash">Flash LED</label><input type="checkbox" id="flash">
      <label for="flashcap">Flash on snap</label><input type="checkbox" id="flashcap">
      <label for="autosave">Auto-save</label>
      <select id="autosave">
        <option value="0">Off</option>
        <option value="5">Every 5 seconds</option>
        <option value="10">Every 10 seconds</option>
        <option value="30">Every 30 seconds</option>
        <option value="60">Every minute</option>
        <option value="300">Every 5 minutes</option>
        <option value="600">Every 10 minutes</option>
      </select>
    </div>
    <p class="hint">JPEG quality: a lower number means a sharper picture and a bigger file. Auto-save keeps running when this page is closed.</p>
  </section>

  <section class="card wide">
    <h2>SD card photos <span id="count"></span></h2>
    <div class="row" style="margin-top:0">
      <button onclick="loadList()">Refresh</button>
      <button class="d" onclick="delAll()">Delete all</button>
    </div>
    <div id="files"></div>
  </section>
</main>

<div id="modal" onclick="if(event.target===this)closeModal()">
  <div class="box">
    <img id="mimg" alt="Photo">
    <div class="row">
      <span id="mname"></span>
      <a class="btn" id="mdl" href="#">Download</a>
      <button class="d" onclick="del(cur)">Delete</button>
      <button onclick="closeModal()">Close</button>
    </div>
  </div>
</div>
<div id="toast"></div>

<script>
const $=id=>document.getElementById(id);
const BLANK='data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==';
let streaming=false,cur='',lastUrl='';

function toast(msg,bad){
  const t=$('toast');
  t.textContent=msg;
  t.className=bad?'show bad':'show';
  clearTimeout(toast.id);
  toast.id=setTimeout(()=>{t.className=''},2800);
}
async function api(url){
  const r=await fetch(url,{cache:'no-store'});
  if(!r.ok)throw new Error(r.status);
  return r;
}
async function ctl(k,v){
  try{await api('/control?var='+k+'&val='+v)}
  catch(e){toast('Could not change '+k,true)}
}

function startStream(){
  streaming=true;
  $('stream').src='http://'+location.hostname+':81/stream?'+Date.now();
  $('ph').style.display='none';
  $('bs').textContent='Stop stream';
}
function stopStream(){
  streaming=false;
  $('stream').src=BLANK;
  $('ph').style.display='';
  $('bs').textContent='Start stream';
}
function toggleStream(){streaming?stopStream():startStream()}
$('stream').onerror=()=>{if(streaming){stopStream();toast('Stream lost',true)}};
function goFull(){
  const v=$('view');
  if(v.requestFullscreen)v.requestFullscreen();
  else if(v.webkitRequestFullscreen)v.webkitRequestFullscreen();
}

async function snap(){
  const b=$('bsnap');
  b.disabled=true;
  try{
    const r=await api('/snap');
    const name=r.headers.get('X-Saved-As');
    const blob=await r.blob();
    if(lastUrl)URL.revokeObjectURL(lastUrl);
    lastUrl=URL.createObjectURL(blob);
    $('lastimg').src=lastUrl;
    $('lastlink').href=lastUrl;
    $('lastlink').download=name||'snapshot.jpg';
    $('lastname').textContent=name?('Saved as '+name):'Not saved (no SD card)';
    $('last').hidden=false;
    toast(name?('Saved '+name+' to the SD card'):'Captured, but there is no SD card to save to',!name);
    loadList();
    refresh(false);
  }catch(e){toast('Snapshot failed',true)}
  b.disabled=false;
}

function fmtUp(s){
  const h=Math.floor(s/3600),m=Math.floor(s%3600/60);
  return h?h+'h '+m+'m':m+'m '+(s%60)+'s';
}
async function refresh(first){
  try{
    const s=await (await api('/status')).json();
    $('c_sd').innerHTML=s.sd?'SD <b>'+s.sdFree+' MB free</b>':'SD <b style="color:var(--bad)">not found</b>';
    $('c_ph').innerHTML='Photos <b>'+s.photos+'</b>';
    $('c_cl').innerHTML='Clients <b>'+s.clients+'</b>';
    $('c_up').innerHTML='Uptime <b>'+fmtUp(s.uptime)+'</b>';
    if(first){
      ['framesize','quality','brightness','contrast','saturation','autosave'].forEach(k=>{
        $(k).value=s[k];
        const v=$(k+'_v');
        if(v)v.textContent=s[k];
      });
      ['hmirror','vflip','flash','flashcap'].forEach(k=>{$(k).checked=!!s[k]});
      $('autosave').disabled=!s.sd;
      if(!s.psram){
        Array.from($('framesize').options).forEach(o=>{if(+o.value>9)o.disabled=true});
      }
    }
  }catch(e){
    $('c_sd').innerHTML='<b style="color:var(--bad)">Offline</b>';
  }
}

async function loadList(){
  const box=$('files');
  try{
    const j=await (await api('/list')).json();
    if(!j.sd){
      $('count').textContent='';
      box.innerHTML='<div class="empty">No SD card found. Insert a FAT32 card and restart the board.</div>';
      return;
    }
    $('count').textContent=j.total?(j.total>j.files.length?'- newest '+j.files.length+' of '+j.total:'- '+j.total):'';
    if(!j.files.length){
      box.innerHTML='<div class="empty">No photos yet. Press Snapshot to SD to save one.</div>';
      return;
    }
    box.innerHTML=j.files.map(f=>`<div class="file"><a href="#" onclick="view('${f.n}');return false">${f.n}</a><span>${Math.max(1,Math.round(f.s/1024))} KB</span><span class="act"><a href="/photo?name=${f.n}&dl=1">Download</a><a href="#" class="bad" onclick="del('${f.n}');return false">Delete</a></span></div>`).join('');
  }catch(e){
    box.innerHTML='<div class="empty">Could not read the photo list.</div>';
  }
}
function view(n){
  cur=n;
  $('mimg').src='/photo?name='+n;
  $('mname').textContent=n;
  $('mdl').href='/photo?name='+n+'&dl=1';
  $('modal').className='open';
}
function closeModal(){
  $('modal').className='';
  $('mimg').removeAttribute('src');
}
async function del(n){
  if(!confirm('Delete '+n+'?'))return;
  try{
    await api('/delete?name='+n);
    toast('Deleted '+n);
    closeModal();
    loadList();
    refresh(false);
  }catch(e){toast('Delete failed',true)}
}
async function delAll(){
  if(!confirm('Delete ALL photos on the SD card? This cannot be undone.'))return;
  try{
    const j=await (await api('/delete?all=1')).json();
    toast('Deleted '+j.deleted+' photos');
    closeModal();
    loadList();
    refresh(false);
  }catch(e){toast('Delete failed',true)}
}

['quality','brightness','contrast','saturation'].forEach(k=>{
  const e=$(k);
  e.addEventListener('input',()=>{$(k+'_v').textContent=e.value});
  e.addEventListener('change',()=>ctl(k,e.value));
});
['hmirror','vflip','flash','flashcap'].forEach(k=>{
  $(k).addEventListener('change',e=>ctl(k,e.target.checked?1:0));
});
['framesize','autosave'].forEach(k=>{
  $(k).addEventListener('change',e=>ctl(k,e.target.value));
});
document.addEventListener('keydown',e=>{if(e.key==='Escape')closeModal()});

refresh(true);
loadList();
setInterval(()=>refresh(false),5000);
</script>
</body>
</html>
)rawliteral";

// reads one value from the URL query string, e.g. getParam(req, "var", buf, sizeof(buf))
static bool getParam(httpd_req_t *req, const char *key, char *out, size_t outLen) {
  char query[128];
  if(httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
  return httpd_query_key_value(query, key, out, outLen) == ESP_OK;
}

static esp_err_t sendJson(httpd_req_t *req, const char *json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if(res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char part[64];
  while(true){
    camera_fb_t *fb = esp_camera_fb_get();
    if(!fb){
      Serial.println("Stream: camera capture failed");
      res = ESP_FAIL;
    } else {
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      if(res == ESP_OK){
        size_t hlen = snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
        res = httpd_resp_send_chunk(req, part, hlen);
      }
      if(res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      esp_camera_fb_return(fb);
    }
    if(res != ESP_OK) break;   // browser closed the stream or the camera failed
  }
  return res;
}

static esp_err_t snap_handler(httpd_req_t *req) {
  if(flashOnSnap){
    digitalWrite(FLASH_GPIO_NUM, HIGH);
    delay(120);
    for(int i = 0; i < 2; i++){            // drop frames taken before the light was on
      camera_fb_t *old = esp_camera_fb_get();
      if(old) esp_camera_fb_return(old);
    }
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if(flashOnSnap && !flashOn) digitalWrite(FLASH_GPIO_NUM, LOW);
  if(!fb){
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
    return ESP_FAIL;
  }

  char saved[24] = "";
  bool ok = saveFrameToSD(fb, saved, sizeof(saved));   // 'saved' must stay alive until the send below
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if(ok) httpd_resp_set_hdr(req, "X-Saved-As", saved);
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t status_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  unsigned long totalMB = 0, freeMB = 0;
  if(sdReady){
    uint64_t total = SD_MMC.totalBytes();
    uint64_t used = SD_MMC.usedBytes();
    totalMB = (unsigned long)(total / (1024ULL * 1024ULL));
    freeMB = (total > used) ? (unsigned long)((total - used) / (1024ULL * 1024ULL)) : 0;
  }
  char buf[420];
  snprintf(buf, sizeof(buf),
           "{\"sd\":%d,\"sdTotal\":%lu,\"sdFree\":%lu,\"photos\":%lu,\"uptime\":%lu,\"clients\":%d,\"psram\":%d,"
           "\"framesize\":%d,\"quality\":%d,\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,"
           "\"hmirror\":%d,\"vflip\":%d,\"flash\":%d,\"flashcap\":%d,\"autosave\":%lu}",
           sdReady ? 1 : 0, totalMB, freeMB, (unsigned long)photoCount, (unsigned long)(millis() / 1000),
           (int)WiFi.softAPgetStationNum(), psramFound() ? 1 : 0,
           (int)s->status.framesize, (int)s->status.quality, (int)s->status.brightness,
           (int)s->status.contrast, (int)s->status.saturation,
           (int)s->status.hmirror, (int)s->status.vflip,
           flashOn ? 1 : 0, flashOnSnap ? 1 : 0, (unsigned long)autoSaveSec);
  return sendJson(req, buf);
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char var[24], val[12];
  if(!getParam(req, "var", var, sizeof(var)) || !getParam(req, "val", val, sizeof(val))){
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing var or val");
    return ESP_FAIL;
  }
  int v = atoi(val);
  sensor_t *s = esp_camera_sensor_get();
  int res = 0;

  if(!strcmp(var, "framesize")){
    if(v < 0 || v > (int)FRAMESIZE_UXGA || (!psramFound() && v > (int)FRAMESIZE_SVGA)) res = -1;
    else res = s->set_framesize(s, (framesize_t)v);
  }
  else if(!strcmp(var, "quality"))    res = s->set_quality(s, constrain(v, 10, 63));
  else if(!strcmp(var, "brightness")) res = s->set_brightness(s, constrain(v, -2, 2));
  else if(!strcmp(var, "contrast"))   res = s->set_contrast(s, constrain(v, -2, 2));
  else if(!strcmp(var, "saturation")) res = s->set_saturation(s, constrain(v, -2, 2));
  else if(!strcmp(var, "hmirror"))    res = s->set_hmirror(s, v ? 1 : 0);
  else if(!strcmp(var, "vflip"))      res = s->set_vflip(s, v ? 1 : 0);
  else if(!strcmp(var, "flash")){
    flashOn = (v != 0);
    digitalWrite(FLASH_GPIO_NUM, flashOn ? HIGH : LOW);
  }
  else if(!strcmp(var, "flashcap"))   flashOnSnap = (v != 0);
  else if(!strcmp(var, "autosave")){
    if(v > 0 && !sdReady) res = -1;
    else {
      autoSaveSec = (uint32_t)constrain(v, 0, 3600);
      lastAutoSave = millis();
    }
  }
  else res = -1;

  if(res < 0){
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Setting not supported");
    return ESP_FAIL;
  }
  return sendJson(req, "{\"ok\":true}");
}

static esp_err_t list_handler(httpd_req_t *req) {
  if(!sdReady) return sendJson(req, "{\"sd\":false,\"total\":0,\"files\":[]}");

  PhotoInfo top[GALLERY_MAX];
  int cnt = 0;
  uint32_t total = 0;
  File dir = SD_MMC.open(PHOTO_DIR);
  if(dir && dir.isDirectory()){
    File f = dir.openNextFile();
    while(f){
      if(!f.isDirectory()){
        const char *nm = photoBaseName(f.name());
        if(validPhotoName(nm)){
          total++;
          insertTop(top, &cnt, nm, (uint32_t)f.size());
        }
      }
      f = dir.openNextFile();
    }
    dir.close();
  }
  photoCount = total;

  // newest first, sent in small chunks so no large buffer is needed
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char buf[96];
  int n = snprintf(buf, sizeof(buf), "{\"sd\":true,\"total\":%lu,\"files\":[", (unsigned long)total);
  esp_err_t res = httpd_resp_send_chunk(req, buf, n);
  for(int i = cnt - 1; i >= 0 && res == ESP_OK; i--){
    n = snprintf(buf, sizeof(buf), "%s{\"n\":\"%s\",\"s\":%lu}",
                 (i == cnt - 1) ? "" : ",", top[i].name, (unsigned long)top[i].size);
    res = httpd_resp_send_chunk(req, buf, n);
  }
  if(res == ESP_OK) res = httpd_resp_send_chunk(req, "]}", 2);
  if(res == ESP_OK) res = httpd_resp_send_chunk(req, NULL, 0);
  return res;
}

static esp_err_t photo_handler(httpd_req_t *req) {
  char name[32], dl[4] = "0";
  if(!sdReady || !getParam(req, "name", name, sizeof(name)) || !validPhotoName(name)){
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Photo not found");
    return ESP_FAIL;
  }
  getParam(req, "dl", dl, sizeof(dl));

  char path[48];
  snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
  File f = SD_MMC.open(path, FILE_READ);
  if(!f || f.isDirectory()){
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Photo not found");
    return ESP_FAIL;
  }
  uint8_t *buf = (uint8_t *)malloc(4096);
  if(!buf){
    f.close();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }

  char disp[64];   // must stay alive until the last send below
  snprintf(disp, sizeof(disp), "%s; filename=\"%s\"", dl[0] == '1' ? "attachment" : "inline", name);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", disp);

  esp_err_t res = ESP_OK;
  size_t n;
  while(res == ESP_OK && (n = f.read(buf, 4096)) > 0){
    res = httpd_resp_send_chunk(req, (const char *)buf, n);
  }
  free(buf);
  f.close();
  if(res == ESP_OK) res = httpd_resp_send_chunk(req, NULL, 0);
  return res;
}

static esp_err_t delete_handler(httpd_req_t *req) {
  if(!sdReady || !sdMutex){
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No SD card");
    return ESP_FAIL;
  }

  char all[4] = "0";
  getParam(req, "all", all, sizeof(all));

  if(all[0] == '1'){
    // delete every photo, 16 at a time (removing files while walking a folder can skip entries)
    uint32_t deleted = 0;
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    while(true){
      char batch[16][24];
      int bn = 0;
      File dir = SD_MMC.open(PHOTO_DIR);
      if(dir && dir.isDirectory()){
        File f = dir.openNextFile();
        while(f && bn < 16){
          if(!f.isDirectory()){
            const char *nm = photoBaseName(f.name());
            if(validPhotoName(nm)) strlcpy(batch[bn++], nm, sizeof(batch[0]));
          }
          f = dir.openNextFile();
        }
        dir.close();
      }
      if(bn == 0) break;
      int removed = 0;
      for(int i = 0; i < bn; i++){
        char path[48];
        snprintf(path, sizeof(path), PHOTO_DIR "/%s", batch[i]);
        if(SD_MMC.remove(path)){
          removed++;
          deleted++;
        }
      }
      if(removed == 0) break;   // nothing could be removed (write-protected card?), avoid looping forever
    }
    photoCount = (photoCount > deleted) ? photoCount - deleted : 0;
    xSemaphoreGive(sdMutex);
    char out[40];
    snprintf(out, sizeof(out), "{\"ok\":true,\"deleted\":%lu}", (unsigned long)deleted);
    return sendJson(req, out);
  }

  char name[32];
  if(!getParam(req, "name", name, sizeof(name)) || !validPhotoName(name)){
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad photo name");
    return ESP_FAIL;
  }
  char path[48];
  snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  bool ok = SD_MMC.remove(path);
  if(ok && photoCount > 0) photoCount--;
  xSemaphoreGive(sdMutex);
  if(!ok){
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Could not delete photo");
    return ESP_FAIL;
  }
  return sendJson(req, "{\"ok\":true}");
}

static void addUri(httpd_handle_t server, const char *uri, esp_err_t (*handler)(httpd_req_t *)) {
  httpd_uri_t u;
  memset(&u, 0, sizeof(u));
  u.uri = uri;
  u.method = HTTP_GET;
  u.handler = handler;
  u.user_ctx = NULL;
  httpd_register_uri_handler(server, &u);
}

void startCameraServer() {
  // Main server on port 80: the page and all the buttons
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 12;
  config.stack_size = 8192;          // SD card access needs more than the default 4096
  config.lru_purge_enable = true;    // drop the oldest idle connection instead of refusing a new one

  httpd_handle_t uiServer = NULL;
  if(httpd_start(&uiServer, &config) == ESP_OK){
    addUri(uiServer, "/",        index_handler);
    addUri(uiServer, "/status",  status_handler);
    addUri(uiServer, "/snap",    snap_handler);
    addUri(uiServer, "/control", cmd_handler);
    addUri(uiServer, "/list",    list_handler);
    addUri(uiServer, "/photo",   photo_handler);
    addUri(uiServer, "/delete",  delete_handler);
  } else {
    Serial.println("Failed to start the web server on port 80");
  }

  // Stream server on port 81, so a running stream never blocks the page
  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = 81;
  streamConfig.ctrl_port = config.ctrl_port + 1;   // must differ from the first server
  streamConfig.lru_purge_enable = true;

  httpd_handle_t streamServer = NULL;
  if(httpd_start(&streamServer, &streamConfig) == ESP_OK){
    addUri(streamServer, "/stream", stream_handler);
  } else {
    Serial.println("Failed to start the stream server on port 81");
  }
}
