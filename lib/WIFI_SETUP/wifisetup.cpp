#include "wifisetup.h"
#include "config.h"
#include <ArduinoJson.h>
#include <DNSServer.h>

#if defined(ESP32)
#include <WiFi.h>
#define NET_IS_OPEN(i) (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)
#else
#include <ESP8266WiFi.h>
#define NET_IS_OPEN(i) (WiFi.encryptionType(i) == ENC_TYPE_NONE)
#endif

String wifiNote;

static bool portalDone = false;
static uint32_t restartAt = 0;

// ---------------------------------------------------------------- подключение

static void applyHostname()
{
#if defined(ESP32)
    WiFi.setHostname(MDNS_HOST);
#else
    WiFi.hostname(MDNS_HOST);
#endif
}

static bool connectSta(const char *ssid, const char *pass, uint32_t timeoutMs)
{
    Serial.printf("WiFi: connecting to \"%s\"...\n", ssid);
    WiFi.persistent(false); // данные хранит config.json, SDK во flash не пишем
    applyHostname();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass[0] ? pass : nullptr);

    for (uint32_t t = millis(); millis() - t < timeoutMs;)
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.printf("WiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        delay(250);
    }
    Serial.println("WiFi: connection failed");
    WiFi.disconnect();
    return false;
}

bool wifiMigrateLegacy()
{
    Serial.println("WiFi: trying credentials saved by the previous firmware...");
    applyHostname();
    WiFi.mode(WIFI_STA);
    WiFi.begin(); // без аргументов — данные из памяти SDK

    for (uint32_t t = millis(); millis() - t < 20000;)
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            strlcpy(cfg.ssid, WiFi.SSID().c_str(), sizeof(cfg.ssid));
            strlcpy(cfg.pass, WiFi.psk().c_str(), sizeof(cfg.pass));
            configSave();
            Serial.printf("WiFi: migrated \"%s\" to config.json\n", cfg.ssid);
            return true;
        }
        delay(250);
    }
    WiFi.disconnect();
    return false;
}

bool wifiConnectBoot()
{
    if (cfg.pendSsid[0])
    {
        String target = cfg.pendSsid;
        bool ok = connectSta(cfg.pendSsid, cfg.pendPass, WIFI_PENDING_TIMEOUT_MS);

        if (ok)
        {
            strlcpy(cfg.ssid, cfg.pendSsid, sizeof(cfg.ssid));
            strlcpy(cfg.pass, cfg.pendPass, sizeof(cfg.pass));
            wifiNote = "Выполнено подключение к новой сети «" + target + "»";
        }
        else
        {
            wifiNote = "Не удалось подключиться к «" + target + "», использована прежняя сеть";
        }
        cfg.pendSsid[0] = '\0';
        cfg.pendPass[0] = '\0';
        configSave();
        if (ok) return true;
    }

    if (!cfg.ssid[0]) return false;
    return connectSta(cfg.ssid, cfg.pass, WIFI_CONNECT_TIMEOUT_MS);
}

void wifiForget()
{
#if defined(ESP32)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
#else
    WiFi.persistent(true);
    WiFi.disconnect();
    WiFi.persistent(false);
#endif
}

// ---------------------------------------------------------------- список сетей

struct Net
{
    char ssid[SSID_MAX + 1];
    int rssi;
    bool open;
};

static const int MAX_NETS = 32;
static const int MAX_SHOWN = 20;
static Net nets[MAX_NETS];
static int netCount = 0;

