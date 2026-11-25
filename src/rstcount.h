#pragma once

// Возвращает true, если Reset нажат RST_MAX раз подряд
// (нужно сбросить настройки Wi-Fi и URL).
// Вызывать как можно раньше в setup().
bool resetClick();
