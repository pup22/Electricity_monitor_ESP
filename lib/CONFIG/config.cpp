#include "config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

Config cfg;

static const char *CONFIG_PATH = "/config.json";

ConfigFile configLoad()
{
    ConfigFile info{false, false};
    memset(&cfg, 0, sizeof(cfg));

    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) return info;
    info.exists = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return info;

    info.hasSsid = !doc["ssid"].isNull();
    strlcpy(cfg.ssid, doc["ssid"] | "", sizeof(cfg.ssid));
    strlcpy(cfg.pass, doc["pass"] | "", sizeof(cfg.pass));
    strlcpy(cfg.pendSsid, doc["new_ssid"] | "", sizeof(cfg.pendSsid));
    strlcpy(cfg.pendPass, doc["new_pass"] | "", sizeof(cfg.pendPass));
    strlcpy(cfg.scriptUrl, doc["script_url"] | "", sizeof(cfg.scriptUrl));

    Serial.printf("Config loaded: ssid=\"%s\", url %s\n", cfg.ssid, cfg.scriptUrl[0] ? "set" : "empty");
    return info;
}

bool configSave()
{
    JsonDocument doc;
    doc["ssid"] = cfg.ssid; // ключ пишется всегда — по нему отличаем новый формат от старого
    doc["pass"] = cfg.pass;
    doc["script_url"] = cfg.scriptUrl;
    if (cfg.pendSsid[0])
    {
        doc["new_ssid"] = cfg.pendSsid;
        doc["new_pass"] = cfg.pendPass;
    }

    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f)
    {
        Serial.println("Error: cannot open config for writing");
        return false;
    }
    serializeJson(doc, f);
    f.close();
    Serial.println("Config saved");
    return true;
}

void configWipe()
{
    LittleFS.remove(CONFIG_PATH);
    memset(&cfg, 0, sizeof(cfg));
}
