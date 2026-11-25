#include <Arduino.h>
#include <time.h>
#include <stdarg.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#if defined(ESP32)
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPUpdate.h>
#include <esp_system.h>
using WebServerT = WebServer;
#define UPDATER httpUpdate
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266httpUpdate.h>
using WebServerT = ESP8266WebServer;
#define UPDATER ESPhttpUpdate
#endif

#include "rstcount.h"
#include "version.h"

// ---------------------------------------------------------------- настройки

#if defined(ARDUINO_ESP32C3_DEV)
static const uint8_t LED = 8;
#else
static const uint8_t LED = 2;
#endif
static const bool LED_ACTIVE_LOW = true; // светодиод горит при низком уровне

static const char *AP_NAME = "Power-Monitor-Setup";
static const char *MDNS_HOST = "power-monitor"; // http://power-monitor.local
static const char *CONFIG_PATH = "/config.json";
static const size_t URL_MAX = 200;

static const uint32_t HEARTBEAT_INTERVAL_MS = 60UL * 1000;   // период отправки
static const uint32_t HTTP_TIMEOUT_MS = 15UL * 1000;
static const uint32_t WIFI_LOST_REBOOT_MS = 2UL * 60 * 1000; // сколько ждать Wi-Fi до перезагрузки
static const uint32_t PORTAL_TIMEOUT_S = 180;                // после этого портал закрывается и плата перезагружается
static const uint32_t NTP_TIMEOUT_MS = 60UL * 1000;
static const time_t MIN_VALID_EPOCH = 1700000000;

// --- Обновление через GitHub Releases (репозиторий должен быть публичным) ---
static const char *GITHUB_REPO = "pup22/Electricity_monitor_ESP";               // репозиторий
static const bool AUTO_UPDATE_CHECK = true;                                     // периодически проверять релизы
static const bool AUTO_INSTALL_UPDATES = true;                                  // true — ставить сразу, false — только сообщать
static const uint32_t UPDATE_FIRST_CHECK_MS = 2UL * 60 * 1000;
static const uint32_t UPDATE_CHECK_INTERVAL_MS = 6UL * 60 * 60 * 1000;

// Имя файла в релизе = имя окружения PlatformIO
#if defined(ESP8266)
#define FW_ASSET "firmware-esp12e.bin"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define FW_ASSET "firmware-esp32c3.bin"
#else
#define FW_ASSET "firmware-esp32.bin"
#endif

// ---------------------------------------------------------------- состояние

static char scriptUrl[URL_MAX] = "";
static bool shouldSaveConfig = false;
static uint32_t bootTime = 0; // Unix-время включения питания

static WebServerT server(80);

// статистика heartbeat
static uint32_t lastBeatEpoch = 0;
static int lastBeatCode = 0;
static bool lastBeatOk = false;
static uint32_t beatsSent = 0;
static uint32_t beatsFailed = 0;

// обновление
static char updateStatus[112] = "Не проверялось";
static char latestVersion[40] = "";
static uint32_t lastUpdateCheck = 0;
static volatile bool updateRequested = false;
static bool updating = false;

// ---------------------------------------------------------------- светодиод

static bool ledLit = false;

static void setLed(bool on)
{
    ledLit = on;
    digitalWrite(LED, (on != LED_ACTIVE_LOW) ? HIGH : LOW);
}

// ------------------------------------------------- boot_time, переживающий soft-reset
//
// boot_time = момент включения питания. Если плата перезагрузилась программно
// (WDT, потеря Wi-Fi, кнопка Reset, обновление прошивки), питание при этом не
// пропадало, поэтому прежнее значение нужно сохранить — иначе скрипт Google
// решит, что было отключение света. После реального пропадания питания
// память RTC теряется.

struct BootRecord
{
    uint32_t magic;
    uint32_t bootTime;
    uint32_t check;
};
static const uint32_t BOOT_MAGIC = 0x50574D4E; // "PWMN"

static uint32_t recordCheck(const BootRecord &r) { return r.magic ^ r.bootTime ^ 0xA5A5A5A5UL; }

#if defined(ESP32)
RTC_NOINIT_ATTR static BootRecord rtcRecord;

static bool powerWasCycled()
{
    esp_reset_reason_t r = esp_reset_reason();
    return r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_UNKNOWN;
}

static bool loadBootTime(uint32_t &out)
{
    if (powerWasCycled()) return false;
    if (rtcRecord.magic != BOOT_MAGIC || rtcRecord.check != recordCheck(rtcRecord)) return false;
    out = rtcRecord.bootTime;
    return true;
}