static void scanNetworks()
{
    Serial.println("WiFi: scanning...");
    int n = WiFi.scanNetworks();
    netCount = 0;

    for (int i = 0; i < n; i++)
    {
        String ss = WiFi.SSID(i);
        if (ss.length() == 0 || ss.length() > SSID_MAX) continue;
        int rssi = WiFi.RSSI(i);

        int dup = -1;
        for (int j = 0; j < netCount; j++)
            if (ss == nets[j].ssid) dup = j;

        if (dup >= 0)
        {
            if (rssi > nets[dup].rssi) nets[dup].rssi = rssi;
            continue;
        }
        if (netCount >= MAX_NETS) continue;

        strlcpy(nets[netCount].ssid, ss.c_str(), sizeof(nets[netCount].ssid));
        nets[netCount].rssi = rssi;
        nets[netCount].open = NET_IS_OPEN(i);
        netCount++;
    }
    WiFi.scanDelete();

    // сортировка по уровню сигнала (сильные сверху)
    for (int i = 1; i < netCount; i++)
    {
        Net key = nets[i];
        int j = i - 1;
        while (j >= 0 && nets[j].rssi < key.rssi)
        {
            nets[j + 1] = nets[j];
            j--;
        }
        nets[j + 1] = key;
    }
    Serial.printf("WiFi: %d networks found\n", netCount);
}

// ---------------------------------------------------------------- страницы

static const char PAGE_CSS[] PROGMEM = R"CSS(
:root{--bg:#f4f5f7;--card:#fff;--fg:#1c1e21;--mut:#6b7280;--bad:#dc2626;--bd:#e5e7eb;--btn:#2563eb}
@media(prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#1a1d23;--fg:#e8eaed;--mut:#9aa0a6;--bd:#2b2f36;--btn:#3b82f6}}
body{margin:0;font:15px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:560px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:8px 0 16px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:14px 16px;margin-bottom:12px}
.card h2{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--mut);margin:0 0 8px}
.row{display:flex;justify-content:space-between;gap:12px;padding:4px 0}
.row span:first-child{color:var(--mut)}
label{display:block;color:var(--mut);font-size:13px;margin-top:8px}
input{width:100%;box-sizing:border-box;padding:10px;border:1px solid var(--bd);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit;margin:4px 0 8px}
.net{display:flex;justify-content:space-between;gap:12px;padding:9px 10px;border:1px solid var(--bd);border-radius:8px;margin-bottom:6px;cursor:pointer}
button,a.btn{display:block;box-sizing:border-box;margin-top:10px;width:100%;padding:10px;border:0;border-radius:8px;background:var(--btn);color:#fff;font:inherit;text-align:center;text-decoration:none;cursor:pointer}
.sec{background:transparent;color:var(--btn);border:1px solid var(--btn)}
)CSS";

static const char FORM_HTML[] PROGMEM = R"HTML(
<h1>Настройка Wi-Fi</h1>
<div class="card"><h2>Сеть</h2>
<div class="row"><span>{{CURLABEL}}</span><span>{{CUR}}</span></div>
<label>Доступные сети (нажмите, чтобы выбрать)</label>
<div id="nets" style="margin:8px 0">Поиск…</div>
<button type="button" class="sec" id="rescan">Обновить список</button>
</div>
<form class="card" method="POST" action="/wifi/save">
<h2>Подключение</h2>
<label>Название сети (SSID)</label>
<input name="ssid" id="ssid" maxlength="32" required value="{{SSID}}">
<label>Пароль</label>
<input name="pass" type="password" maxlength="63" autocomplete="off" placeholder="{{PH}}">
{{URLFIELD}}
<button type="submit">Сохранить</button>
</form>
{{BACK}}
<script>
const $=i=>document.getElementById(i);
async function scan(r){
 $('nets').textContent='Поиск…';
 try{
  const l=await(await fetch('/wifi/scan'+(r?'?rescan=1':''))).json();
  $('nets').innerHTML='';
  if(!l.length){$('nets').textContent='Сети не найдены';return}
  l.forEach(n=>{
   const d=document.createElement('div');d.className='net';
   const a=document.createElement('span');a.textContent=n.s;
   const b=document.createElement('span');b.textContent=(n.o?'':'🔒 ')+n.r+' дБм';
   d.append(a,b);
   d.onclick=()=>{$('ssid').value=n.s;document.getElementsByName('pass')[0].focus()};
   $('nets').append(d);
  });
 }catch(e){$('nets').textContent='Не удалось получить список'}
}
$('rescan').onclick=()=>scan(true);
scan(false);
</script>
</main></body></html>
)HTML";

