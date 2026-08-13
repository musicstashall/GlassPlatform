#include "WndPch.h"
#include "DateTimePickerSubclass.h"
#include "WndCompat.h"
#include <CommCtrl.h>
#include <GlassMessages.h>
#include <GlassControlKind.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    constexpr UINT_PTR kDateTimePickerSubclassId = 1;
    constexpr DWORD kDetachTimeoutMs = 1000;
    constexpr int kDetachRetryCount = 2;

    std::vector<HWND> g_windows;
    std::mutex        g_mutex;

    void ApplyDefaultGuiFontIfNeeded(HWND hWnd) noexcept
    {
        const HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
        LOGFONTW lf{};
        if (font && GetObjectW(font, sizeof(lf), &lf) > 0
            && lf.lfFaceName[0] != L'\0'
            && _wcsicmp(lf.lfFaceName, L"System") != 0)
        {
            return;
        }
        SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }

    LRESULT CALLBACK DateTimePickerSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        if (uMsg == WndCompat::WM_GLASS_DETACH_DATETIMEPICKER)
        {
            WndCompat::DateTimePicker_Detach(hWnd);
            DebugLog(L"[WndCompat][DateTimePicker] WM_GLASS_DETACH_DATETIMEPICKER: снят hwnd=0x%p", hWnd);
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case WM_NCDESTROY:
            WndCompat::DateTimePicker_Detach(hWnd);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void DateTimePicker_Attach(HWND hWnd, GlassShared::GlassControlKind kind)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == kind)
            return;

        if (!SetWindowSubclass(hWnd, DateTimePickerSubclassProc, kDateTimePickerSubclassId, 0))
        {
            DebugLog(DBG_WARNING, L"[WndCompat][DateTimePicker] DateTimePicker_Attach: FAILED subclass 0x%p", hWnd);
            return;
        }

        ApplyDefaultGuiFontIfNeeded(hWnd);
        GlassMarkWindow(hWnd, kind);

        {
            std::lock_guard lock(g_mutex);
            g_windows.push_back(hWnd);
        }

        DebugLog(L"[WndCompat][DateTimePicker] DateTimePicker_Attach: hwnd=0x%p kind=%u",
            hWnd, static_cast<unsigned>(kind));
    }

    void DateTimePicker_Detach(HWND hWnd)
    {
        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, DateTimePickerSubclassProc, kDateTimePickerSubclassId);

        GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_mutex);
            const auto it = std::find(g_windows.begin(), g_windows.end(), hWnd);
            if (it != g_windows.end())
                g_windows.erase(it);
        }
    }

    void DateTimePicker_DetachAll()
    {
        std::vector<HWND> copy;
        {
            std::lock_guard lock(g_mutex);
            copy = g_windows;
            g_windows.clear();
        }

        const DWORD curTid = GetCurrentThreadId();
        for (HWND hWnd : copy)
        {
            if (!IsWindow(hWnd))
                continue;

            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
            if (wndTid == 0 || wndTid == curTid)
            {
                DateTimePicker_Detach(hWnd);
                DebugLog(L"[WndCompat][DateTimePicker] DateTimePicker_DetachAll: detached hwnd=0x%p", hWnd);
                continue;
            }

            bool detached = false;
            for (int attempt = 1; attempt <= kDetachRetryCount; ++attempt)
            {
                DWORD_PTR sendResult = 0;
                if (SendMessageTimeoutW(
                        hWnd,
                        WndCompat::WM_GLASS_DETACH_DATETIMEPICKER,
                        0,
                        0,
                        SMTO_ABORTIFHUNG,
                        kDetachTimeoutMs,
                        &sendResult))
                {
                    detached = true;
                    DebugLog(
                        L"[WndCompat][DateTimePicker] DateTimePicker_DetachAll: cross-thread detached hwnd=0x%p attempt=%d",
                        hWnd,
                        attempt);
                    break;
                }
            }

            if (!detached)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[WndCompat][DateTimePicker] DateTimePicker_DetachAll: cross-thread timeout hwnd=0x%p",
                    hWnd);
            }
        }
    }
}
