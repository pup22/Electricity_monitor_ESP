// Code.gs — мониторинг электропитания (Google Apps Script, один файл)
//
// Лист "Log": строка 1 — заголовки, строка 2 — текущая (последняя) сессия,
// ниже — история. Колонки: Включено | Выключено (последний сигнал) |
// Длительность | Отсутствие (после этой сессии).
//
// Порядок установки: удалить старые файлы (_Global, Main, Setup, Telegram, Triger),
// вставить этот, заполнить TG_BOT_TOKEN, один раз запустить setupSheet().

// ============================================================================
//                                  НАСТРОЙКИ
// ============================================================================

// --- Таблица ---------------------------------------------------------------
const SHEET_NAME = "Log";
const TIMEZONE = "Europe/Kiev";
const DATE_FORMAT = "dd.MM.yyyy HH:mm";
const DIFF_FORMAT = '[h]" ч. "mm" мин. "';

// --- Оформление листа (setupSheet) ----------------------------------------
const HEADERS = ["Включено", "Выключено", "Длительность", "Отсутствие"];
const HEADER_BG = "#444444";
const HEADER_FONT_COLOR = "#FFFFFF";
const COLUMN_WIDTHS = [110, 110, 100, 100];

// --- Логика --------------------------------------------------------------
const HEARTBEAT_TIMEOUT_MIN = 4;          // нет сигнала дольше N минут => свет отключён
const TRIGGER_EVERY_MIN = 5;              // как часто триггер проверяет тайм-аут (1, 5, 10, 15, 30)
const RESTORE_NOTIFY_MIN_ABSENCE_MS = 60 * 1000; // не сообщать о «включении», если перерыв короче
const LOCK_WAIT_POST_MS = 15000;          // ожидание блокировки в doPost
const LOCK_WAIT_TRIGGER_MS = 5000;        // ожидание блокировки в триггере
const MIN_VALID_EPOCH = 1700000000;       // boot_time раньше этого — мусор
const MAX_FUTURE_SKEW_SEC = 300;          // boot_time «из будущего» больше чем на столько — мусор

// --- Telegram --------------------------------------------------------------
// Токен лучше не хранить в коде: можно задать в Свойствах скрипта
// (Настройки проекта -> Свойства скрипта -> TG_BOT_TOKEN), тогда он приоритетнее.
const TG_BOT_TOKEN = "<ВАШ_ТОКЕН_БОТА>";
// Получатели уведомлений. Каждый должен хотя бы раз написать боту /start,
// иначе Telegram не даст боту писать ему. ID группы — отрицательное число.
const TG_CHAT_IDS = [
  "111111111", // Имя 1
  "222222222", // Имя 2
  "333333333", // Имя 3
];
const TG_PARSE_MODE = "HTML";
const SEND_TEST_MESSAGE_ON_SETUP = true;

// --- Тексты уведомлений ----------------------------------------------------
const MSG = {
  test: () => "Тестовое сообщение из меню Google Таблиц",
  powerOff: (lastSignal) =>
    `🔴 <b>Электроэнергия отключена!</b>\nПоследний сигнал: ${lastSignal}`,
  powerOn: (bootTime, absence) =>
    `🟢 <b>Электроэнергия восстановлена!</b>\nВремя: ${bootTime}\nОтсутствовал: <b>${absence}</b>`,
  // Сигнал вернулся, а питание не пропадало (то же время включения) —
  // значит, пропал интернет/Wi-Fi, а не свет.
  signalBack: (now, gap) =>
    `🟡 <b>Сигнал устройства восстановлен</b> (питание не отключалось — вероятно, пропал интернет/Wi-Fi)\nВремя: ${now}\nНе было сигнала: <b>${gap}</b>`,
  hours: "ч.",
  minutes: "мин.",
};

// --- Служебные ключи -------------------------------------------------------
const PROP_OUTAGE_NOTIFIED = "outageNotified"; // флаг: «отключено» уже отправлено

const DAY_MS = 24 * 60 * 60 * 1000;

// ============================================================================
//                              ПРИЁМ HEARTBEAT
// ============================================================================

function doPost(e) {
  const lock = LockService.getScriptLock();
  if (!lock.tryLock(LOCK_WAIT_POST_MS)) return textOut_("Error: Lock timeout");

  let notification = null;
  try {
    notification = processHeartbeat_(e);
  } catch (err) {
    console.error("Critical error: " + err);
    return textOut_("Error: " + err);
  } finally {
    lock.releaseLock();
  }

  // Telegram отправляем уже без блокировки, чтобы не задерживать другие вызовы
  if (notification) sendTelegramMessage(notification);
  return textOut_("Success");
}

