#pragma once

#include <Windows.h>
#include <string>

namespace Shared::OpenGlassDwmGuard
{
    // Маркер живёт в реестре и переживает выход из системы: это долговременная
    // запись о сработавшем «стороже», а не состояние процесса.
    inline constexpr wchar_t kMarkerSubKey[] = L"Software\\StanislavKa\\GlassPlatform\\OpenGlassGuard";

    inline constexpr DWORD kArmedBySetup = 1;
    inline constexpr DWORD kArmedBySettings = 2;

    inline constexpr DWORD kReasonPairInWindow = 0x1;      // второй краш в течение окна 30 с
    inline constexpr DWORD kReasonDangerSecondCrash = 0x2; // второй краш в режиме Danger
    inline constexpr DWORD kReasonFuseTotal = 0x3;         // третий краш за цикл наблюдения (вне окна)

    inline constexpr DWORD kLastResultNone = 0;
    inline constexpr DWORD kLastResultClean = 1;       // окно истекло без карантина
    inline constexpr DWORD kLastResultQuarantined = 2; // карантин выполнен
    inline constexpr DWORD kLastResultAborted = 3;     // Disarm до конца окна
    inline constexpr DWORD kLastResultShutdown = 4;    // система завершает работу
    inline constexpr DWORD kLastResultTimeout = 5;     // жёсткий потолок 120 с

    inline constexpr DWORD kStateIdle = 0;
    inline constexpr DWORD kStateWatching = 1;
    inline constexpr DWORD kStateSuspect = 2;
    inline constexpr DWORD kStateDanger = 3;
    inline constexpr DWORD kStateQuarantined = 4;

    struct MarkerInfo
    {
        DWORD armedBy = 0;
        DWORD osBuild = 0;
        DWORD quarantine = 0;      // 1 — карантин активен
        DWORD reason = 0;          // kReason*
        DWORD lastResult = 0;      // kLastResult*
        DWORD crashCount = 0;      // меток краша внутри окна на момент чтения маркера
        ULONGLONG armedAt = 0;     // FILETIME UTC
        ULONGLONG quarantinedAt = 0; // FILETIME UTC
        std::wstring ogVersion;    // версия OG на момент последнего запуска «сторожа»
    };

    /// Включить «сторожа»: снять базовую идентичность dwm.exe текущей сессии,
    /// записать маркер и запустить фоновый поток наблюдения.
    /// Сторож живёт до kMaxWatchSeconds (120 с); без единого краша выходит досрочно
    /// через kCleanExitSeconds (30 с). Краш считается внутри скользящего окна 30 с.
    /// Кросс-процессная единственность — именованный мьютекс Local\GlassPlatform.OpenGlassDwmGuard.Watch:
    /// если наблюдение уже ведёт другой процесс, Arm() возвращает true, но поток не стартует —
    /// маркер и Stop/Disable выполнит владелец. Идемпотентно в пределах процесса.
    /// Всегда вызывать ДО команды старта OG — базовый dwm.exe снимается синхронно.
    /// Авто-сброс-событие Local\GlassPlatform.OpenGlassDwmGuard.StateChanged взводится
    /// на каждой смене состояния (Watching/Suspect/Danger/Quarantined/конец) — UI может
    /// ждать на нём вместо опроса (GlassSettings поллит маркер по таймеру).
    [[nodiscard]] bool Arm(DWORD armedBy, const std::wstring& ogVersion) noexcept;

    /// Прервать активное наблюдение без записи карантина (старт OG не состоялся).
    void Disarm() noexcept;

    /// Текущее состояние (kState*).
    [[nodiscard]] DWORD QueryState() noexcept;

    /// Фоновый поток ещё работает (в состояниях Watching/Suspect/Danger).
    [[nodiscard]] bool IsWatching() noexcept;

    /// Прочитать долговременный маркер. false — ключа нет (OG ни разу не наблюдали).
    [[nodiscard]] bool ReadMarker(MarkerInfo& out) noexcept;

    /// Карантин маркера активен.
    [[nodiscard]] bool IsQuarantined() noexcept;

    /// Запрет автозапуска OG: true — старт пропустить.
    /// Если в маркере записана версия OG и в системе установлена СТРОГО БОЛЕЕ НОВАЯ
    /// версия, карантин снимается автоматически (новая сборка могла исправить
    /// проблему) и возвращается false. Любое непредвиденное сравнение версий
    /// трактуется как «оставить карантин».
    [[nodiscard]] bool ShouldSuppressOpenGlassStart(const std::wstring& installedOgVersion) noexcept;

    /// Ручное снятие карантина (GlassSettings). История крашей сохраняется.
    [[nodiscard]] bool ClearQuarantine() noexcept;
}
