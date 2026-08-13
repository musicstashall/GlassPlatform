#pragma once
#include <string>
#include <unordered_map>
#include <InjectionMode.h>

// Список процессов из реестра с их режимами инъекции.
// Перенесён из GlassPlatform — теперь работает в контексте целевого процесса.
class AttachList
{
public:
    static Shared::InjectionMode GetProcessMode(const std::wstring& exeName);
    static bool IsProcessAllowed(const std::wstring& exeName);
    static void Initialize();
    // Логирует все загруженные записи. Вызывать только в хост-процессе.
    static void LogAll();

private:
    static std::wstring Normalize(const std::wstring& exeName);
    static std::unordered_map<std::wstring, DWORD> m_allowList;
};
