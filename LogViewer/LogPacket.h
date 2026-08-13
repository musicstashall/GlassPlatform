#pragma once
#include <cstdint>

struct LogPacket
{
    uint32_t level;
    uint32_t processId;   // PID клиентского процесса
    uint32_t threadId;    // TID клиентского потока
    uint32_t length;      // длина text в wchar_t включая нулевой терминатор
    wchar_t  text[];
};

#define DBG_NORMAL      0
#define DBG_IMPORTANT   1
#define DBG_WARNING     2
#define DBG_ERROR       3
#define DBG_CRITICAL    4

// Служебный уровень: процесс отключился от лог-пайпа (DLL выгружена / процесс завершён).
// Пакет с этим уровнем НЕ попадает в список — используется только для обновления счётчика.
#define LOG_LEVEL_DISCONNECT 0xFFFFFFFFu

#ifndef FR_DOWN
#define FR_DOWN 0x00000001
#endif