static void storeBootTime(uint32_t value)
{
    rtcRecord.magic = BOOT_MAGIC;
    rtcRecord.bootTime = value;
    rtcRecord.check = recordCheck(rtcRecord);
}

#elif defined(ESP8266)
static const uint32_t RTC_SLOT = 4; // слот 0 занят счётчиком Reset

static bool loadBootTime(uint32_t &out)
{
    if (ESP.getResetInfoPtr()->reason == REASON_DEFAULT_RST) return false; // включение питания
    BootRecord r;
    ESP.rtcUserMemoryRead(RTC_SLOT, (uint32_t *)&r, sizeof(r));
    if (r.magic != BOOT_MAGIC || r.check != recordCheck(r)) return false;
    out = r.bootTime;
    return true;
}

static void storeBootTime(uint32_t value)
{
    BootRecord r{BOOT_MAGIC, value, 0};
    r.check = recordCheck(r);
    ESP.rtcUserMemoryWrite(RTC_SLOT, (uint32_t *)&r, sizeof(r));
}
#endif

// ---------------------------------------------------------------- утилиты

static time_t nowEpoch()
{
    time_t t = time(nullptr);
    return (t < MIN_VALID_EPOCH) ? 0 : t;
}

static void saveConfigCallback()
{
    Serial.println("WiFiManager: config save requested");
    shouldSaveConfig = true;
}

static void loadConfig()
{
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) return;

    JsonDocument doc;
    if (!deserializeJson(doc, f))
    {
        strlcpy(scriptUrl, doc["script_url"] | "", sizeof(scriptUrl));
        Serial.printf("Loaded URL: %s\n", scriptUrl);
    }
    f.close();
}

static void saveConfig()
{
    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f)
    {
        Serial.println("Error: cannot open config for writing");
        return;
    }
    JsonDocument doc;
    doc["script_url"] = scriptUrl;
    serializeJson(doc, f);
    f.close();
    Serial.println("Config saved");
}

static void mountFS()
{
    Serial.println("Mounting LittleFS...");
#if defined(ESP32)
    if (!LittleFS.begin(true)) // true = форматировать при ошибке
        Serial.println("LittleFS mount failed (check partition table)");
#else
    if (!LittleFS.begin())
    {
        Serial.println("Formatting LittleFS...");
        LittleFS.format();
        LittleFS.begin();
    }
#endif
}

static void restartWithDelay(const char *reason)
{
    Serial.printf("%s Restarting...\n", reason);
    delay(2000);
    ESP.restart();
}

// ---------------------------------------------------------------- отправка heartbeat

