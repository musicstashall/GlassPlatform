#pragma once
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
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
    // Все записи реестра (имя в нижнем регистре → сырое значение). Нужен хосту, чтобы
    // опубликовать гейт загрузки (R7): Forbidden-записи попадают в deny-список.
    static void GetEntries(std::vector<std::pair<std::wstring, DWORD>>& out);

private:
    static std::wstring Normalize(const std::wstring& exeName);
    static std::unordered_map<std::wstring, DWORD> m_allowList;
};
