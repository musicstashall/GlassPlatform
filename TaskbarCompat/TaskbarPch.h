#pragma once
#undef WIN32_LEAN_AND_MEAN
#undef NOUSER
#undef NOMSG
#undef NOWINMESSAGES

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#ifndef _WIN32_IE
#define _WIN32_IE 0x0602
#endif

#include <windows.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <roapi.h>
#include <winstring.h>
#include <objidl.h>
#include <xamlOM.h>

#include <cstdint>
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>

#include <DebugLog.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "runtimeobject.lib")
