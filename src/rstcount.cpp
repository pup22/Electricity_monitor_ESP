#include <Arduino.h>
#include "rstcount.h"

static const uint32_t WAIT_TIME = 1000; // окно ожидания следующего клика, мс
static const uint8_t RST_MAX = 3;       // сколько кликов Reset подряд сбрасывают настройки

#if defined(ESP32)
#include <Preferences.h>

static uint8_t loadCount()
{
  Preferences p;
  p.begin("drd", true);
  uint8_t v = p.getUChar("count", 0);
  p.end();
  return v;
}

static void storeCount(uint8_t value)
{
  Preferences p;
  p.begin("drd", false);
  p.putUChar("count", value);
  p.end();
}

#elif defined(ESP8266)
static const uint16_t RTC_SIGN = 0xABCD;
static const uint32_t RTC_SLOT = 0; // блок RTC-памяти (4 байта)

static uint8_t loadCount()
{
  uint32_t d = 0;
  ESP.rtcUserMemoryRead(RTC_SLOT, &d, sizeof(d));
  if ((uint16_t)(d >> 16) == RTC_SIGN &&
      (uint8_t)~((d >> 8) & 0xFF) == (uint8_t)(d & 0xFF))
    return d & 0xFF;
  return 0;
}

static void storeCount(uint8_t value)
{
  uint32_t d = ((uint32_t)RTC_SIGN << 16) | ((uint32_t)(uint8_t)~value << 8) | value;
  ESP.rtcUserMemoryWrite(RTC_SLOT, &d, sizeof(d));
}
#endif

bool resetClick()
{
  uint8_t count = loadCount() + 1;

  if (count >= RST_MAX)
  {
    storeCount(0);
    return true;
  }

  storeCount(count);
  delay(WAIT_TIME); // если за это время Reset не нажали — счётчик обнуляется
  storeCount(0);
  return false;
}
