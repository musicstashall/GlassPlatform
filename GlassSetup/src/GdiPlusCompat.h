#pragma once

// WIN32_LEAN_AND_MEAN excludes the COM types used by Windows SDK GDI+ headers.
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <algorithm>

// With NOMINMAX, some Windows SDK versions still call min/max unqualified
// from inside Gdiplus. Keep the C++ functions in that namespace instead of
// defining Windows min/max macros that would break std::min/std::max elsewhere.
namespace Gdiplus {
using std::min;
using std::max;
}

#include <gdiplus.h>
