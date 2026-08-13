#pragma once
#undef WIN32_LEAN_AND_MEAN
#undef NOUSER
#undef NOCOMMCTRL
#undef NOMSG
#undef NOWINMESSAGES

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10 и выше (для IDCompositionVisual3, GaussianBlurEffect)
#endif

#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif


#include <windows.h>
// WinUser: макрос GetCurrentTime конфликтует с методами в заголовках WinRT XAML.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <roapi.h>
#include <winstring.h>
#include <objidl.h>
#include <xamlOM.h>
#include <CommCtrl.h>
#include <dcomp.h>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <functional>
#include <windowsx.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <DebugLog.h>
#include <d3d11.h>

// WIL для RAII-обёрток COM
#include <wil/result.h>
#include <wil/com.h>
#include <wil/resource.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "runtimeobject.lib")
