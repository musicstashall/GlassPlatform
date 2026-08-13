#pragma once

// ============================================================================
// Единый предкомпилированный заголовок для всех проектов Glass
// ============================================================================

//-----------------------------------------------------------------------------
// 1. СНАЧАЛА: отключаем надоедливые предупреждения
//-----------------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable : 26812)    // Unchecked iterators
#pragma warning(disable : 4458)      // declaration hides class member
#pragma warning(disable : 4820)      // bytes padding added after data member
#pragma warning(disable : 4514)      // unreferenced inline function removed
#pragma warning(disable : 4668)      // 'symbol' is not defined as a preprocessor macro
#pragma warning(disable : 4710)      // function not inlined
#pragma warning(disable : 5045)      // Compiler will insert Spectre mitigation
#pragma warning(disable : 5264)      // 'variable' has internal linkage but is not defined

//-----------------------------------------------------------------------------
// 2. СИСТЕМНЫЕ ЗАГОЛОВКИ Windows
//-----------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <comdef.h>
#include <comip.h>
#include <comutil.h>
#include <winternl.h>

//-----------------------------------------------------------------------------
// 3. STL/CRT - ВСЕ ЧЕРЕЗ cpprt.h (включая chrono)
//-----------------------------------------------------------------------------
#include "cpprt.h"  // Ваш существующий файл со всеми STL включениями

//-----------------------------------------------------------------------------
// 4. ATL (если используется)
//-----------------------------------------------------------------------------
#include <atlbase.h>
#include <atlcom.h>
#include <atlwin.h>
#include <atltypes.h>

//-----------------------------------------------------------------------------
// 5. СТОРОННИЕ БИБЛИОТЕКИ
//-----------------------------------------------------------------------------
#include "detours.h"
#include "dx.h"

//-----------------------------------------------------------------------------
// 6. WIL - ДОЛЖЕН БЫТЬ ПОСЛЕ STL, НО ДО НАШЕГО КОДА
//-----------------------------------------------------------------------------
#include "wil.h"

//-----------------------------------------------------------------------------
// 7. НАШИ ОБЩИЕ ЗАГОЛОВКИ (из GlassShared)
//-----------------------------------------------------------------------------
#include <DebugLog.h>
#include "DwmThumbnailAPI.h"    // если это общий заголовок

#pragma comment(lib, "version.lib")
//-----------------------------------------------------------------------------
// 8. ГЛОБАЛЬНЫЕ НАСТРОЙКИ ПРОЕКТА
//-----------------------------------------------------------------------------
// Общие макросы для всех проектов
#ifndef GLASS_ENABLE_LOGGING
#define GLASS_ENABLE_LOGGING
#endif

// Настройки экспорта/импорта для DLL
#ifdef GLASS_SHARED_EXPORTS
#define GLASS_SHARED_API __declspec(dllexport)
#else
#define GLASS_SHARED_API __declspec(dllimport)
#endif

// Удобные using namespace для часто используемых пространств
using namespace std::chrono_literals;

//-----------------------------------------------------------------------------
// 9. ОБЩИЕ ВСПОМОГАТЕЛЬНЫЕ МАКРОСЫ
//-----------------------------------------------------------------------------
// SAFE_RELEASE для COM-объектов
template <typename T>
inline void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

// SAFE_DELETE для обычных указателей
template <typename T>
inline void SafeDelete(T*& ptr) {
    delete ptr;
    ptr = nullptr;
}

// SAFE_DELETE_ARRAY для массивов
template <typename T>
inline void SafeDeleteArray(T*& ptr) {
    delete[] ptr;
    ptr = nullptr;
}

//-----------------------------------------------------------------------------
// Восстанавливаем предупреждения
//-----------------------------------------------------------------------------
#pragma warning(pop)