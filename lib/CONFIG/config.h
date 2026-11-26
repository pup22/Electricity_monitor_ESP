#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------- настройки сети

constexpr const char *AP_NAME = "Power-Monitor-Setup"; // точка доступа портала настройки
constexpr const char *MDNS_HOST = "power-monitor";     // http://power-monitor.local

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30UL * 1000; // ожидание подключения к сохранённой сети
constexpr uint32_t WIFI_PENDING_TIMEOUT_MS = 20UL * 1000; // ожидание подключения к новой сети (до отката)
constexpr uint32_t PORTAL_TIMEOUT_S = 180;                // сколько держать портал открытым

// ---------------------------------------------------------------- хранимые настройки

constexpr size_t SSID_MAX = 32;
constexpr size_t PASS_MAX = 63;
constexpr size_t URL_MAX = 200;

struct Config
{
    char ssid[SSID_MAX + 1];        // рабочая сеть
    char pass[PASS_MAX + 1];
    char pendSsid[SSID_MAX + 1];    // новая сеть, ожидающая проверки (если не подключится — откат на рабочую)
    char pendPass[PASS_MAX + 1];
    char scriptUrl[URL_MAX];        // URL веб-приложения Google
};

extern Config cfg;

struct ConfigFile
{
    bool exists;  // файл /config.json найден
    bool hasSsid; // в нём есть ключ "ssid" (файл написан этой версией прошивки)
};

ConfigFile configLoad(); // читает файл в cfg (LittleFS должна быть смонтирована)
bool configSave();       // записывает cfg в файл
void configWipe();       // удаляет файл и обнуляет cfg
