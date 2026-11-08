#pragma once

#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>

// Обследование текущей установки для карточек продуктов (Task F, стадии 1-2):
// целостность файлов по таблицам File/MsiFileHash установленного MSI (повреждение,
// подмена), подпись файлов относительно издателя самого установщика, параметры
// реестра установки, службы/задачи автозапуска и сравнение версий.
namespace GlassProductInspect {

enum class Reason {
    FileMissing,       // файл из состава продукта отсутствует
    FileSizeMismatch,  // размер файла не совпадает с пакетом
    FileHashMismatch,  // контрольная сумма не совпадает (повреждение/подмена)
    SignatureMismatch, // подпись файла не принадлежит издателю установщика
    RegistryInvalid,   // параметры установки в реестре нечитаемы/неверны
    InspectionUnavailable, // no verified installed-package baseline; never equate unknown with healthy
    AutostartMissing,  // служба или задача автозапуска отсутствует
    VersionOutdated,   // установлена более старая версия
};

struct ProductReport {
    bool installed = false; // MSI registration only; never inferred from a service.
    bool residual = false; // Physical OpenGlass service/tasks without a registered MSI.
    bool residualSafe = false; // Read-only root/action/account validation succeeded.
    bool updateAvailable = false;
    bool damaged = false;
    bool sourceVerified = false;
    bool integrityKnown = false;
    std::wstring targetVersion;
    std::wstring installedVersion;
    std::wstring installedPackageVersion;
    std::vector<Reason> reasons;
    std::wstring registryValueNames; // имена некорректных значений реестра (диагностика)
};

struct Report {
    bool ran = false;
    // UI capability hint only; execution re-enumerates and verifies the cache.
    std::wstring officialSecureUxThemeCode;
    std::wstring secureUxThemePresenceDetail;
    ProductReport x64;
    ProductReport x86;
    ProductReport settings; // compatibility view for the current UI
    ProductReport settings64,settings32;
    ProductReport secureUxTheme;
    ProductReport og;
};

struct Inputs {
    std::wstring signerSha1;
    bool os64 = false;
    // Execution preflight already acquired its media: inspect installed facts
    // without fetching the catalogue or preparing/downloading packages again.
    bool inspectSources = true;
    const std::atomic_bool* cancelled = nullptr; // остановка между проверками продуктов
};

Report Run(const Inputs& in);

[[nodiscard]] std::wstring QuerySelfSignerSha1() noexcept;
[[nodiscard]] bool FileHasSameSignerAsSetup(const std::wstring& path) noexcept;

} // namespace GlassProductInspect
