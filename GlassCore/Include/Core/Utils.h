#pragma once
#include <string>
#include <windows.h>

namespace GlassCore::Utils
{
    std::wstring ToWString(const std::string& s);
    std::string ToString(const std::wstring& ws);

    HMODULE GetModuleByName(const std::wstring& name);
    bool GetModuleVersion(HMODULE module, DWORD& major, DWORD& minor);

    // Добавленные функции из BWM

    // Создание рабочего окна (аналог SHCreateWorkerWindowW)
    HWND SHCreateWorkerWindowW(
        _In_opt_ WNDPROC wndProc,
        _In_opt_ HWND hwndParent,
        _In_ DWORD dwExStyle,
        _In_ DWORD dwStyle,
        _In_opt_ HMENU hMenu,
        _In_opt_ void* lpParam
    );

    // Скрыть окно от захвата (SetWindowDisplayAffinity + возможно WS_EX_LAYERED)
    void CloakWindow(HWND hWnd, BOOL cloak);

    // Получить строку версии модуля (например, "10.0.19041.1")
    std::wstring GetVersion(HMODULE module);

    // Создать полный путь к файлу во временной папке с заданным именем
    std::wstring MakeCurrentFolderFileStr(const std::wstring& fileName);

    // Инициализация COM для многопоточной модели (MTA)
    struct RoInit
    {
        RoInit();
        ~RoInit();
        HRESULT hr;
    };

    // Шаблон для приведения указателя на функцию-член к void*
    template<typename T, typename U>
    inline T union_cast(U u)
    {
        static_assert(sizeof(U) == sizeof(T), "sizes must match");
        T t;
        memcpy(&t, &u, sizeof(u));
        return t;
    }
}