static String esc(const String &s)
{
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++)
    {
        char c = s[i];
        switch (c)
        {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

static String pageStart(const char *title)
{
    String p;
    p.reserve(4096);
    p += F("<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>");
    p += title;
    p += F("</title><style>");
    p += FPSTR(PAGE_CSS);
    p += F("</style></head><body><main>");
    return p;
}

static String msgPage(const String &title, const String &text, bool withBack)
{
    String p = pageStart("Wi-Fi");
    p += "<div class=\"card\"><h2>" + esc(title) + "</h2><p>" + esc(text) + "</p>";
    if (withBack) p += F("<a class=\"btn sec\" href=\"/wifi\">Назад</a>");
    p += F("</div></main></body></html>");
    return p;
}

static void sendHtml(WebServerT &s, int code, const String &body)
{
    s.sendHeader("Cache-Control", "no-store");
    s.send(code, "text/html; charset=utf-8", body);
}

static void sendWifiPage(WebServerT &s, bool portal)
{
    String cur;
    if (portal)
        cur = cfg.ssid[0] ? esc(cfg.ssid) : String("не настроена");
    else
        cur = esc(WiFi.SSID());

    String p = pageStart("Wi-Fi");
    p += FPSTR(FORM_HTML);

    p.replace("{{CURLABEL}}", portal ? "Сохранённая сеть" : "Подключено к");
    p.replace("{{CUR}}", cur);
    p.replace("{{SSID}}", esc(cfg.ssid));
    p.replace("{{PH}}", cfg.ssid[0] ? "пусто — оставить прежний (для этой же сети)" : "");

    if (portal)
    {
        String f = F("<label>Google Script URL</label><input name=\"url\" maxlength=\"199\" value=\"");
        f += esc(cfg.scriptUrl);
        f += F("\">");
        p.replace("{{URLFIELD}}", f);
        p.replace("{{BACK}}", "");
    }
    else
    {
        p.replace("{{URLFIELD}}", "");
        p.replace("{{BACK}}", "<a class=\"btn sec\" href=\"/\">На главную</a>");
    }
    sendHtml(s, 200, p);
}

static void handleScan(WebServerT &s)
{
    if (s.hasArg("rescan") || netCount == 0) scanNetworks();

    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    for (int i = 0; i < netCount && i < MAX_SHOWN; i++)
    {
        JsonObject o = a.add<JsonObject>();
        o["s"] = nets[i].ssid;
        o["r"] = nets[i].rssi;
        o["o"] = nets[i].open;
    }
    String out;
    serializeJson(d, out);
    s.sendHeader("Cache-Control", "no-store");
    s.send(200, "application/json", out);
}

static void handleSave(WebServerT &s, bool portal)
{
    String ssid = s.arg("ssid");
    ssid.trim();
    String pass = s.arg("pass");

    if (ssid.length() == 0 || ssid.length() > SSID_MAX)
    {
        sendHtml(s, 400, msgPage("Ошибка", "Название сети должно быть от 1 до 32 символов.", true));
        return;
    }
    // пустой пароль для той же сети — оставляем прежний
    if (pass.length() == 0 && ssid == cfg.ssid) pass = cfg.pass;
    if (pass.length() > 0 && (pass.length() < 8 || pass.length() > PASS_MAX))
    {
        sendHtml(s, 400, msgPage("Ошибка", "Пароль WPA должен быть от 8 до 63 символов (для открытой сети оставьте поле пустым).", true));
        return;
    }

    if (portal)
    {
        // Портал: рабочие настройки меняются сразу; URL можно оставить как есть
        String url = s.arg("url");
        url.trim();
        if (url.length() == 0) url = cfg.scriptUrl;
        if (url.length() == 0)
        {
            sendHtml(s, 400, msgPage("Ошибка", "Укажите Google Script URL.", true));
            return;
        }
        if (url.length() >= URL_MAX)
        {
            sendHtml(s, 400, msgPage("Ошибка", "Слишком длинный URL.", true));
            return;
        }

        strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
        strlcpy(cfg.pass, pass.c_str(), sizeof(cfg.pass));
        cfg.pendSsid[0] = cfg.pendPass[0] = '\0';
        strlcpy(cfg.scriptUrl, url.c_str(), sizeof(cfg.scriptUrl));

        if (!configSave())
        {
            sendHtml(s, 500, msgPage("Ошибка", "Не удалось сохранить настройки.", true));
            return;
        }
        sendHtml(s, 200, msgPage("Сохранено", "Устройство подключается к сети «" + ssid + "». Эту страницу можно закрыть.", false));
        portalDone = true;
    }
    else
    {
        // Обычная страница: новая сеть сохраняется как «ожидающая». Если подключиться
        // не получится, после перезагрузки устройство вернётся к прежней сети.
        strlcpy(cfg.pendSsid, ssid.c_str(), sizeof(cfg.pendSsid));
        strlcpy(cfg.pendPass, pass.c_str(), sizeof(cfg.pendPass));

        if (!configSave())
        {
            cfg.pendSsid[0] = cfg.pendPass[0] = '\0';
            sendHtml(s, 500, msgPage("Ошибка", "Не удалось сохранить настройки.", true));
            return;
        }
        sendHtml(s, 200, msgPage("Сохранено",
                                 "Устройство перезагрузится и подключится к сети «" + ssid +
                                     "». Если не получится — вернётся к прежней сети. Страница устройства: http://" +
                                     String(MDNS_HOST) + ".local",
                                 false));
        restartAt = millis() + 2000;
        if (!restartAt) restartAt = 1;
    }
}

void wifiRegisterRoutes(WebServerT &srv)
{
    srv.on("/wifi", HTTP_GET, [&srv]() { sendWifiPage(srv, false); });
    srv.on("/wifi/scan", HTTP_GET, [&srv]() { handleScan(srv); });
    srv.on("/wifi/save", HTTP_POST, [&srv]() { handleSave(srv, false); });
}

void wifiLoop()
{
    if (restartAt && (int32_t)(millis() - restartAt) >= 0)
    {
        Serial.println("Restarting to apply new WiFi...");
        ESP.restart();
    }
}

// ---------------------------------------------------------------- портал

bool wifiPortal(uint32_t timeoutS, bool keepRetrying)
{
    Serial.println("Portal: preparing...");
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    scanNetworks(); // список сетей собираем до запуска точки доступа

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_NAME);
    delay(200);
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("Portal: AP \"%s\", page http://%s\n", AP_NAME, apIP.toString().c_str());

    DNSServer dns;
    dns.start(53, "*", apIP); // captive portal: любое имя ведёт на устройство

    WebServerT srv(80);
    String redirect = "http://" + apIP.toString() + "/";

    srv.on("/", HTTP_GET, [&]() { sendWifiPage(srv, true); });
    srv.on("/wifi", HTTP_GET, [&]() { sendWifiPage(srv, true); });
    srv.on("/wifi/scan", HTTP_GET, [&]() { handleScan(srv); });
    srv.on("/wifi/save", HTTP_POST, [&]() { handleSave(srv, true); });
    srv.onNotFound([&]() {
        srv.sendHeader("Location", redirect, true);
        srv.send(302, "text/plain", "");
    });
    srv.begin();

    // Параллельно пробуем подключиться к сохранённой сети (например, роутер ещё загружается)
    if (keepRetrying && cfg.ssid[0]) WiFi.begin(cfg.ssid, cfg.pass[0] ? cfg.pass : nullptr);

    portalDone = false;
    bool connected = false;
    uint32_t start = millis();
    uint32_t endAt = 0;

    while (true)
    {
        dns.processNextRequest();
        srv.handleClient();

        if (!endAt)
        {
            if (portalDone)
                endAt = millis() + 1500; // даём доотправить ответ браузеру
            else if (keepRetrying && WiFi.status() == WL_CONNECTED)
            {
                connected = true;
                break;
            }
            else if (millis() - start > timeoutS * 1000UL)
                break;
        }
        else if ((int32_t)(millis() - endAt) >= 0)
            break;

        delay(2);
    }

    srv.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    Serial.println(connected ? "Portal: closed, WiFi connected" : "Portal: closed");
    return connected;
}
