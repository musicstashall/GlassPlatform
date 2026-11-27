#pragma once

// Чистая логика «сторожа» падений DWM после старта OpenGlass.
// Без зависимостей от Windows: компилируется и тестируется отдельно
// (см. GlassSetup/tests/openglass_dwm_guard_policy_mock.py).
//
// Спецификация (согласие 2026-11-27):
//   окно подсчёта крашей — скользящие 30 с (по меткам AppendCrash);
//   второй краш DWM в этом окне → немедленный стоп OG + отключение автозапуска;
//   «мгновенный» краш (≤5 с после старта) вводит режим Danger:
//   следующий краш → тот же немедленный стоп;
//   одиночный мгновенный краш без второго — Suspect, без остановки;
//   абсолютный предохранитель: третий краш за один цикл наблюдения → карантин
//   независимо от окна (ловит «медленный» цикл 0→35→70 с);
//   фоновый сторож живёт до kMaxWatchSeconds (120 с) с чистого старта досрочно
//   выключается через kCleanExitSeconds без единого краша.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Shared::OpenGlassDwmGuardPolicy
{
    inline constexpr unsigned kWatchWindowSeconds = 30;
    inline constexpr unsigned long long kWatchWindowTicks = 300000000ull; // 30 с в 100-нс единицах
    inline constexpr unsigned kDangerThresholdMs = 5000;
    inline constexpr size_t kMaxCrashTimesStored = 8;
    inline constexpr unsigned kMaxWatchSeconds = 120;   // жёсткий потолок жизни сторожа
    inline constexpr unsigned kCleanExitSeconds = 30;   // без крашей — выйти досрочно
    inline constexpr size_t kFuseCrashCount = 3;        // абсолютный предохранитель

    enum class Action : std::uint8_t
    {
        Suspect = 0,          // одиночный краш вне Danger — без остановки
        EnterDanger = 1,      // мгновенный краш сразу после старта — режим «опасность»
        QuarantinePair = 2,   // второй краш в окне 30 с
        QuarantineDanger = 3, // второй краш в режиме Danger
        QuarantineFuse = 4,   // третий краш за цикл наблюдения — вне зависимости от окна
    };

    /// Добавить метку краша и отрезать всё старше скользящего окна.
    /// Входной список может содержать метки прошлых запусков — окно привязано к now,
    /// поэтому устаревшие автоматически отбрасываются.
    inline void AppendCrash(std::vector<unsigned long long>& times,
                            unsigned long long nowFileTime,
                            unsigned long long windowTicks = kWatchWindowTicks) noexcept
    {
        times.push_back(nowFileTime);
        const unsigned long long cutoff = nowFileTime > windowTicks ? nowFileTime - windowTicks : 0ull;
        times.erase(std::remove_if(times.begin(), times.end(),
                                   [cutoff](unsigned long long t) { return t < cutoff; }),
                    times.end());
        if (times.size() > kMaxCrashTimesStored)
            times.erase(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(times.size() - kMaxCrashTimesStored));
    }

    /// Сколько крашей в скользящем окне, заканчивающемся в now.
    [[nodiscard]] inline size_t CountWithinWindow(const std::vector<unsigned long long>& times,
                                                  unsigned long long nowFileTime,
                                                  unsigned long long windowTicks = kWatchWindowTicks) noexcept
    {
        const unsigned long long cutoff = nowFileTime > windowTicks ? nowFileTime - windowTicks : 0ull;
        return static_cast<size_t>(std::count_if(times.begin(), times.end(),
                                                 [cutoff](unsigned long long t) { return t >= cutoff; }));
    }

    /// Отбросить устаревшие метки при загрузке маркера (окно привязано к моменту загрузки).
    inline void DropExpired(std::vector<unsigned long long>& times,
                            unsigned long long nowFileTime,
                            unsigned long long windowTicks = kWatchWindowTicks) noexcept
    {
        const unsigned long long cutoff = nowFileTime > windowTicks ? nowFileTime - windowTicks : 0ull;
        times.erase(std::remove_if(times.begin(), times.end(),
                                   [cutoff](unsigned long long t) { return t < cutoff; }),
                    times.end());
    }

    /// Решение по новому крашу.
    /// wasDanger — сторож уже находился в режиме Danger до этого краша;
    /// countInWindow — число крашей в скользящем окне ПОСЛЕ добавления текущего;
    /// countTotal — всего крашей за этот цикл наблюдения (не стареет);
    /// instant — краш произошёл не позднее kDangerThresholdMs после старта.
    [[nodiscard]] constexpr Action EvaluateCrash(bool wasDanger, size_t countInWindow, size_t countTotal, bool instant) noexcept
    {
        if (wasDanger)
            return Action::QuarantineDanger;
        if (countInWindow >= 2)
            return Action::QuarantinePair; // точная причина: два краша в окне
        if (countTotal >= kFuseCrashCount)
            return Action::QuarantineFuse; // абсолютный предохранитель вне окна
        if (instant)
            return Action::EnterDanger;
        return Action::Suspect;
    }

    [[nodiscard]] constexpr bool IsQuarantine(Action action) noexcept
    {
        return action == Action::QuarantinePair || action == Action::QuarantineDanger
            || action == Action::QuarantineFuse;
    }

    /// Сравнение версий «3.0.2.3749» > «3.0.2.3748» лексикографически по числовым токенам.
    /// Все непустые токены должны быть числовыми; отсутствующие токены считаются нулями
    /// («3.0» == «3.0.0.0»). Некорректный ввод → false (карантин не снимается).
    [[nodiscard]] inline bool IsVersionStrictlyGreater(std::wstring_view a, std::wstring_view b) noexcept
    {
        enum class TokenResult { End, Ok, Error };

        auto nextToken = [](std::wstring_view& rest, unsigned long long& value) -> TokenResult
        {
            if (rest.empty())
                return TokenResult::End;
            const size_t dot = rest.find(L'.');
            const bool last = dot == std::wstring_view::npos;
            const std::wstring_view token = rest.substr(0, last ? rest.size() : dot);
            rest = last ? std::wstring_view{} : rest.substr(dot + 1);
            if (token.empty())
                return TokenResult::Error;
            unsigned long long parsed = 0;
            for (const wchar_t ch : token)
            {
                if (ch < L'0' || ch > L'9')
                    return TokenResult::Error;
                parsed = parsed * 10ull + static_cast<unsigned long long>(ch - L'0');
                if (parsed > 1000000ull)
                    return TokenResult::Error; // заведомо не версия, не переполняем молча
            }
            value = parsed;
            return TokenResult::Ok;
        };

        std::wstring_view ra = a, rb = b;
        for (;;)
        {
            unsigned long long ta = 0, tb = 0;
            const TokenResult ra_ = nextToken(ra, ta);
            const TokenResult rb_ = nextToken(rb, tb);
            if (ra_ == TokenResult::Error || rb_ == TokenResult::Error)
                return false;
            const bool endA = ra_ == TokenResult::End;
            const bool endB = rb_ == TokenResult::End;
            if (endA && endB)
                return false; // версии равны
            if (endA) ta = 0;
            if (endB) tb = 0;
            if (ta != tb)
                return ta > tb;
        }
    }
}