static bool sendHeartbeat()
{
    if (strlen(scriptUrl) < 10)
    {
        Serial.println("Heartbeat skipped: Script URL is empty. Triple-click Reset to configure.");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
#if defined(ESP8266)
    client.setBufferSizes(1024, 512); // экономия RAM на ESP8266
#endif

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    // Apps Script отвечает на POST редиректом 302, но сам POST к этому моменту
    // уже обработан, поэтому редиректы не нужны.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    if (!http.begin(client, scriptUrl))
    {
        Serial.println("Heartbeat: http.begin failed");
        return false;
    }
    http.addHeader("Content-Type", "application/json");

    char payload[48];
    snprintf(payload, sizeof(payload), "{\"boot_time\":%lu}", (unsigned long)bootTime);

    int code = http.POST((uint8_t *)payload, strlen(payload));
    http.end();

    bool ok = (code == 200 || code == 301 || code == 302 || code == 303);

    lastBeatEpoch = (uint32_t)nowEpoch();
    lastBeatCode = code;
    lastBeatOk = ok;
    beatsSent++;
    if (!ok) beatsFailed++;

    if (code > 0)
        Serial.printf("Heartbeat: HTTP %d %s\n", code, ok ? "OK" : "(unexpected)");
    else
        Serial.printf("Heartbeat failed: %s\n", HTTPClient::errorToString(code).c_str());
    return ok;
}

// ---------------------------------------------------------------- обновление через GitHub

static void setUpdateStatus(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(updateStatus, sizeof(updateStatus), fmt, ap);
    va_end(ap);
    Serial.printf("Update: %s\n", updateStatus);
}

static void parseVersion(const char *s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (*s == 'v' || *s == 'V') s++;
    sscanf(s, "%d.%d.%d", &out[0], &out[1], &out[2]);
}

// >0 если a новее b
static int compareVersions(const char *a, const char *b)
{
    int va[3], vb[3];
    parseVersion(a, va);
    parseVersion(b, vb);
    for (int i = 0; i < 3; i++)
        if (va[i] != vb[i]) return va[i] > vb[i] ? 1 : -1;
    return 0;
}

static bool githubConfigured()
{
    return strchr(GITHUB_REPO, '/') && !strstr(GITHUB_REPO, "OWNER");
}

// Читает tag_name последнего релиза через GitHub API
static bool fetchLatestTag(char *tag, size_t n)
{
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.useHTTP10(true); // без chunked — можно разбирать JSON прямо из потока
    http.setUserAgent("power-monitor/" FW_VERSION); // GitHub API требует User-Agent

    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", GITHUB_REPO);

    if (!http.begin(client, url))
    {
        setUpdateStatus("Ошибка: не удалось открыть api.github.com");
        return false;
    }
    http.addHeader("Accept", "application/vnd.github+json");

    int code = http.GET();
    if (code != 200)
    {
        if (code == 404)
            setUpdateStatus("Релизы не найдены (HTTP 404)");
        else if (code > 0)
            setUpdateStatus("Ошибка GitHub API: HTTP %d", code);
        else
            setUpdateStatus("Ошибка связи с GitHub: %s", HTTPClient::errorToString(code).c_str());
        http.end();
        return false;
    }

    JsonDocument filter;
    filter["tag_name"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    const char *t = doc["tag_name"] | "";
    if (err || !*t)
    {
        setUpdateStatus("Не удалось разобрать ответ GitHub");
        return false;
    }
    strlcpy(tag, t, n);
    return true;
}

static void performUpdate(const char *tag)
{
    char url[220];
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/%s", GITHUB_REPO, tag, FW_ASSET);

    WiFiClientSecure client;
    client.setInsecure();

    UPDATER.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // github.com -> CDN
    UPDATER.rebootOnUpdate(true);

    setUpdateStatus("Загрузка версии %s...", tag);
    updating = true;
    setLed(true);

    t_httpUpdate_return r = UPDATER.update(client, url);

    // сюда попадаем только при ошибке (при успехе плата перезагрузится)
    updating = false;
    setLed(false);
    switch (r)
    {
    case HTTP_UPDATE_FAILED:
        setUpdateStatus("Ошибка обновления (%d): %s", UPDATER.getLastError(), UPDATER.getLastErrorString().c_str());
        break;
    case HTTP_UPDATE_NO_UPDATES:
        setUpdateStatus("Сервер не вернул обновление");
        break;
    default:
        setUpdateStatus("Обновление завершено");
        break;
    }
}

static void runUpdateCheck(bool install)
{
    if (!githubConfigured())
    {
        setUpdateStatus("GitHub-репозиторий не настроен (GITHUB_REPO)");
        return;
    }

    setUpdateStatus("Проверка обновлений...");
    char tag[40];
    if (!fetchLatestTag(tag, sizeof(tag))) return;

    strlcpy(latestVersion, tag, sizeof(latestVersion));
    lastUpdateCheck = (uint32_t)nowEpoch();

    if (compareVersions(tag, FW_VERSION) <= 0)
    {
        setUpdateStatus("Установлена актуальная версия");
        return;
    }

    if (!install)
    {
        setUpdateStatus("Доступна версия %s — нажмите «Обновить»", tag);
        return;
    }
    performUpdate(tag);
}

// ---------------------------------------------------------------- веб-страница

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Power Monitor</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1c1e21;--mut:#6b7280;--ok:#16a34a;--bad:#dc2626;--bd:#e5e7eb;--btn:#2563eb}
@media(prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#1a1d23;--fg:#e8eaed;--mut:#9aa0a6;--bd:#2b2f36;--btn:#3b82f6}}
body{margin:0;font:15px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:560px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:8px 0 16px;display:flex;align-items:center;gap:8px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--mut);display:inline-block}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:14px 16px;margin-bottom:12px}
.card h2{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--mut);margin:0 0 8px}
.row{display:flex;justify-content:space-between;gap:12px;padding:4px 0}
.row span:first-child{color:var(--mut);white-space:nowrap}
.row span:last-child{text-align:right;word-break:break-word}
.ok{color:var(--ok)}.bad{color:var(--bad)}
button{margin-top:10px;width:100%;padding:10px;border:0;border-radius:8px;background:var(--btn);color:#fff;font:inherit;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
</style></head><body><main>
<h1><span class="dot" id="dot"></span><span id="conn">Подключение…</span></h1>

<div class="card"><h2>Питание</h2>
<div class="row"><span>Включено с</span><span id="since">—</span></div>
<div class="row"><span>Питание есть уже</span><span id="pdur">—</span></div>
<div class="row"><span>Работа с перезагрузки</span><span id="up">—</span></div>
</div>

<div class="card"><h2>Отправка в Google</h2>
<div class="row"><span>Последний сигнал</span><span id="hb">—</span></div>
<div class="row"><span>Отправлено / ошибок</span><span id="cnt">—</span></div>
<div class="row"><span>Google Script URL</span><span id="url">—</span></div>
</div>

<div class="card"><h2>Сеть</h2>
<div class="row"><span>Wi-Fi</span><span id="ssid">—</span></div>
<div class="row"><span>Сигнал</span><span id="rssi">—</span></div>
<div class="row"><span>IP-адрес</span><span id="ip">—</span></div>
<div class="row"><span>Имя в сети</span><span id="host">—</span></div>
</div>

<div class="card"><h2>Прошивка</h2>
<div class="row"><span>Установлена</span><span id="ver">—</span></div>
<div class="row"><span>Последняя на GitHub</span><span id="latest">—</span></div>
<div class="row"><span>Последняя проверка</span><span id="chk">—</span></div>
<div class="row"><span>Статус</span><span id="ust">—</span></div>
<div class="row"><span>Свободная память</span><span id="heap">—</span></div>
<button id="btn">Проверить и обновить</button>
</div>
</main>
<script>
const $=i=>document.getElementById(i);
const t=s=>s?new Date(s*1000).toLocaleString():'—';
function dur(s){s=Math.max(0,s|0);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
return(d?d+' д. ':'')+h+' ч. '+m+' мин.'}
async function load(){
 try{
  const d=await(await fetch('/status',{cache:'no-store'})).json();
  $('dot').style.background='var(--ok)';$('conn').textContent='Устройство на связи';
  $('since').textContent=t(d.boot_time);
  $('pdur').textContent=dur(d.now-d.boot_time);
  $('up').textContent=dur(d.uptime);
  $('hb').innerHTML=d.hb_time?t(d.hb_time)+' · <b class="'+(d.hb_ok?'ok':'bad')+'">'+(d.hb_ok?'OK':'ошибка')+'</b> (HTTP '+d.hb_code+')':'ещё не отправлялся';
  $('cnt').textContent=d.hb_sent+' / '+d.hb_failed;
  $('url').textContent=d.url_set?'задан':'не задан';
  $('ssid').textContent=d.ssid;$('rssi').textContent=d.rssi+' дБм';
  $('ip').textContent=d.ip;$('host').textContent=d.host;
  $('ver').textContent=d.version;$('latest').textContent=d.latest||'—';
  $('chk').textContent=t(d.last_check);$('ust').textContent=d.update_status;
  $('heap').textContent=Math.round(d.heap/1024)+' КБ';
  $('btn').disabled=d.update_busy;
 }catch(e){$('dot').style.background='var(--bad)';$('conn').textContent='Нет связи с устройством'}
}
$('btn').onclick=async()=>{$('btn').disabled=true;try{await fetch('/update',{method:'POST'})}catch(e){}setTimeout(load,1500)};
load();setInterval(load,5000);
</script></body></html>)HTML";

