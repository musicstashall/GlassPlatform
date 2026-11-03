#pragma once

#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>

// Обследование текущей установки для карточек продуктов (Task F, стадии 1-2):
// целостность файлов по таблицам File/MsiFileHash встроенного MSI (повреждение,
// подмена), подпись файлов относительно издателя самого установщика, параметры
// реестра установки, службы/задачи автозапуска и сравнение версий.
namespace GlassProductInspect {

enum class Reason {
    FileMissing,       // файл из состава продукта отсутствует
    FileSizeMismatch,  // размер файла не совпадает с пакетом
    FileHashMismatch,  // контрольная сумма не совпадает (повреждение/подмена)
    SignatureMismatch, // подпись файла не принадлежит издателю установщика
    RegistryInvalid,   // параметры установки в реестре нечитаемы/неверны
    AutostartMissing,  // служба или задача автозапуска отсутствует
    VersionOutdated,   // установлена более старая версия
};

struct ProductReport {
    bool installed = false;
    bool updateAvailable = false;
    bool damaged = false;
    std::wstring installedVersion;
    std::vector<Reason> reasons;
    std::wstring registryValueNames; // имена некорректных значений реестра (диагностика)
};

struct Report {
    bool ran = false;
    ProductReport x64;
    ProductReport x86;
    ProductReport settings;
    ProductReport og;
};

struct Inputs {
    HINSTANCE instance = nullptr; // модуль Setup: встроенные MSI (RT_RCDATA)
    std::wstring signerSha1;      // SHA-1 издателя самого установщика; пусто — без проверки подписи
    bool os64 = false;
    // Идентификаторы встроенных MSI-ресурсов.
    UINT msiResX64 = 0;
    UINT msiResX86 = 0;
    UINT msiResSettings = 0;
    UINT msiResOg = 0;
    const std::atomic_bool* cancelled = nullptr; // остановка между проверками продуктов
};

Report Run(const Inputs& in);

} // namespace GlassProductInspect
