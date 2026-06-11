#include "web_manager.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <WiFi.h>
#include "config.h"
#include "logger.h"

namespace aq {

WebManager Web;

namespace {

constexpr const char* kConfigPath = "/config.json";
constexpr const char* kConfigUploadPath = "/config_upload.tmp";

const char kDashboardHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CoreS3 Air Station</title>
<style>
:root{color-scheme:dark;--bg:#121212;--card:rgba(42,42,42,.68);--line:rgba(255,255,255,.18);--text:#f5f5f5;--muted:#a9a9a9;--good:#00e676;--warn:#ffd600;--bad:#ff1744}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 30% 0,#202823 0,#121212 45%,#090909 100%);color:var(--text);font:14px/1.4 system-ui,-apple-system,Segoe UI,sans-serif}
header{position:sticky;top:0;z-index:2;padding:14px 16px;background:rgba(18,18,18,.86);backdrop-filter:blur(10px);border-bottom:1px solid var(--line);display:flex;gap:12px;justify-content:space-between;align-items:center}
h1{font-size:18px;margin:0}.status{color:var(--muted);font-size:12px}.wrap{max-width:1120px;margin:auto;padding:16px;display:grid;gap:16px}
.grid{display:grid;grid-template-columns:1.1fr 1fr;gap:16px}.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:14px;box-shadow:0 18px 40px rgba(0,0,0,.26)}
.hero{display:grid;grid-template-columns:160px 1fr;gap:16px;align-items:center}.gauge{width:150px;height:150px;border-radius:50%;display:grid;place-items:center;background:conic-gradient(var(--good) 0deg,#303030 0);box-shadow:0 0 30px rgba(0,230,118,.16)}
.gauge:before{content:"";position:absolute}.gauge-inner{width:118px;height:118px;border-radius:50%;background:#151515;display:grid;place-items:center;text-align:center}.big{font-size:34px;font-weight:750}.label{color:var(--muted);font-size:12px}
.metrics{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.metric{padding:12px;border-radius:8px;background:rgba(255,255,255,.05);border:1px solid var(--line)}.metric b{font-size:20px;display:block;margin-top:4px}
.tabs,.files-tabs{display:flex;gap:8px;flex-wrap:wrap}.tabs button,.files-tabs button,.download{border:1px solid var(--line);background:rgba(255,255,255,.06);color:var(--text);border-radius:6px;padding:8px 10px;text-decoration:none}.tabs button.active,.files-tabs button.active{background:var(--good);color:#101010;font-weight:700}
canvas{width:100%;height:260px;background:rgba(0,0,0,.16);border-radius:8px}.summary{display:flex;gap:14px;flex-wrap:wrap;color:var(--muted);margin-top:8px}
.alert{display:none;background:var(--bad);color:#fff;border-radius:6px;padding:10px 12px;font-weight:800}.alert.on{display:block}.files{display:grid;gap:8px}.file{display:grid;grid-template-columns:1fr auto;gap:8px;align-items:center;padding:10px;border-radius:8px;background:rgba(255,255,255,.05);border:1px solid var(--line)}.thumbs{display:grid;grid-template-columns:repeat(auto-fill,minmax(116px,1fr));gap:10px}.thumb{aspect-ratio:4/3;object-fit:cover;width:100%;border-radius:8px;border:1px solid var(--line);background:#222}
.config-actions,.config-actions form{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.config-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin:10px 0 14px}.file-input{max-width:100%;border:1px solid var(--line);border-radius:6px;padding:8px;background:rgba(255,255,255,.05);color:var(--text)}.primary{border:1px solid var(--line);background:var(--good);color:#101010;border-radius:6px;padding:8px 10px;font-weight:800}
@media(max-width:760px){.grid,.hero,.config-grid{grid-template-columns:1fr}.metrics{grid-template-columns:repeat(2,1fr)}canvas{height:220px}}
</style>
</head>
<body>
<header><div><h1>CoreS3 Air Station</h1><div class="status" id="status">Connecting...</div></div><div class="status" id="time">--</div></header>
<main class="wrap">
<section class="grid">
<div class="card hero">
  <div class="gauge" id="gauge"><div class="gauge-inner"><div><div class="big" id="pm25">--</div><div class="label">PM2.5 ug/m3</div></div></div></div>
  <div><div class="alert" id="alert">AIR QUALITY ALERT</div><p class="label">Live readings from the SEN66. The color follows the same good/warning/bad tint as the device display.</p></div>
</div>
<div class="card metrics">
  <div class="metric"><span class="label">CO2</span><b id="co2">--</b></div><div class="metric"><span class="label">Temp</span><b id="temp">--</b></div><div class="metric"><span class="label">Humidity</span><b id="hum">--</b></div>
  <div class="metric"><span class="label">VOC</span><b id="voc">--</b></div><div class="metric"><span class="label">NOx</span><b id="nox">--</b></div><div class="metric"><span class="label">PM10</span><b id="pm10">--</b></div>
</div>
</section>
<section class="card">
<div class="tabs"><button data-mode="day" class="active">24 Hours</button><button data-mode="month">30 Days</button><button data-mode="filter">Filter</button></div>
<canvas id="chart" width="900" height="320"></canvas><div class="summary" id="summary"></div>
</section>
<section class="grid">
<div class="card"><h2>Downloads</h2><div class="files-tabs"><button data-dir="/log" class="active">Logs</button><button data-dir="/cam">Images</button></div><div class="files" id="files"></div></div>
<div class="card"><h2>Camera Roll</h2><div class="thumbs" id="thumbs"></div></div>
</section>
<section class="card">
<h2>Configuration</h2>
<h3>MQTT</h3>
<div class="config-grid">
  <div class="metric"><span class="label">Status</span><b id="mqttStatus">--</b></div>
  <div class="metric"><span class="label">Host</span><b id="mqttHost">--</b></div>
  <div class="metric"><span class="label">Topic</span><b id="mqttTopic">--</b></div>
  <div class="metric"><span class="label">Client</span><b id="mqttClient">--</b></div>
</div>
<div class="config-actions">
  <a class="download" href="/config.json">Download config.json</a>
  <form method="post" action="/config.json" enctype="multipart/form-data">
    <input class="file-input" type="file" name="config" accept=".json,application/json">
    <button class="primary" type="submit">Upload and restart</button>
  </form>
</div>
</section>
</main>
<script>
let mode='day', fileDir='/log';
const $=id=>document.getElementById(id);
function fmt(n,d=1){return Number.isFinite(n)?n.toFixed(d):'--'}
function short(v){return v&&String(v).length?String(v):'--'}
function tint(pm,co2,voc){if(pm>=55||co2>=1500||voc>=300)return '#ff1744';if(pm>=25||co2>=1000||voc>=150)return '#ffd600';return '#00e676'}
async function live(){const r=await fetch('/api/live');const j=await r.json();const s=j.sample||{};const m=j.config?.mqtt||{};const c=tint(s.pm25_ugm3,s.co2_ppm,s.voc_index);$('pm25').textContent=fmt(s.pm25_ugm3);$('co2').textContent=(s.co2_ppm||'--')+' ppm';$('temp').textContent=fmt(s.temp_c)+' C';$('hum').textContent=fmt(s.humidity_pct,0)+'%';$('voc').textContent=fmt(s.voc_index,0);$('nox').textContent=fmt(s.nox_index,0);$('pm10').textContent=fmt(s.pm10_ugm3);$('gauge').style.background=`conic-gradient(${c} ${Math.min(360,(s.pm25_ugm3||0)*6)}deg,#303030 0)`;$('alert').className='alert '+(j.alert?'on':'');$('status').textContent=`WiFi ${j.wifi?'ok':'--'}  MQTT ${j.mqtt?'ok':'--'}  SD ${j.sd?'ok':'--'}  ${j.state}  ${j.ip||''}`;$('time').textContent=new Date((j.epoch||0)*1000).toLocaleTimeString();$('mqttStatus').textContent=m.host?(j.mqtt?'connected':'offline'):'off';$('mqttHost').textContent=m.host?`${m.host}:${m.port}`:'off';$('mqttTopic').textContent=short(m.topic);$('mqttClient').textContent=short(m.client_id);}
async function history(){const r=await fetch('/api/history?mode='+mode);const j=await r.json();draw(j.points||[]);$('summary').textContent=mode==='filter'?`Now ${j.current_percent||0}%   Peak ${j.peak_percent||0}%   Avg ${j.mean_percent||0}%`:`Mean ${fmt(j.mean)}   Min ${fmt(j.min)}   Max ${fmt(j.max)}`;}
function draw(p){const cv=$('chart'),ctx=cv.getContext('2d');ctx.clearRect(0,0,cv.width,cv.height);ctx.strokeStyle='rgba(255,255,255,.12)';ctx.lineWidth=1;for(let i=1;i<5;i++){let y=i*cv.height/5;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(cv.width,y);ctx.stroke()}if(!p.length)return;let vals=p.map(x=>x.v),min=Math.min(...vals,0),max=Math.max(...vals,10);ctx.strokeStyle=mode==='filter'?'#ffd600':'#00e676';ctx.lineWidth=4;ctx.beginPath();p.forEach((pt,i)=>{let x=i*Math.max(1,cv.width/(p.length-1));let y=cv.height-((pt.v-min)/(max-min||1))*cv.height;if(i)ctx.lineTo(x,y);else ctx.moveTo(x,y)});ctx.stroke();}
async function files(){const r=await fetch('/api/files?dir='+encodeURIComponent(fileDir));const j=await r.json();$('files').innerHTML=(j.files||[]).map(f=>`<div class="file"><span>${f.name}<br><small>${f.size} bytes</small></span><a class="download" href="/download?path=${encodeURIComponent(f.path)}">Download</a></div>`).join('')||'<p class="label">No files yet.</p>';if(fileDir==='/cam')$('thumbs').innerHTML=(j.files||[]).slice(0,12).map(f=>`<a href="/image?path=${encodeURIComponent(f.path)}"><img class="thumb" src="/image?path=${encodeURIComponent(f.path)}" loading="lazy"></a>`).join('');}
document.querySelectorAll('.tabs button').forEach(b=>b.onclick=()=>{document.querySelectorAll('.tabs button').forEach(x=>x.classList.remove('active'));b.classList.add('active');mode=b.dataset.mode;history();});
document.querySelectorAll('.files-tabs button').forEach(b=>b.onclick=()=>{document.querySelectorAll('.files-tabs button').forEach(x=>x.classList.remove('active'));b.classList.add('active');fileDir=b.dataset.dir;files();});
live();history();files();setInterval(live,5000);setInterval(history,30000);
</script>
</body>
</html>
)HTML";

String jsonEscape(const String& value) {
    String out;
    out.reserve(value.length() + 4);
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

}  // namespace

bool WebManager::begin() {
    mutex_ = xSemaphoreCreateMutex();
    registerRoutes();
    return true;
}

void WebManager::service() {
    if (serverStarted_) {
        server_.handleClient();
    }
    if (rebootPending_ && static_cast<int32_t>(millis() - rebootAtMs_) >= 0) {
        ESP.restart();
    }
}

void WebManager::updateSample(const SensorSample& sample, bool alertActive, DeviceState state) {
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        latest_ = sample;
        sampleReady_ = sample.valid;
        alertActive_ = alertActive;
        state_ = state;
        xSemaphoreGive(mutex_);
    }
}

void WebManager::updateNetwork(bool wifiConnected, bool mqttConnected) {
    wifiConnected_ = wifiConnected;
    mqttConnected_ = mqttConnected;
    if (wifiConnected_ && !serverStarted_) {
        server_.begin();
        serverStarted_ = true;
        Serial.println("[web] HTTP server started");
    } else if (!wifiConnected_ && serverStarted_) {
        server_.stop();
        serverStarted_ = false;
        Serial.println("[web] HTTP server stopped");
    }
}

void WebManager::registerRoutes() {
    server_.on("/", HTTP_GET, [this]() { sendDashboard(); });
    server_.on("/api/live", HTTP_GET, [this]() { sendLiveJson(); });
    server_.on("/api/history", HTTP_GET, [this]() { sendHistoryJson(); });
    server_.on("/api/files", HTTP_GET, [this]() { sendFileList(); });
    server_.on("/download", HTTP_GET, [this]() { sendDownload(); });
    server_.on("/image", HTTP_GET, [this]() { sendImage(); });
    server_.on("/config.json", HTTP_GET, [this]() { sendConfigDownload(); });
    server_.on("/config.json", HTTP_POST, [this]() { finishConfigUpload(); }, [this]() { handleConfigUpload(); });
    server_.onNotFound([this]() { sendNotFound(); });
}

void WebManager::sendDashboard() {
    server_.send_P(200, "text/html", kDashboardHtml);
}

void WebManager::sendLiveJson() {
    SensorSample sample;
    bool ready = false;
    bool alert = false;
    DeviceState currentState = DeviceState::Active;
    if (mutex_ && xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        sample = latest_;
        ready = sampleReady_;
        alert = alertActive_;
        currentState = state_;
        xSemaphoreGive(mutex_);
    }

    JsonDocument doc;
    doc["epoch"] = static_cast<uint32_t>(time(nullptr));
    doc["wifi"] = wifiConnected_;
    doc["mqtt"] = mqttConnected_;
    doc["sd"] = Config.sdReady();
    doc["alert"] = alert;
    doc["state"] = stateName(currentState);
    doc["ip"] = WiFi.localIP().toString();
    JsonObject s = doc["sample"].to<JsonObject>();
    s["valid"] = ready;
    s["ts"] = sample.timestamp;
    s["pm1_ugm3"] = sample.pm1p0;
    s["pm25_ugm3"] = sample.pm2p5;
    s["pm4_ugm3"] = sample.pm4p0;
    s["pm10_ugm3"] = sample.pm10p0;
    s["co2_ppm"] = sample.co2;
    s["voc_index"] = sample.vocIndex;
    s["nox_index"] = sample.noxIndex;
    s["temp_c"] = sample.temperature;
    s["humidity_pct"] = sample.humidity;

    JsonObject filter = doc["filter"].to<JsonObject>();
    const AppSettings& settings = Config.settings();
    filter["active"] = settings.filterBaselineActive;
    filter["ready"] = settings.filterBaselineReady;
    filter["pm25_ugm3"] = settings.filterBaselinePm25;
    filter["pm10_ugm3"] = settings.filterBaselinePm10;

    JsonObject config = doc["config"].to<JsonObject>();
    JsonObject mqtt = config["mqtt"].to<JsonObject>();
    mqtt["host"] = settings.mqttHost;
    mqtt["port"] = settings.mqttPort;
    mqtt["client_id"] = settings.mqttClientId;
    mqtt["topic"] = settings.mqttTopic;

    String json;
    serializeJson(doc, json);
    server_.send(200, "application/json", json);
}

void WebManager::sendHistoryJson() {
    const String mode = server_.arg("mode");
    const Metric metric = metricFromArg(server_.arg("metric"));
    HistoryPoint points[120];
    size_t count = 0;
    FilterDeviationSummary filterSummary;
    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    if (mode == "filter") {
        count = Logger.readFilterDeviation(now, Config.settings(), points, 120, &filterSummary);
    } else if (mode == "month") {
        count = Logger.readMonth(now, metric, points, 120, 120);
    } else {
        count = Logger.readDay(now, metric, points, 120, 120);
    }

    float minValue = NAN;
    float maxValue = NAN;
    float sum = 0.0f;
    String json = "{";
    json += "\"mode\":\"" + mode + "\",\"points\":[";
    for (size_t i = 0; i < count; ++i) {
        const float value = mode == "filter" ? points[i].value : scaledValue(metric, points[i].value);
        if (!isfinite(minValue) || value < minValue) minValue = value;
        if (!isfinite(maxValue) || value > maxValue) maxValue = value;
        sum += value;
        if (i) json += ",";
        json += "{\"t\":" + String(points[i].timestamp) + ",\"v\":" + String(value, 2) + "}";
    }
    json += "]";
    if (mode == "filter") {
        json += ",\"current_percent\":" + String(filterSummary.currentPercent);
        json += ",\"peak_percent\":" + String(filterSummary.peakPercent);
        json += ",\"mean_percent\":" + String(filterSummary.meanPercent);
    } else if (count > 0) {
        json += ",\"mean\":" + String(sum / count, 2);
        json += ",\"min\":" + String(minValue, 2);
        json += ",\"max\":" + String(maxValue, 2);
    }
    json += "}";
    server_.send(200, "application/json", json);
}

void WebManager::sendFileList() {
    String dirPath = server_.arg("dir");
    if (dirPath != "/log" && dirPath != "/cam") {
        server_.send(400, "application/json", "{\"error\":\"dir must be /log or /cam\"}");
        return;
    }

    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) {
        server_.send(404, "application/json", "{\"files\":[]}");
        return;
    }

    String json = "{\"dir\":\"" + dirPath + "\",\"files\":[";
    bool first = true;
    for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
        if (!file.isDirectory()) {
            String path = file.name();
            if (!path.startsWith("/")) {
                path = dirPath + "/" + path;
            }
            if (allowedPath(path)) {
                if (!first) json += ",";
                first = false;
                const int slash = path.lastIndexOf('/');
                const String name = slash >= 0 ? path.substring(slash + 1) : path;
                json += "{\"name\":\"" + jsonEscape(name) + "\",\"path\":\"" + jsonEscape(path) + "\",\"size\":" + String(file.size()) + "}";
            }
        }
        file.close();
    }
    dir.close();
    if (dirPath == "/log" && SD.exists("/alerts.log")) {
        File alerts = SD.open("/alerts.log", FILE_READ);
        if (alerts) {
            if (!first) json += ",";
            json += "{\"name\":\"alerts.log\",\"path\":\"/alerts.log\",\"size\":" + String(alerts.size()) + "}";
            alerts.close();
        }
    }
    json += "]}";
    server_.send(200, "application/json", json);
}

void WebManager::sendDownload() {
    const String path = server_.arg("path");
    if (!allowedPath(path) || !SD.exists(path)) {
        server_.send(404, "text/plain", "File not found");
        return;
    }
    File file = SD.open(path, FILE_READ);
    if (!file) {
        server_.send(500, "text/plain", "Open failed");
        return;
    }
    const int slash = path.lastIndexOf('/');
    const String name = slash >= 0 ? path.substring(slash + 1) : path;
    server_.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    server_.streamFile(file, contentType(path));
    file.close();
}

void WebManager::sendImage() {
    const String path = server_.arg("path");
    if (!path.startsWith("/cam/") || !path.endsWith(".jpg") || !SD.exists(path)) {
        server_.send(404, "text/plain", "Image not found");
        return;
    }
    File file = SD.open(path, FILE_READ);
    if (!file) {
        server_.send(500, "text/plain", "Open failed");
        return;
    }
    server_.streamFile(file, "image/jpeg");
    file.close();
}

void WebManager::sendConfigDownload() {
    if (!SD.exists(kConfigPath)) {
        server_.send(404, "text/plain", "config.json not found");
        return;
    }
    File file = SD.open(kConfigPath, FILE_READ);
    if (!file) {
        server_.send(500, "text/plain", "Open failed");
        return;
    }
    server_.sendHeader("Content-Disposition", "attachment; filename=\"config.json\"");
    server_.streamFile(file, "application/json");
    file.close();
}

void WebManager::handleConfigUpload() {
    HTTPUpload& upload = server_.upload();
    if (upload.status == UPLOAD_FILE_START) {
        configUploadOk_ = false;
        configUploadError_.clear();
        if (!Config.sdReady()) {
            configUploadError_ = "SD card is not mounted";
            return;
        }
        SD.remove(kConfigUploadPath);
        configUploadFile_ = SD.open(kConfigUploadPath, FILE_WRITE);
        if (!configUploadFile_) {
            configUploadError_ = "Unable to create upload file";
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE) {
        if (configUploadFile_) {
            const size_t written = configUploadFile_.write(upload.buf, upload.currentSize);
            if (written != upload.currentSize) {
                configUploadError_ = "Upload write failed";
            }
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_END) {
        if (configUploadFile_) {
            configUploadFile_.close();
        }
        if (configUploadError_.length() > 0) {
            SD.remove(kConfigUploadPath);
            return;
        }

        File file = SD.open(kConfigUploadPath, FILE_READ);
        if (!file) {
            configUploadError_ = "Uploaded file missing";
            return;
        }
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, file);
        file.close();
        if (err) {
            configUploadError_ = String("Invalid JSON: ") + err.c_str();
            SD.remove(kConfigUploadPath);
            return;
        }

        SD.remove(kConfigPath);
        if (!SD.rename(kConfigUploadPath, kConfigPath)) {
            configUploadError_ = "Unable to replace config.json";
            SD.remove(kConfigUploadPath);
            return;
        }
        configUploadOk_ = true;
        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED) {
        if (configUploadFile_) {
            configUploadFile_.close();
        }
        SD.remove(kConfigUploadPath);
        configUploadError_ = "Upload aborted";
    }
}

void WebManager::finishConfigUpload() {
    if (!configUploadOk_) {
        const String error = configUploadError_.length() ? configUploadError_ : String("No config file uploaded");
        configUploadOk_ = false;
        server_.send(400, "text/plain", error);
        return;
    }
    rebootPending_ = true;
    rebootAtMs_ = millis() + 1000;
    configUploadOk_ = false;
    server_.send(200, "text/html", "<!doctype html><meta name=\"viewport\" content=\"width=device-width\"><body style=\"font-family:sans-serif;background:#121212;color:#f5f5f5\"><h1>Config uploaded</h1><p>The station is restarting so the new settings take effect.</p></body>");
}

void WebManager::sendNotFound() {
    server_.send(404, "text/plain", "Not found");
}

Metric WebManager::metricFromArg(const String& value) const {
    if (value == "co2") return Metric::Co2;
    if (value == "pm10") return Metric::Pm10;
    if (value == "temp") return Metric::Temperature;
    if (value == "hum") return Metric::Humidity;
    if (value == "voc") return Metric::Voc;
    if (value == "nox") return Metric::Nox;
    return Metric::Pm25;
}

const char* WebManager::stateName(DeviceState state) const {
    switch (state) {
        case DeviceState::Active: return "ACTIVE";
        case DeviceState::IdleDimmed: return "DIM";
        case DeviceState::LightSleep: return "SLEEP";
        case DeviceState::Warmup: return "WARM";
        case DeviceState::Measuring: return "MEAS";
        case DeviceState::Error: return "ERROR";
    }
    return "UNKNOWN";
}

bool WebManager::allowedPath(const String& path) const {
    if (path.indexOf("..") >= 0) {
        return false;
    }
    return path == "/config.json" || path == "/alerts.log" ||
           path.startsWith("/log/") || path.startsWith("/cam/");
}

String WebManager::contentType(const String& path) const {
    if (path.endsWith(".jpg")) return "image/jpeg";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".log") || path.endsWith(".txt")) return "text/plain";
    return "application/octet-stream";
}

float WebManager::scaledValue(Metric metric, int16_t value) const {
    if (value == INT16_MAX || value == INT16_MIN) {
        return NAN;
    }
    switch (metric) {
        case Metric::Co2: return value;
        case Metric::Temperature:
        case Metric::Humidity:
            return value / 100.0f;
        default:
            return value / 10.0f;
    }
}

}  // namespace aq