static void handleRoot()
{
    server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleStatus()
{
    JsonDocument d;
    d["version"] = FW_VERSION;
    d["latest"] = latestVersion;
    d["update_status"] = updateStatus;
    d["update_busy"] = updating || updateRequested;
    d["last_check"] = lastUpdateCheck;
    d["now"] = (uint32_t)nowEpoch();
    d["boot_time"] = bootTime;
    d["uptime"] = millis() / 1000;
    d["hb_time"] = lastBeatEpoch;
    d["hb_code"] = lastBeatCode;
    d["hb_ok"] = lastBeatOk;
    d["hb_sent"] = beatsSent;
    d["hb_failed"] = beatsFailed;
    d["url_set"] = strlen(scriptUrl) >= 10;
    d["ssid"] = WiFi.SSID();
    d["rssi"] = WiFi.RSSI();
    d["ip"] = WiFi.localIP().toString();
    d["host"] = String(MDNS_HOST) + ".local";
    d["heap"] = ESP.getFreeHeap();

    String out;
    serializeJson(d, out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

static void handleUpdate()
{
    if (updating || updateRequested)
    {
        server.send(409, "application/json", "{\"ok\":false}");
        return;
    }
    updateRequested = true; // выполняется в loop(), после отправки ответа
    server.send(202, "application/json", "{\"ok\":true}");
}

static void startWeb()
{
    if (MDNS.begin(MDNS_HOST))
    {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local\n", MDNS_HOST);
    }
    else
    {
        Serial.println("mDNS start failed");
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/update", HTTP_POST, handleUpdate);
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
    server.begin();
    Serial.printf("Web: http://%s\n", WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------- setup / loop

void setup()
{
    pinMode(LED, OUTPUT);
    setLed(true); // горит, пока нет связи

    Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    // USB-CDC (ESP32-C3): ждём, пока монитор порта подключится, но не вечно
    for (uint32_t t = millis(); !Serial && millis() - t < 3000;) delay(10);
#endif
    Serial.printf("\n\n--- START (firmware %s) ---\n", FW_VERSION);

    bool factoryReset = resetClick(); // окно ~1 с при загрузке
    mountFS();

    WiFiManager wm;
    if (factoryReset)
    {
        Serial.println("Reset x3 detected: wiping settings");
        WiFi.mode(WIFI_STA);
        wm.resetSettings();
        LittleFS.remove(CONFIG_PATH);
        scriptUrl[0] = '\0';
        shouldSaveConfig = true;
    }
    else
    {
        loadConfig();
    }

    WiFiManagerParameter urlParam("script", "Google Script URL", scriptUrl, URL_MAX - 1);
    wm.addParameter(&urlParam);
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setHostname(MDNS_HOST);
    wm.setConnectTimeout(30);
    // Без таймаута портал после отключения света висел бы вечно, если
    // роутер загружается медленнее платы.
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);

    if (!wm.autoConnect(AP_NAME))
        restartWithDelay("WiFi connect/portal timeout.");

    strlcpy(scriptUrl, urlParam.getValue(), sizeof(scriptUrl));
    if (shouldSaveConfig) saveConfig();

    Serial.println("WiFi connected");
    WiFi.setAutoReconnect(true);

    // NTP
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Waiting for NTP");
    uint32_t start = millis();
    while (!nowEpoch())
    {
        if (millis() - start > NTP_TIMEOUT_MS)
            restartWithDelay("\nNTP timeout.");
        setLed(!ledLit);
        Serial.print(".");
        delay(500);
    }
    setLed(false);

    // Момент включения питания
    uint32_t now = (uint32_t)nowEpoch();
    uint32_t saved;
    if (loadBootTime(saved) && saved >= (uint32_t)MIN_VALID_EPOCH && saved <= now)
    {
        bootTime = saved;
        Serial.printf("\nBoot time restored (soft reset): %lu\n", (unsigned long)bootTime);
    }
    else
    {
        bootTime = now - millis() / 1000;
        storeBootTime(bootTime);
        Serial.printf("\nBoot time established: %lu\n", (unsigned long)bootTime);
    }

    startWeb();
}

void loop()
{
    static bool firstRun = true;
    static uint32_t lastBeat = 0;
    static bool offline = false;
    static uint32_t offlineSince = 0;
    static uint32_t updateWaitStart = millis();
    static uint32_t updateWait = UPDATE_FIRST_CHECK_MS;

    uint32_t ms = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
        if (!offline)
        {
            offline = true;
            offlineSince = ms;
            setLed(true);
            Serial.println("WiFi lost, waiting for reconnect...");
        }
        if (ms - offlineSince > WIFI_LOST_REBOOT_MS)
            restartWithDelay("WiFi not restored.");
        delay(200);
        return;
    }

    if (offline)
    {
        offline = false;
        setLed(false);
        Serial.println("WiFi restored");
    }

#if defined(ESP8266)
    MDNS.update();
#endif
    server.handleClient();

    // Ручной запуск обновления с веб-страницы
    if (updateRequested)
    {
        runUpdateCheck(true);
        updateRequested = false;
        updateWaitStart = millis();
        updateWait = UPDATE_CHECK_INTERVAL_MS;
        return;
    }

    // Периодическая проверка релизов
    if (AUTO_UPDATE_CHECK && ms - updateWaitStart >= updateWait)
    {
        updateWaitStart = ms;
        updateWait = UPDATE_CHECK_INTERVAL_MS;
        runUpdateCheck(AUTO_INSTALL_UPDATES);
        return;
    }

    if (firstRun || ms - lastBeat >= HEARTBEAT_INTERVAL_MS)
    {
        firstRun = false;
        lastBeat = ms;
        sendHeartbeat();
    }

    delay(5);


    // check GH
    const int ledPin = 2;              // GPIO2 (часто встроенный светодиод на модулях ESP-12)
    const unsigned long interval = 500; // Интервал переключения в миллисекундах (500 мс = 0.5 сек)
    static unsigned long previousMillis = 0;   // Время последнего переключения
    static bool ledState = false;              // Текущее состояние светодиода
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval)
    {
        previousMillis = currentMillis;
        ledState = !ledState;
        digitalWrite(ledPin, ledState ? HIGH : LOW);
    }
}