/** Обновляет таблицу. Возвращает текст уведомления или null. */
function processHeartbeat_(e) {
  const sheet = getSheet_();
  const bootSec = parseBootTime_(e);

  const bootDate = new Date(bootSec * 1000);
  const now = new Date();
  const fmtBoot = formatDate_(bootDate);
  const fmtNow = formatDate_(now);

  const props = PropertiesService.getScriptProperties();
  const outageWasNotified = props.getProperty(PROP_OUTAGE_NOTIFIED) === "1";

  const lastRow = sheet.getLastRow();
  const hasData = lastRow > 1;
  const firstBoot = hasData ? sheet.getRange(2, 1).getDisplayValue().trim() : "";
  const prevOffDate = hasData ? parseSheetDate_(sheet.getRange(2, 2).getDisplayValue()) : null;

  let notification = null;

  if (hasData && firstBoot === fmtBoot) {
    // --- СЦЕНАРИЙ 1: та же сессия — обновляем строку 2 ---
    sheet.getRange(2, 2, 1, 3).setValues([[fmtNow, (now - bootDate) / DAY_MS, 0]]);

    if (outageWasNotified) {
      const gapMs = prevOffDate ? Math.max(0, now - prevOffDate) : 0;
      notification = MSG.signalBack(fmtNow, formatDuration_(gapMs));
    }
  } else {
    // --- СЦЕНАРИЙ 2: новая сессия — вставляем строку сверху ---
    let absenceMs = 0;
    if (prevOffDate) absenceMs = Math.max(0, bootDate - prevOffDate);

    sheet.insertRowBefore(2);
    applyRowFormat_(sheet, 2); // новая строка могла унаследовать оформление заголовка
    sheet.getRange(2, 1, 1, 4).setValues([[fmtBoot, fmtNow, (now - bootDate) / DAY_MS, 0]]);

    if (hasData && prevOffDate) {
      // предыдущая сессия теперь в строке 3: записываем итоговое отсутствие
      sheet.getRange(3, 4).setValue(absenceMs / DAY_MS);
    }

    if (absenceMs > RESTORE_NOTIFY_MIN_ABSENCE_MS) {
      notification = MSG.powerOn(fmtBoot, formatDuration_(absenceMs));
    }
  }

  if (outageWasNotified) props.deleteProperty(PROP_OUTAGE_NOTIFIED);
  return notification;
}

function parseBootTime_(e) {
  let data;
  try {
    data = JSON.parse(e.postData.contents);
  } catch (err) {
    throw "Invalid JSON";
  }
  const sec = Number(data && data.boot_time);
  const maxSec = Date.now() / 1000 + MAX_FUTURE_SKEW_SEC;
  if (!isFinite(sec) || sec < MIN_VALID_EPOCH || sec > maxSec) {
    throw "Invalid boot_time";
  }
  return sec;
}

// ============================================================================
//                       ТРИГГЕР: ПРОВЕРКА «ЗАВИСШЕГО» СИГНАЛА
// ============================================================================

function checkHeartbeatTimeout() {
  const lock = LockService.getScriptLock();
  if (!lock.tryLock(LOCK_WAIT_TRIGGER_MS)) return;

  let notification = null;
  try {
    const sheet = getSheet_();
    if (sheet.getLastRow() < 2) return;

    const lastOffStr = sheet.getRange(2, 2).getDisplayValue().trim();
    const lastOffDate = parseSheetDate_(lastOffStr);
    if (!lastOffDate) return;

    const diffMs = Date.now() - lastOffDate.getTime();
    if (diffMs / 60000 <= HEARTBEAT_TIMEOUT_MIN) return;

    const props = PropertiesService.getScriptProperties();
    if (props.getProperty(PROP_OUTAGE_NOTIFIED) !== "1") {
      notification = MSG.powerOff(lastOffStr);
    }

    // Текущее время отсутствия — в D2 (после включения оно перейдёт в D3)
    sheet.getRange(2, 4).setValue(diffMs / DAY_MS);
    console.log("Тайм-аут зафиксирован, D2 обновлено.");
  } catch (err) {
    console.error("Ошибка в checkHeartbeatTimeout: " + err);
  } finally {
    lock.releaseLock();
  }

  // Флаг ставим только если сообщение реально ушло — иначе следующий запуск повторит
  if (notification && sendTelegramMessage(notification)) {
    PropertiesService.getScriptProperties().setProperty(PROP_OUTAGE_NOTIFIED, "1");
    console.log("Отправлено уведомление об отключении.");
  }
}

