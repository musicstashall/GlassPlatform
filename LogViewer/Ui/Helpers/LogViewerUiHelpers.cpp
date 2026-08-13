#include "SharedPch.h"

#include "framework.h"
#include "Ui/Helpers/LogViewerUiHelpers.h"
#include <GlassPlatformTask.h>
#include <commctrl.h>

#pragma comment(lib, "Comctl32.lib")

bool ListViewIsScrolledToBottom(HWND hList, int itemCount) noexcept
{
    if (!hList || itemCount <= 0)
        return true;
    const int top = ListView_GetTopIndex(hList);
    int page = ListView_GetCountPerPage(hList);
    if (page < 1)
        page = 1;
    return (top + page >= itemCount);
}

void FormatFilterToolbarLabel(
    wchar_t* buf,
    size_t bufChars,
    bool restrictActive,
    size_t selectedCount,
    const wchar_t* allLabel,
    const wchar_t* form1,
    const wchar_t* form2to4,
    const wchar_t* form5plus) noexcept
{
    if (!restrictActive)
    {
        wcscpy_s(buf, bufChars, allLabel);
        return;
    }
    if (selectedCount == 0)
    {
        swprintf_s(buf, bufChars, L"0 %s", form5plus);
        return;
    }
    const wchar_t* w = form5plus;
    const unsigned long n = static_cast<unsigned long>(selectedCount);
    if (n % 10UL == 1UL && n % 100UL != 11UL)
        w = form1;
    else if (n % 10UL >= 2UL && n % 10UL <= 4UL && (n % 100UL < 10UL || n % 100UL >= 20UL))
        w = form2to4;
    swprintf_s(buf, bufChars, L"%zu %s", selectedCount, w);
}

bool IsGlassPlatformRunning()
{
    return GlassShared::GlassPlatformTask::IsGlassPlatformRunning();
}
