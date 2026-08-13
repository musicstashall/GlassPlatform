#include "WndPch.h"
#include "MenuContext.h"

namespace WndCompat::Menu
{
    thread_local MenuContext g_MenuContext{};

    void ResetContext()
    {
        g_MenuContext = MenuContext{};
    }

    // Список модулей, в которых разрешены хуки меню.
    // Аналог g_hookModuleList из BWM/MenuHooks.cpp:
    // хук меню применяется только если вызывающий модуль есть в этом списке.
    // Пустой список = хуки применяются везде (поведение по умолчанию).
    // Для включения фильтрации добавить имена exe/dll в нижнем регистре.
    const wchar_t* g_hookModuleList[] =
    {
        // Примеры (раскомментировать по необходимости):
        // L"explorer.exe",
        // L"notepad.exe",
        nullptr  // признак конца списка
    };

    // Проверяет, входит ли модуль hCaller в список g_hookModuleList.
    // Если список пуст (только nullptr) — возвращает true (всё разрешено).
    bool IsModuleHookAllowed(HMODULE hCaller) noexcept
    {
        if (!g_hookModuleList[0])
            return true; // список пуст — разрешено всем

        wchar_t callerPath[MAX_PATH]{};
        if (!GetModuleFileNameW(hCaller, callerPath, MAX_PATH))
            return false;

        // Берём только имя файла (без пути)
        const wchar_t* name = wcsrchr(callerPath, L'\\');
        name = name ? name + 1 : callerPath;

        for (const wchar_t** p = g_hookModuleList; *p; ++p)
        {
            if (_wcsicmp(name, *p) == 0)
                return true;
        }
        return false;
    }
}
