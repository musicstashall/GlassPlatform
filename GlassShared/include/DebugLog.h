#pragma once

#include <windows.h>
#include <string>
#include <cstdarg>

// -------------------------------
// Уровни логов (общие для всех)
// -------------------------------
#define DBG_NORMAL      0
#define DBG_IMPORTANT   1
#define DBG_WARNING     2
#define DBG_ERROR       3
#define DBG_CRITICAL    4

// Формат строки (для LogViewer: модуль и подсистема — отдельные фильтры):
//   L"[Модуль][Подсистема] сообщение..."
// Пример: L"[WndCompat][Root] attach hwnd=0x%p"

// ============================================================================
//  RELEASE MODE — логирование отключено
// ============================================================================
#if !defined(_DEBUG) || !defined(GLASS_ENABLE_LOGGING)

inline void DebugLog(int, const wchar_t*, ...) {}
inline void DebugLog(const wchar_t*, ...) {}
inline void DebugLog(const std::wstring&) {}

#else // DEBUG + LOGGING ENABLED

// ============================================================================
//  SERVER MODE (GlassPlatform.exe)
//  Определяется флагом GLASS_LOG_SERVER
// ============================================================================
#if defined(GLASS_LOG_SERVER)

#include "GlassLogServer.h"

inline void DebugLogImpl(int level, const wchar_t* fmt, va_list args)
{
    if (!fmt)
        return;

    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, fmt, args);

    // Дублируем в OutputDebugString
    ::OutputDebugStringW(buffer);
    ::OutputDebugStringW(L"\n");

    // Отправляем в LogViewer.exe через GlassLogServer
    Shared::Log::GlassLogServer::Instance().WriteLine(
        static_cast<std::uint32_t>(level),
        ::GetCurrentProcessId(),
        ::GetCurrentThreadId(),
        buffer
    );
}

// ============================================================================
//  CLIENT MODE (GlassCompat.dll)
// ============================================================================
#else

#include "GlassLogClient.h"

inline void DebugLogImpl(int level, const wchar_t* fmt, va_list args)
{
    if (!fmt)
        return;

    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, fmt, args);

    // Дублируем в OutputDebugString
    ::OutputDebugStringW(buffer);
    ::OutputDebugStringW(L"\n");

    if (Shared::Log::GlassLogClient::IsStopped())
        return;

    // Отправляем в GlassPlatform через старый протокол
    Shared::Log::SendLog(buffer, level);
}

#endif // GLASS_LOG_SERVER

// ============================================================================
//  PUBLIC API
// ============================================================================

inline void DebugLog(int level, const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    DebugLogImpl(level, fmt, args);
    va_end(args);
}

inline void DebugLog(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    DebugLogImpl(DBG_NORMAL, fmt, args);
    va_end(args);
}

inline void DebugLog(const std::wstring& text)
{
#if defined(GLASS_LOG_SERVER)
    Shared::Log::GlassLogServer::Instance().WriteLine(
        DBG_NORMAL,
        ::GetCurrentProcessId(),
        ::GetCurrentThreadId(),
        text.c_str()
    );
#else
    if (!Shared::Log::GlassLogClient::IsStopped())
        Shared::Log::SendLog(text, DBG_NORMAL);
#endif

    ::OutputDebugStringW(text.c_str());
    ::OutputDebugStringW(L"\n");
}

#endif // DEBUG + LOGGING