/** Создаёт (пересоздаёт) периодический триггер. */
function createTrigger() {
  ScriptApp.getProjectTriggers().forEach((t) => {
    if (t.getHandlerFunction() === "checkHeartbeatTimeout") ScriptApp.deleteTrigger(t);
  });

  ScriptApp.newTrigger("checkHeartbeatTimeout")
    .timeBased()
    .everyMinutes(TRIGGER_EVERY_MIN)
    .create();

  console.log("Триггер успешно создан.");
}

// ============================================================================
//                              ОФОРМЛЕНИЕ ЛИСТА
// ============================================================================

/** Запускать вручную один раз: оформляет лист, создаёт триггер. */
function setupSheet() {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const sheet = ss.getSheetByName(SHEET_NAME) || ss.insertSheet(SHEET_NAME);

  sheet.getRange(1, 1, 1, HEADERS.length).setValues([HEADERS])
    .setFontWeight("bold")
    .setFontColor(HEADER_FONT_COLOR)
    .setBackground(HEADER_BG)
    .setHorizontalAlignment("center")
    .setVerticalAlignment("middle");

  sheet.setFrozenRows(1);
  COLUMN_WIDTHS.forEach((w, i) => sheet.setColumnWidth(i + 1, w));

  sheet.getRange("A2:B").setNumberFormat(DATE_FORMAT);
  sheet.getRange("C2:D").setNumberFormat(DIFF_FORMAT);
  sheet.getRange("A2:D").setHorizontalAlignment("right");

  console.log("Лист успешно оформлен!");

  if (SEND_TEST_MESSAGE_ON_SETUP) sendTelegramMessage(MSG.test());
  createTrigger();
}

/** Обычное оформление строки данных (сбрасывает стиль заголовка). */
function applyRowFormat_(sheet, row) {
  sheet.getRange(row, 1, 1, 4)
    .setFontWeight("normal")
    .setFontColor("#000000")
    .setBackground(null)
    .setHorizontalAlignment("right")
    .setNumberFormats([[DATE_FORMAT, DATE_FORMAT, DIFF_FORMAT, DIFF_FORMAT]]);
}

// ============================================================================
//                                  TELEGRAM
// ============================================================================

/**
 * Отправляет сообщение всем получателям из TG_CHAT_IDS.
 * Сбой у одного получателя (например, он заблокировал бота) не мешает остальным.
 * Возвращает true, если сообщение дошло хотя бы до одного.
 */
function sendTelegramMessage(text) {
  const token =
    PropertiesService.getScriptProperties().getProperty("TG_BOT_TOKEN") || TG_BOT_TOKEN;
  const url = `https://api.telegram.org/bot${token}/sendMessage`;

  let delivered = 0;
  TG_CHAT_IDS.forEach((chatId) => {
    try {
      const response = UrlFetchApp.fetch(url, {
        method: "post",
        contentType: "application/json",
        payload: JSON.stringify({
          chat_id: chatId,
          text: text,
          parse_mode: TG_PARSE_MODE,
        }),
        muteHttpExceptions: true,
      });
      const code = response.getResponseCode();
      console.log(`Telegram [${chatId}] (${code}): ${response.getContentText()}`);
      if (code === 200) delivered++;
    } catch (e) {
      console.error(`Ошибка при отправке в Telegram [${chatId}]: ${e}`);
    }
  });

  return delivered > 0;
}

// ============================================================================
//                                  УТИЛИТЫ
// ============================================================================

function getSheet_() {
  const sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
  if (!sheet) throw `Лист "${SHEET_NAME}" не найден — запустите setupSheet()`;
  return sheet;
}

function textOut_(s) {
  return ContentService.createTextOutput(s);
}

function formatDate_(date) {
  return Utilities.formatDate(date, TIMEZONE, DATE_FORMAT);
}

/** Разбирает дату из ячейки; при ошибке возвращает null. */
function parseSheetDate_(str) {
  try {
    const s = String(str).trim();
    return s ? Utilities.parseDate(s, TIMEZONE, DATE_FORMAT) : null;
  } catch (e) {
    return null;
  }
}

/** Миллисекунды -> "N ч. M мин." / "M мин." */
function formatDuration_(ms) {
  if (ms < 0) ms = 0;
  const totalMinutes = Math.floor(ms / 60000);
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  return hours > 0
    ? `${hours} ${MSG.hours} ${minutes} ${MSG.minutes}`
    : `${minutes} ${MSG.minutes}`;
}