#pragma once
#include <Arduino.h>

#if defined(ESP32)
#include <WebServer.h>
using WebServerT = WebServer;
#elif defined(ESP8266)
#include <ESP8266WebServer.h>
using WebServerT = ESP8266WebServer;
#endif

// Сообщение о результате последней смены сети (показывается на странице устройства)
extern String wifiNote;

// Одноразовый перенос Wi-Fi из памяти SDK (прошивки с WiFiManager) в config.json.
// Возвращает true, если подключились и настройки перенесены.
bool wifiMigrateLegacy();

// Подключение при старте. Если есть новая сеть (pendSsid) — сначала пробует её,
// при неудаче возвращается к рабочей. true — подключено.
bool wifiConnectBoot();

// Портал настройки: точка доступа AP_NAME + страница выбора сети.
// keepRetrying — параллельно пытаться подключиться к сохранённой сети и выйти, как только получится.
// Возвращает true, если во время портала подключились к сохранённой сети.
bool wifiPortal(uint32_t timeoutS, bool keepRetrying);

// Забыть Wi-Fi, сохранённый в памяти SDK (полный сброс)
void wifiForget();

// Страницы /wifi, /wifi/scan, /wifi/save для обычного веб-сервера устройства
void wifiRegisterRoutes(WebServerT &srv);

// Вызывать из loop(): выполняет отложенную перезагрузку после смены сети
void wifiLoop();
