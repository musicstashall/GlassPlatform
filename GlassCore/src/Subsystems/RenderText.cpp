//   Первичный путь: DrawTextWithGlow (uxtheme.dll, ordinal #126), bPreMultiply=TRUE.
//   Вызывается всегда при наличии pfn: основная роль — корректный premultiplied-alpha текст
//   в BPBF_TOPDOWNDIB / offscreen DIB. Параметры ореола (radius/intensity/crGlow) задаются
//   настройками Glow и могут быть нулевыми — тогда визуально как обычный DrawText, но с
//   правильным альфа-каналом. Без pfn — деградация на DrawTextW.
//
//   Fallback: DrawThemeTextEx с DTT_COMPOSITED | DTT_TEXTCOLOR | DTT_CALLBACK —
//   тот же эффект через другой механизм.
//
//   Оба пути обёрнуты в DrawThemeContent (BPBF_TOPDOWNDIB + BLENDFUNCTION),
//   что обеспечивает правильный alpha blend с DC получателя.
//
//   ExtTextOutWithAlpha: два режима рендера.
//   Режим Single-pass (white-on-black):
//     Coverage → ProcessWhiteOnBlackPixels (gamma).
//   Режим Dual-pass (black+white diff):
//     max(aR,aG,aB) + гамма маски; UI-гамма из капсулы ×2 при рендере (см. InkCapsuleQuery::kDualPassCoverageGammaMultiplier).
//   EditSingleLinePlain: узкий paint rect (+1px), как у многострочной сессии — иначе kGlyphMargin
//   размывает эффект слайдеров; dual/single — PlainEditUseDualPassSingleLinePlain; HWND из EditHooks иначе WindowFromDC.

#include "CorePch.h"
#include "Subsystems/RenderText.h"
#include <Subsystems/TextGlowClientRegistry.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/MenuHandler.h>
#include <Subsystems/RootLiveReload.h>
#include <Helpers/MenuUtils.h>
#include <Hooks/UxThemeHooks.h>
#include <Hooks/Button.h>
#include <Hooks/TooltipHooks.h>
#include <Helpers/ThemeHelper.h>
#include <DebugLog.h>
#include <Helpers/RegHelper.h>
#include <GlassInkAlphaCapsule.h>
#include <InkCapsuleQuery.h>
#include <wil/registry.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>
#include <type_traits>
#include <string>
#include <vector>

#pragma comment(lib, "msimg32.lib")

namespace GlassCore::Text
{
    namespace detail
    {
        constexpr float kDefCoverageGamma = 1775.f / 1000.f;
        constexpr BYTE  kDefTargetBlendAlpha = 190;
        // Новые параметры
        constexpr BYTE  kDefRenderMode = 0;    // 0=auto, 1=single, 2=dual

        constexpr std::wstring_view kRegValueGlassInkAlphaCapsule = L"GlassInkAlphaCapsule";
        const std::vector<std::wstring_view> kLegacyRegKeyTree{ L"RenderText" };

        // InkCapsuleQuery — только load/deserialize (registry, UI, preview bytes).
        // Hot path ExtTextOutWithAlpha читает per-call inkCapsule / runtime globals;
        // reload — LoadRenderTextSettings / ReloadRenderTextInkFromRegistry.

        using GlassInkProfilePayload = Shared::InkCapsule::ProfilePayload;
        using GlassInkAlphaCapsuleBlob = Shared::InkCapsule::GlassInkAlphaCapsule;
        using GlobalTextGlow = Shared::InkCapsule::GlobalTextGlow;

        GlobalTextGlow g_runtimeGlowLight = Shared::InkCapsule::MakeFactoryGlobalGlow();
        GlobalTextGlow g_runtimeGlowDark = Shared::InkCapsule::MakeFactoryGlobalGlow();

        constexpr uint32_t kGlassInkCapsuleMagic = Shared::InkCapsule::kMagic;

        // Режим «Авто»: kAutoProfileSingle (single-pass) и kAutoProfileDual (dual) — см. GlassInkAlphaCapsule.h.
        constexpr const GlassInkProfilePayload& kProfileAutoSingle = Shared::InkCapsule::kAutoProfileSingle;
        constexpr const GlassInkProfilePayload& kProfileAutoDual = Shared::InkCapsule::kAutoProfileDual;

        // Заводские значения для пользовательских профилей (один / два прохода) после сброса.
        constexpr const GlassInkProfilePayload& kFactoryUserProfile = Shared::InkCapsule::kFactoryUserProfileSingle;
        constexpr const GlassInkProfilePayload& kFactoryUserProfileDual = Shared::InkCapsule::kFactoryUserProfileDual;

        GlassInkProfilePayload g_userProfileSingle = Shared::InkCapsule::kFactoryUserProfileSingle;
        GlassInkProfilePayload g_userProfileDual = Shared::InkCapsule::kFactoryUserProfileDual;

        [[nodiscard]] DWORD FloatToThousandths(float v) noexcept
        {
            v = std::clamp(v, 0.1f, 10.f);
            return static_cast<DWORD>(v * 1000.f + (v >= 0 ? 0.5f : -0.5f));
        }

        [[nodiscard]] float ReadFloatThou(std::wstring_view valueName, float defaultVal, float minV, float maxV) noexcept
        {
            const DWORD defTh = FloatToThousandths(defaultVal);
            const DWORD th = Shared::RegHelper::Get<DWORD>(kLegacyRegKeyTree, valueName, defTh);
            return std::clamp(static_cast<float>(th) / 1000.f, minV, maxV);
        }

        [[nodiscard]] BYTE ReadByte255(std::wstring_view valueName, BYTE defaultVal) noexcept
        {
            const DWORD d = Shared::RegHelper::Get<DWORD>(kLegacyRegKeyTree, valueName, static_cast<DWORD>(defaultVal));
            return static_cast<BYTE>(std::min<DWORD>(d, 255));
        }

        void ApplyProfileInkGenerationToGlobals(
            const GlassInkProfilePayload& p,
            bool applyDualPassMultiplier) noexcept
        {
            GlassInkProfilePayload clamped = p;
            Shared::InkCapsuleQuery::ClampProfile(clamped);
            const uint32_t effectiveTh = Shared::InkCapsuleQuery::EffectiveCoverageGammaThousandths(
                clamped.coverageGammaThousandths,
                applyDualPassMultiplier);
            g_ExtTextOutCoverageGamma = static_cast<float>(effectiveTh) / 1000.f;
            g_ExtTextOutPseudoGlowEnabled = clamped.pseudoGlowEnabled != 0;
            g_ExtTextOutPseudoGlowRadiusThousandths = clamped.pseudoGlowRadiusThousandths;
            g_ExtTextOutPseudoGlowIntensity = clamped.pseudoGlowIntensity;
        }

        void ApplyProfilePayloadToGlobals(
            const GlassInkProfilePayload& p,
            bool applyDualPassMultiplier) noexcept
        {
            ApplyProfileInkGenerationToGlobals(p, applyDualPassMultiplier);
            g_ExtTextOutTargetBlendAlpha =
                static_cast<BYTE>(std::min<uint32_t>(p.targetBlendAlpha, 255));
        }

        void ClampGlobalGlowInPlace(GlobalTextGlow& g) noexcept
        {
            g.glowEnabled = g.glowEnabled ? 1u : 0u;
            g.glowRadiusThousandths = std::min(g.glowRadiusThousandths, 1000u);
            g.glowSpreadMode = std::min(g.glowSpreadMode, 2u);
            const uint32_t alpha = std::min((g.glowColor >> 24) & 0xFFu, 255u);
            g.glowColor = (g.glowColor & 0x00FFFFFFu) | (alpha << 24);
        }

        [[nodiscard]] const GlobalTextGlow& ResolveEffectiveGlobalTextGlow() noexcept
        {
            return Shared::ThemeHelper::IsRegistryDarkModeFromColorWindow()
                ? g_runtimeGlowDark
                : g_runtimeGlowLight;
        }

        void SyncEffectiveGlowToFlatGlobals() noexcept
        {
            const GlobalTextGlow& g = ResolveEffectiveGlobalTextGlow();
            g_ExtTextOutGlowEnabled = g.glowEnabled != 0;
            g_ExtTextOutGlowRadiusThousandths =
                g.glowRadiusThousandths > 1000u ? 1000u : g.glowRadiusThousandths;
            g_ExtTextOutGlowIntensityThousandths =
                Shared::InkCapsule::ResolveGlowIntensityThousandths(g);
            g_ExtTextOutGlowSpreadMode = std::min(g.glowSpreadMode, 2u);
        }

        [[nodiscard]] GlobalTextGlow& MutableRuntimeGlowForRegistryTheme() noexcept
        {
            return Shared::ThemeHelper::IsRegistryDarkModeFromColorWindow()
                ? g_runtimeGlowDark
                : g_runtimeGlowLight;
        }

        [[nodiscard]] bool TryLoadThemeGlowFromHive(
            HKEY hive, std::wstring_view valueName, GlobalTextGlow& out) noexcept
        {
            const auto blob = wil::reg::try_get_value_binary(
                hive, Shared::RegHelper::g_regPath.data(), valueName.data(), REG_BINARY);
            if (!blob.has_value() || blob->size() < sizeof(GlobalTextGlow))
                return false;
            Shared::InkCapsule::DeserializeGlobalTextGlow(out, blob->data(), blob->size());
            ClampGlobalGlowInPlace(out);
            return true;
        }

        void LoadThemeGlowFromRegistry(GlobalTextGlow& light, GlobalTextGlow& dark) noexcept
        {
            light = Shared::InkCapsule::MakeFactoryGlobalGlow();
            dark = Shared::InkCapsule::MakeFactoryGlobalGlow();
            for (const HKEY hive : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
            {
                GlobalTextGlow tmpLight = light;
                GlobalTextGlow tmpDark = dark;
                const bool gotLight = TryLoadThemeGlowFromHive(
                    hive, Shared::InkCapsule::kValLightGlobalTextGlow, tmpLight);
                const bool gotDark = TryLoadThemeGlowFromHive(
                    hive, Shared::InkCapsule::kValDarkGlobalTextGlow, tmpDark);
                if (gotLight)
                    light = tmpLight;
                if (gotDark)
                    dark = tmpDark;
                if (gotLight || gotDark)
                    return;
            }
        }

        [[nodiscard]] bool GlobalTextGlowEqualsFactory(const GlobalTextGlow& g) noexcept
        {
            return Shared::InkCapsule::IsFactoryGlobalGlow(g);
        }

        void WriteThemeGlowToRegistryOrRemove(
            wil::unique_hkey& hKey, std::wstring_view valueName, const GlobalTextGlow& g) noexcept
        {
            if (GlobalTextGlowEqualsFactory(g))
            {
                (void)RegDeleteValueW(hKey.get(), valueName.data());
                return;
            }
            (void)RegSetValueExW(
                hKey.get(),
                valueName.data(),
                0,
                REG_BINARY,
                reinterpret_cast<const BYTE*>(&g),
                static_cast<DWORD>(sizeof(GlobalTextGlow)));
        }

        void LogGlobalGlowApplied(const wchar_t* source, const GlobalTextGlow& g) noexcept
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Text] %s GlobalGlow glowEnabled=%u r=%u color=0x%08X spread=%u",
                source,
                g.glowEnabled,
                g.glowRadiusThousandths,
                g.glowColor,
                g.glowSpreadMode);
        }

        void LoadGlobalGlowFromRegistry() noexcept
        {
            LoadThemeGlowFromRegistry(g_runtimeGlowLight, g_runtimeGlowDark);
            SyncEffectiveGlowToFlatGlobals();
            LogGlobalGlowApplied(L"LoadGlobalGlow light", g_runtimeGlowLight);
            LogGlobalGlowApplied(L"LoadGlobalGlow dark", g_runtimeGlowDark);
        }

        void ApplyActiveProfileToGlobals() noexcept
        {
            const BYTE rm = g_ExtTextOutRenderMode;
            if (rm == 0)
                // Базовая линия при «Авто»: профиль dual из капсулы (reload), не захардкоженный kProfileAutoDual.
                ApplyProfilePayloadToGlobals(g_userProfileDual, true);
            else if (rm == 1)
                ApplyProfilePayloadToGlobals(g_userProfileSingle, false);
            else
                ApplyProfilePayloadToGlobals(g_userProfileDual, true);
        }

        enum class ApplyInkCapsuleMode { RegistryLoad, PreviewOverlay };

        static void LogTextInkApplyCapsule(ApplyInkCapsuleMode mode) noexcept
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][TextInk] ApplyCapsule mode=%s rm=%u gamma=%.4f glowR=%u glowI=%u tblend=%u",
                mode == ApplyInkCapsuleMode::RegistryLoad ? L"Registry" : L"Preview",
                static_cast<unsigned>(g_ExtTextOutRenderMode),
                g_ExtTextOutCoverageGamma,
                static_cast<unsigned>(g_ExtTextOutGlowRadiusThousandths),
                static_cast<unsigned>(g_ExtTextOutGlowIntensityThousandths),
                static_cast<unsigned>(g_ExtTextOutTargetBlendAlpha));
        }

        [[nodiscard]] static uint32_t FingerprintInkBlob(const uint8_t* data, size_t byteCount) noexcept
        {
            constexpr uint32_t kFnvOffset = 2166136261u;
            constexpr uint32_t kFnvPrime = 16777619u;
            if (!data || byteCount == 0)
                return 0;
            uint32_t h = kFnvOffset;
            for (size_t i = 0; i < byteCount; ++i)
            {
                h ^= data[i];
                h *= kFnvPrime;
            }
            return h;
        }

        static void LogInkCapsuleFingerprintRegistry(const uint8_t* data, size_t byteCount) noexcept
        {
            const uint32_t fp = FingerprintInkBlob(data, byteCount);
            DebugLog(DBG_NORMAL,
                L"[GlassCore][TextInk] InkCapsuleFingerprint Registry fp=0x%08X sz=%zu",
                fp, byteCount);
        }

        [[nodiscard]] bool ApplyInkCapsuleBytesToEngine(
            const uint8_t* data, size_t byteCount, ApplyInkCapsuleMode mode) noexcept
        {
            GlassInkAlphaCapsuleBlob cap{};
            if (!Shared::InkCapsuleQuery::TryDeserializeCapsuleBytes(data, byteCount, cap))
                return false;

            const uint32_t rm = std::min(cap.renderMode, 2u);
            g_ExtTextOutRenderMode = static_cast<BYTE>(rm);
            if (mode == ApplyInkCapsuleMode::RegistryLoad)
            {
                g_userProfileSingle = cap.single;
                g_userProfileDual = cap.dual;
                ApplyActiveProfileToGlobals();
            }
            else
            {
                if (rm == 0u)
                    ApplyProfilePayloadToGlobals(cap.dual, true);
                else if (rm == 1u)
                    ApplyProfilePayloadToGlobals(cap.single, false);
                else
                    ApplyProfilePayloadToGlobals(cap.dual, true);
            }
            if (mode == ApplyInkCapsuleMode::RegistryLoad)
            {
                LoadGlobalGlowFromRegistry();
                LogInkCapsuleFingerprintRegistry(data, byteCount);
            }
            LogTextInkApplyCapsule(mode);
            return true;
        }

        [[nodiscard]] bool TryApplyPreviewCapsuleFromBytes(const uint8_t* data, size_t byteCount) noexcept
        {
            return ApplyInkCapsuleBytesToEngine(data, byteCount, ApplyInkCapsuleMode::PreviewOverlay);
        }

        [[nodiscard]] bool TryApplyCapsuleFromBytes(const uint8_t* data, size_t byteCount) noexcept
        {
            return ApplyInkCapsuleBytesToEngine(data, byteCount, ApplyInkCapsuleMode::RegistryLoad);
        }

        [[nodiscard]] bool TryLoadCapsuleFromHive(HKEY hive) noexcept
        {
            GlassInkAlphaCapsuleBlob cap{};
            if (!Shared::InkCapsuleQuery::TryReadLocalCapsuleFromHive(hive, cap))
                return false;

            const auto* bytes = reinterpret_cast<const uint8_t*>(&cap);
            return TryApplyCapsuleFromBytes(bytes, sizeof(cap));
        }

        void PopulateUserProfilesFromGlobals() noexcept
        {
            const GlassInkAlphaCapsuleBlob factory = Shared::InkCapsuleQuery::MakeFactoryCapsule();
            g_userProfileSingle = factory.single;
            g_userProfileDual = factory.dual;
        }

        void LoadLegacyRenderTextSubkey() noexcept
        {
            g_ExtTextOutCoverageGamma = ReadFloatThou(L"CoverageGamma", kDefCoverageGamma, 0.1f, 10.f);
            g_ExtTextOutTargetBlendAlpha = ReadByte255(L"TargetBlendAlpha", kDefTargetBlendAlpha);
            g_ExtTextOutRenderMode = ReadByte255(L"RenderMode", kDefRenderMode);
            PopulateUserProfilesFromGlobals();
            ApplyActiveProfileToGlobals();
        }
    } // namespace detail
    
    // ── Глобальные параметры ────────────────────────────────────────────────────
    float g_ExtTextOutCoverageGamma = detail::kDefCoverageGamma;
    bool g_ExtTextOutGlowEnabled = false;
    uint32_t g_ExtTextOutGlowRadiusThousandths = 0;
    uint32_t g_ExtTextOutGlowIntensityThousandths = 0;
    uint32_t g_ExtTextOutGlowSpreadMode = 0;
    bool g_ExtTextOutPseudoGlowEnabled = false;
    uint32_t g_ExtTextOutPseudoGlowRadiusThousandths = 610u;
    uint32_t g_ExtTextOutPseudoGlowIntensity = 113u;
    BYTE  g_ExtTextOutTargetBlendAlpha = detail::kDefTargetBlendAlpha;
    BYTE  g_ExtTextOutRenderMode = detail::kDefRenderMode;

    [[nodiscard]] bool PlainEditUseDualPass(HWND hWnd, BYTE renderMode) noexcept
    {
        if (!hWnd)
            return true;
        const DWORD style = static_cast<DWORD>(GetWindowLongW(hWnd, GWL_STYLE));
        const bool hasHScroll = (style & WS_HSCROLL) != 0;
        if (renderMode == 1)
            return false;
        if (renderMode == 2)
            return true;
        if (hasHScroll)
            return false;
        return true;
    }

    [[nodiscard]] bool PlainEditUseDualPassSingleLinePlain(HWND hWnd, BYTE renderMode) noexcept
    {
        (void)hWnd;
        // Однострочный plain: та же политика, что у многострочного без WS_HSCROLL и у FullQuality в «Авто»
        // (ветка HSCROLL у multiline не применяется — иначе однострочный с WS_HSCROLL уходит в single-pass и выглядит «жирнее»).
        if (renderMode == 1)
            return false;
        if (renderMode == 2)
            return true;
        return true;
    }

    [[nodiscard]] BYTE ResolveEffectiveTargetBlendAlpha(bool useDualPassPath) noexcept
    {
        using namespace detail;
        const uint32_t rm = g_ExtTextOutRenderMode;
        const GlassInkProfilePayload& profile = (rm == 1u)
            ? g_userProfileSingle
            : (rm == 2u)
                ? g_userProfileDual
                : (useDualPassPath ? g_userProfileDual : g_userProfileSingle);
        return static_cast<BYTE>(std::min<uint32_t>(profile.targetBlendAlpha, 255u));
    }

    const Shared::InkCapsule::ProfilePayload& GetRuntimeInkCapsuleProfile(bool dualPassPath) noexcept
    {
        using namespace detail;
        const uint32_t rm = g_ExtTextOutRenderMode;
        if (rm == 1u)
            return g_userProfileSingle;
        if (rm == 2u)
            return g_userProfileDual;
        return dualPassPath ? g_userProfileDual : g_userProfileSingle;
    }

    ExtTextOutInkDrawParams BuildInkDrawParamsFromProfile(
        const Shared::InkCapsule::ProfilePayload& profile,
        bool applyDualPassMultiplier) noexcept
    {
        Shared::InkCapsule::ProfilePayload clamped = profile;
        Shared::InkCapsuleQuery::ClampProfile(clamped);
        const uint32_t effectiveTh = Shared::InkCapsuleQuery::EffectiveCoverageGammaThousandths(
            clamped.coverageGammaThousandths,
            applyDualPassMultiplier);
        ExtTextOutInkDrawParams out{};
        out.coverageGamma = static_cast<float>(effectiveTh) / 1000.f;
        out.targetBlendAlpha = static_cast<BYTE>(std::min<uint32_t>(clamped.targetBlendAlpha, 255u));
        out.pseudoGlowEnabled = clamped.pseudoGlowEnabled != 0;
        out.pseudoGlowRadiusThousandths = clamped.pseudoGlowRadiusThousandths;
        out.pseudoGlowIntensity = clamped.pseudoGlowIntensity;
        return out;
    }

    // ── DrawTextWithGlow (uxtheme ordinal #126) ─────────────────────────────────
    using PfnDrawTextWithGlow = HRESULT(WINAPI*)(
        HDC, LPCWSTR, int, LPRECT, UINT,
        UINT, UINT, UINT, UINT, BOOL,
        DTT_CALLBACK_PROC, LPARAM);

    static PfnDrawTextWithGlow GetDrawTextWithGlow()
    {
        static const auto fn = reinterpret_cast<PfnDrawTextWithGlow>(
            GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(126)));
        return fn;
    }

    // Защита от рекурсии DrawTextWithAlpha ↔ IAT-хуки DrawTextW/ExW (коллбэк #126, uxtheme).
    thread_local unsigned s_drawTextWithAlphaDepth = 0;

    /// user32!DrawTextW — обход IAT exe/comctl32 (паритет BWM: DrawTextW из DLL движка).
    [[nodiscard]] static decltype(&DrawTextW) GetUser32DrawTextW() noexcept
    {
        static const auto pfn = reinterpret_cast<decltype(&DrawTextW)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "DrawTextW"));
        return pfn;
    }

    [[nodiscard]] int InvokeUser32DrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format) noexcept
    {
        const auto pfn = GetUser32DrawTextW();
        return pfn ? pfn(hdc, lpchText, cchText, lprc, format)
                   : DrawTextW(hdc, lpchText, cchText, lprc, format);
    }

    static int CALLBACK DrawTextCallback(
        HDC hdc, LPWSTR pszText, int cchText, LPRECT prc, UINT dwFlags, LPARAM lParam)
    {
        int* pResult = reinterpret_cast<int*>(lParam);
        *pResult = InvokeUser32DrawTextW(hdc, pszText, cchText, prc, dwFlags);
        return *pResult;
    }

    /// Нет видимых символов — не вызывать uxtheme #126 (AutoIt: «пустая» подпись часто = пробел).
    [[nodiscard]] static bool IsDrawTextTrivialForPlainGdi(LPCWSTR lpchText, int cchText) noexcept
    {
        if (!lpchText)
            return true;
        if (cchText == 0)
            return true;

        const int len = (cchText < 0)
            ? static_cast<int>(wcslen(lpchText))
            : cchText;
        if (len <= 0)
            return true;

        for (int i = 0; i < len; ++i)
        {
            if (!iswspace(lpchText[i]))
                return false;
        }
        return true;
    }

    /// Во время DrawTextWithAlpha (#126) uxtheme может вызвать ExtTextOutW — обход ink-конвейера (паритет BWM disable ExtTextOut).
    [[nodiscard]] static bool TryExtTextOutPlainDuringDrawTextWithAlpha(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx,
        decltype(&::ExtTextOutW) origExtTextOut,
        HRESULT& outHr) noexcept
    {
        if (s_drawTextWithAlphaDepth == 0)
            return false;

        if (s_drawTextWithAlphaDepth > 1)
        {
            thread_local DWORD s_extReentryLogTick = 0;
            const DWORD nowTick = GetTickCount();
            if (s_extReentryLogTick == 0 || (nowTick - s_extReentryLogTick) > 2500)
            {
                s_extReentryLogTick = nowTick;
                DebugLog(DBG_WARNING,
                    L"[GlassCore][RenderText] ExtTextOutWithAlpha: reentry depth=%u — orig ExtTextOutW",
                    s_drawTextWithAlphaDepth);
            }
        }

        outHr = origExtTextOut(hdc, x, y, options, lprect, lpString, c, lpDx) ? S_OK : E_FAIL;
        return true;
    }

    void Prepare()
    {
        DebugLog(L"[GlassCore][Text] Prepare");
    }

    void LoadRenderTextSettings() noexcept
    {
        using namespace detail;
        if (TryLoadCapsuleFromHive(HKEY_CURRENT_USER))
        {
            SyncTextGlowHooksFromRegistry();
            return;
        }
        if (TryLoadCapsuleFromHive(HKEY_LOCAL_MACHINE))
        {
            SyncTextGlowHooksFromRegistry();
            return;
        }
        LoadLegacyRenderTextSubkey();
        LoadGlobalGlowFromRegistry();
        SyncTextGlowHooksFromRegistry();
    }

    void SyncTextGlowHooksFromRegistry() noexcept
    {
        GlassCore::Button::SyncTextGlowHooksFromRegistry();
        GlassCore::TooltipHooks::SyncTextGlowHooksFromRegistry();
    }

    void ReloadRenderTextInkFromRegistry() noexcept
    {
        const uint32_t previousSpread = std::min(g_ExtTextOutGlowSpreadMode, 2u);
        LoadRenderTextSettings();
        const uint32_t newSpread = std::min(g_ExtTextOutGlowSpreadMode, 2u);

        if (newSpread < previousSpread)
        {
            GlassCore::TextGlow::SoftInvalidateRegisteredClientsDroppedBySpreadDecrease(
                previousSpread, newSpread);
        }

        GlassCore::TextGlow::RebindTextGlowClientsFromMarkedWindows();
        GlassCore::RootLiveReload::InvalidateTextGlowClientsOnTrackedRoots();
        GlassCore::RootLiveReload::InvalidateTextInkClientsOnTrackedRoots();
    }

    HRESULT SaveRenderTextSettings() noexcept
    {
        using namespace detail;
        GlassInkAlphaCapsuleBlob cap{};
        cap.magic = kGlassInkCapsuleMagic;
        cap.renderMode = static_cast<uint32_t>(g_ExtTextOutRenderMode);
        cap.reservedPad = 0;
        cap.single = g_userProfileSingle;
        cap.dual = g_userProfileDual;

        std::vector<uint8_t> bytes(sizeof(GlassInkAlphaCapsuleBlob));
        static_assert(std::is_trivially_copyable_v<GlassInkAlphaCapsuleBlob>);
        std::memcpy(bytes.data(), &cap, sizeof(cap));

        wil::unique_hkey hKey;
        LSTATUS ls = RegCreateKeyExW(
            HKEY_CURRENT_USER, Shared::RegHelper::g_regPath.data(),
            0, nullptr, 0, KEY_SET_VALUE, nullptr, wil::out_param(hKey), nullptr);
        if (ls != ERROR_SUCCESS) return HRESULT_FROM_WIN32(ls);

        ls = RegSetValueExW(hKey.get(),
            kRegValueGlassInkAlphaCapsule.data(), 0, REG_BINARY,
            bytes.data(), static_cast<DWORD>(bytes.size()));
        if (ls != ERROR_SUCCESS) return HRESULT_FROM_WIN32(ls);

        GlobalTextGlow gLight = detail::g_runtimeGlowLight;
        GlobalTextGlow gDark = detail::g_runtimeGlowDark;
        detail::ClampGlobalGlowInPlace(gLight);
        detail::ClampGlobalGlowInPlace(gDark);
        detail::WriteThemeGlowToRegistryOrRemove(
            hKey, Shared::InkCapsule::kValLightGlobalTextGlow, gLight);
        detail::WriteThemeGlowToRegistryOrRemove(
            hKey, Shared::InkCapsule::kValDarkGlobalTextGlow, gDark);
        return S_OK;
    }

    void Startup() { LoadRenderTextSettings(); DebugLog(L"[GlassCore][Text] Startup"); }
    void Shutdown() { DebugLog(L"[GlassCore][Text] Shutdown"); }
    void Update() {}

    // =========================================================================
    //  Вспомогательный DIB для второго прохода dual-pass рендера.
    //  Один экземпляр на поток; создаётся лениво, растёт с запасом ×1.5.
    // =========================================================================
    struct WhiteDib
    {
        HDC     hDC = nullptr;
        HBITMAP hBmp = nullptr;
        void* pBits = nullptr;
        int     width = 0;
        int     height = 0;

        // Создать/расширить буфер. hdcRef нужен только при первом создании DC.
        bool EnsureSize(HDC hdcRef, int reqW, int reqH) noexcept
        {
            if (reqW <= width && reqH <= height) return hBmp != nullptr;

            const int newW = std::max(reqW * 3 / 2, 64);
            const int newH = std::max(reqH * 3 / 2, 64);

            // Освободить старый bitmap (DC оставляем — переиспользуем)
            if (hBmp)
            {
                if (hDC) SelectObject(hDC, static_cast<HGDIOBJ>(nullptr));
                DeleteObject(hBmp);
                hBmp = nullptr; pBits = nullptr;
            }
            if (!hDC)
            {
                hDC = CreateCompatibleDC(hdcRef);
                if (!hDC) return false;
            }

            BITMAPINFOHEADER bi{};
            bi.biSize = sizeof(bi);
            bi.biWidth = newW;
            bi.biHeight = -newH;   // top-down
            bi.biPlanes = 1;
            bi.biBitCount = 32;
            bi.biCompression = BI_RGB;

            hBmp = CreateDIBSection(hdcRef, reinterpret_cast<BITMAPINFO*>(&bi),
                DIB_RGB_COLORS, &pBits, nullptr, 0);
            if (!hBmp) return false;

            SelectObject(hDC, hBmp);
            width = newW;
            height = newH;
            return true;
        }

        void Release() noexcept
        {
            if (hDC) { SelectObject(hDC, static_cast<HGDIOBJ>(nullptr)); DeleteDC(hDC);    hDC = nullptr; }
            if (hBmp) { DeleteObject(hBmp); hBmp = nullptr; }
            pBits = nullptr; width = height = 0;
        }

        ~WhiteDib() { Release(); }
    };

    thread_local WhiteDib tls_whiteDib;

    [[nodiscard]] static BYTE ApplyCoverageGammaSlow(BYTE raw, float gamma) noexcept;

    struct CoverageGammaLut
    {
        BYTE table[256]{};
        float cachedGamma = -1.f;

        void EnsureFor(float gamma) noexcept
        {
            if (gamma == cachedGamma)
                return;
            if (cachedGamma >= 0.f && std::fabs(gamma - cachedGamma) < 1e-6f)
                return;
            for (int i = 0; i < 256; ++i)
                table[i] = ApplyCoverageGammaSlow(static_cast<BYTE>(i), gamma);
            cachedGamma = gamma;
        }
    };

    thread_local CoverageGammaLut tls_coverageGammaLut;

    struct GrowOnlyByteBuffer
    {
        std::vector<BYTE> data;

        [[nodiscard]] BYTE* EnsureSize(size_t n) noexcept
        {
            if (data.size() < n)
                data.resize(n);
            return data.empty() ? nullptr : data.data();
        }
    };

    thread_local GrowOnlyByteBuffer tls_inkAPlane;
    thread_local GrowOnlyByteBuffer tls_inkGlowM;
    thread_local GrowOnlyByteBuffer tls_inkBlurTmp;

    // =========================================================================
    //  Pixel-level утилиты
    // =========================================================================
    constexpr int  kGlyphMargin = 4;
    constexpr int  kTextMargin = 2;
    constexpr BYTE kCoverageFloor = 4;

    // Coverage из пикселя «белый текст на чёрном A=255 фоне».
    [[nodiscard]] static BYTE CoverageFromWhiteOnBlack(BYTE r, BYTE g, BYTE b) noexcept
    {
        const unsigned lum = (77u * r + 150u * g + 29u * b) >> 8;
        const unsigned mn = std::min({ static_cast<unsigned>(r),
                                            static_cast<unsigned>(g),
                                            static_cast<unsigned>(b) });
        const unsigned blended = (lum + mn + mn) / 3u;
        return static_cast<BYTE>(std::min(255u, blended));
    }

    [[nodiscard]] static BYTE ApplyCoverageGammaSlow(BYTE raw, float gamma) noexcept
    {
        if (raw == 0 || gamma == 1.0f || !(gamma > 0.f)) return raw;
        const double x = static_cast<double>(raw) * (1.0 / 255.0);
        const double y = std::pow(x, static_cast<double>(gamma));
        return static_cast<BYTE>(std::clamp(static_cast<int>(y * 255.0 + 0.5), 0, 255));
    }

    // ── ComputePaintRect ────────────────────────────────────────────────────────
    static RECT ComputePaintRect(
        HDC hdc, int x, int y, UINT options,
        const RECT* lpRect, LPCWSTR lpString, UINT cbCount) noexcept
    {
        RECT paintRect{};
        if (lpRect)
            paintRect = *lpRect;
        else
            GetClipBox(hdc, &paintRect);

        if (x < paintRect.left) paintRect.left = x;
        if (y < paintRect.top)  paintRect.top = y;

        if (lpString && cbCount > 0 && !(options & ETO_GLYPH_INDEX))
        {
            SIZE sz{};
            if (GetTextExtentPoint32W(hdc, lpString, static_cast<int>(cbCount), &sz))
            {
                const UINT ta = GetTextAlign(hdc);
                int textLeft = x, textRight = x + sz.cx;
                if (ta & TA_RIGHT) { textLeft = x - sz.cx; textRight = x; }
                else if (ta & TA_CENTER) { textLeft = x - sz.cx / 2; textRight = x + (sz.cx + 1) / 2; }

                if (textLeft < paintRect.left)             paintRect.left = textLeft;
                if (textRight + kTextMargin > paintRect.right) paintRect.right = textRight + kTextMargin;
                if (y + sz.cy + kTextMargin > paintRect.bottom) paintRect.bottom = y + sz.cy + kTextMargin;
            }
        }

        InflateRect(&paintRect, kGlyphMargin, kGlyphMargin);
        return paintRect;
    }

    // Как ExtTextOutWithAlphaToSession / MultilinePlainGetDc: +1px вокруг lpRect (без kGlyphMargin),
    // чтобы слайдеры InkAlpha на однострочном plain Edit вели себя как на многострочном.
    [[nodiscard]] static RECT ComputePaintRectSessionLikeForEditPlain(
        HDC hdc, int x, int y, const RECT* lpRect) noexcept
    {
        RECT paintRect{};
        if (lpRect)
            paintRect = *lpRect;
        else
        {
            GetClipBox(hdc, &paintRect);
            if (x < paintRect.left) paintRect.left = x;
            if (y < paintRect.top)  paintRect.top = y;
        }
        InflateRect(&paintRect, 1, 1);
        return paintRect;
    }

    // ── ProcessWhiteOnBlackPixels ───────────────────────────────────────────────
    // Преобразует пиксели после white-on-gray рендера в premultiplied BGRA.
    // Фоновые пиксели (coverage ≤ порога) → (0,0,0,0).
    // ──────────────────────────────────────────────────────────────────────────
    static void ProcessWhiteOnBlackPixels(
        RGBQUAD* buffer, int width, int height, int cxRow,
        COLORREF textColor, float coverageGamma) noexcept
    {
        const BYTE dR = GetRValue(textColor);
        const BYTE dG = GetGValue(textColor);
        const BYTE dB = GetBValue(textColor);
        tls_coverageGammaLut.EnsureFor(coverageGamma);
        const BYTE* const covLut = tls_coverageGammaLut.table;

        for (int row = 0; row < height; ++row)
        {
            RGBQUAD* px = buffer + static_cast<size_t>(row) * static_cast<size_t>(cxRow);
            for (int col = 0; col < width; ++col, ++px)
            {
                const BYTE pr = px->rgbRed;
                const BYTE pg = px->rgbGreen;
                const BYTE pb = px->rgbBlue;

                const BYTE rawCov = CoverageFromWhiteOnBlack(pr, pg, pb);
                const BYTE cov = covLut[rawCov];

                if (cov <= kCoverageFloor)
                {
                    px->rgbBlue = px->rgbGreen = px->rgbRed = px->rgbReserved = 0;
                    continue;
                }

                px->rgbRed = static_cast<BYTE>((dR * cov + 127) / 255);
                px->rgbGreen = static_cast<BYTE>((dG * cov + 127) / 255);
                px->rgbBlue = static_cast<BYTE>((dB * cov + 127) / 255);
                px->rgbReserved = cov;
            }
        }
    }

    static void BoxBlurGrayBufferHorizPass(
        const BYTE* src, BYTE* dst, int w, int h, int rad) noexcept
    {
        for (int y = 0; y < h; ++y)
        {
            const BYTE* const row = src + static_cast<size_t>(y) * static_cast<size_t>(w);
            BYTE* const outRow = dst + static_cast<size_t>(y) * static_cast<size_t>(w);

            int sum = 0;
            int cnt = 0;
            for (int dx = -rad; dx <= rad; ++dx)
            {
                const int nx = dx;
                if (nx < 0 || nx >= w)
                    continue;
                sum += row[nx];
                ++cnt;
            }
            outRow[0] = static_cast<BYTE>(sum / std::max(1, cnt));

            for (int x = 1; x < w; ++x)
            {
                const int removeX = x - rad - 1;
                if (removeX >= 0)
                {
                    sum -= row[removeX];
                    --cnt;
                }
                const int addX = x + rad;
                if (addX < w)
                {
                    sum += row[addX];
                    ++cnt;
                }
                outRow[x] = static_cast<BYTE>(sum / std::max(1, cnt));
            }
        }
    }

    static void BoxBlurGrayBufferVertPass(
        const BYTE* src, BYTE* dst, int w, int h, int rad) noexcept
    {
        for (int x = 0; x < w; ++x)
        {
            int sum = 0;
            int cnt = 0;
            for (int dy = -rad; dy <= rad; ++dy)
            {
                const int ny = dy;
                if (ny < 0 || ny >= h)
                    continue;
                sum += src[static_cast<size_t>(ny) * static_cast<size_t>(w) + static_cast<size_t>(x)];
                ++cnt;
            }
            dst[static_cast<size_t>(x)] = static_cast<BYTE>(sum / std::max(1, cnt));

            for (int y = 1; y < h; ++y)
            {
                const int removeY = y - rad - 1;
                if (removeY >= 0)
                {
                    sum -= src[static_cast<size_t>(removeY) * static_cast<size_t>(w) + static_cast<size_t>(x)];
                    --cnt;
                }
                const int addY = y + rad;
                if (addY < h)
                {
                    sum += src[static_cast<size_t>(addY) * static_cast<size_t>(w) + static_cast<size_t>(x)];
                    ++cnt;
                }
                dst[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                    static_cast<BYTE>(sum / std::max(1, cnt));
            }
        }
    }

    static void BoxBlurGrayBuffer(
        BYTE* plane, GrowOnlyByteBuffer& tmpBuf, int w, int h, int rad) noexcept
    {
        if (rad <= 0 || !plane || w <= 0 || h <= 0)
            return;
        rad = std::min(rad, 8);
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
        BYTE* const tmp = tmpBuf.EnsureSize(n);
        if (!tmp)
            return;
        BoxBlurGrayBufferHorizPass(plane, tmp, w, h, rad);
        BoxBlurGrayBufferVertPass(tmp, plane, w, h, rad);
    }

    /// Ореол не рисуем, если RGB текста совпадает с настроенным цветом glow (визуально бессмысленно).
    static bool TextRgbEqualsInkGlowColor(COLORREF textColor, COLORREF glowCol) noexcept
    {
        return (textColor & 0xFFFFFF) == (glowCol & 0xFFFFFF);
    }

    /// Glow только на «нейтральном» фоне подложки: 0, 0xFFFFFF (дефолт GDI),
    /// COLOR_WINDOW, COLOR_BTNFACE. В dark COLOR_WINDOW≠белый, а DUI часто оставляет bk=white —
    /// без 0xFFFFFF ореол гасился при том же DC, что в light проходит через COLOR_WINDOW.
    static bool IsInkGlowAllowedForBackgroundColor(COLORREF bk) noexcept
    {
        const unsigned rgb = static_cast<unsigned>(bk & 0xFFFFFF);
        if (rgb == 0)
            return true;
        if (rgb == 0xFFFFFFu)
            return true;
        if (rgb == static_cast<unsigned>(GetSysColor(COLOR_WINDOW) & 0xFFFFFF))
            return true;
        if (rgb == static_cast<unsigned>(GetSysColor(COLOR_BTNFACE) & 0xFFFFFF))
            return true;
        return false;
    }

    static bool IsInkGlowAllowedForDcBackground(HDC hdc) noexcept
    {
        if (!hdc)
            return false;
        return IsInkGlowAllowedForBackgroundColor(GetBkColor(hdc));
    }

    /// Псевдо-Glow (ExtTextOutWithAlpha): цвет ореола = textColor. Не отключаем по GetBkColor (selection и др.);
    /// глобальный glow (DrawText / #126) и IsInkGlowAllowed* к этой постобработке не относятся.
    /// Радиус ≥150‰…1000‰; inten 0…255 (альфа силы контура).
    static void PostProcessPremulInkBuffer(
        RGBQUAD* buffer, int width, int height, int cxRow, COLORREF textColor,
        const ExtTextOutInkDrawParams& inkDraw,
        GrowOnlyByteBuffer& aPlaneBuf, GrowOnlyByteBuffer& glowMBuf,
        GrowOnlyByteBuffer& blurTmpBuf) noexcept
    {
        if (!buffer || width <= 0 || height <= 0 || cxRow < width)
            return;
        if (!inkDraw.pseudoGlowEnabled)
            return;

        const COLORREF glowColPre = textColor;

        uint32_t r = inkDraw.pseudoGlowRadiusThousandths;
        const uint32_t i = inkDraw.pseudoGlowIntensity;
        if (i == 0u)
            return;

        r = std::clamp(
            r,
            Shared::InkCapsuleQuery::kUiPseudoRadiusMinThousandths,
            Shared::InkCapsuleQuery::kUiPseudoRadiusMaxThousandths);

        // Интенсивность: 0…255, как targetBlendAlpha / глобальный glow alpha.
        const UINT gIntScaled = static_cast<UINT>(std::min<uint32_t>(i, 255u));

        // Радиус 0.15…1 px (тысячные) → box blur 1…2; сила — inten * spanPx
        const float spanPx = std::min(1.f, static_cast<float>(r) / 1000.f);
        const int boxBlurR = std::clamp(static_cast<int>(std::lround(spanPx * 2.f)), 1, 2);
        const float inten = std::clamp(static_cast<float>(gIntScaled) / 255.f, 0.f, 1.f) * spanPx;
        if (inten <= 0.f)
            return;

        const size_t planeSize = static_cast<size_t>(width) * static_cast<size_t>(height);
        BYTE* const aPlane = aPlaneBuf.EnsureSize(planeSize);
        BYTE* const glowM = glowMBuf.EnsureSize(planeSize);
        if (!aPlane || !glowM)
            return;

        for (int row = 0; row < height; ++row)
        {
            const RGBQUAD* src = buffer + static_cast<size_t>(row) * static_cast<size_t>(cxRow);
            for (int col = 0; col < width; ++col)
                aPlane[static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col)] =
                    src[col].rgbReserved;
        }

        memcpy(glowM, aPlane, planeSize);
        BoxBlurGrayBuffer(glowM, blurTmpBuf, width, height, boxBlurR);

        const BYTE gR = GetRValue(glowColPre);
        const BYTE gG = GetGValue(glowColPre);
        const BYTE gB = GetBValue(glowColPre);

        for (int row = 0; row < height; ++row)
        {
            RGBQUAD* px = buffer + static_cast<size_t>(row) * static_cast<size_t>(cxRow);
            for (int col = 0; col < width; ++col, ++px)
            {
                const size_t idx = static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(col);
                const float gA = static_cast<float>(glowM[idx]) / 255.f * inten;
                if (gA <= 0.f) continue;

                const unsigned ga = static_cast<unsigned>(std::min(255.f, gA * 255.f + 0.5f));
                const unsigned gRpm = static_cast<unsigned>(gR) * ga / 255u;
                const unsigned gGpm = static_cast<unsigned>(gG) * ga / 255u;
                const unsigned gBpm = static_cast<unsigned>(gB) * ga / 255u;

                const unsigned tr = px->rgbRed;
                const unsigned tg = px->rgbGreen;
                const unsigned tb = px->rgbBlue;
                const unsigned ta = px->rgbReserved;

                const unsigned invTa = 255u - static_cast<unsigned>(ta);
                const unsigned oR = tr + gRpm * invTa / 255u;
                const unsigned oG = tg + gGpm * invTa / 255u;
                const unsigned oB = tb + gBpm * invTa / 255u;
                unsigned oa = static_cast<unsigned>(ta) + ga * invTa / 255u;
                if (oa > 255u) oa = 255u;

                px->rgbRed = static_cast<BYTE>(std::min(255u, oR));
                px->rgbGreen = static_cast<BYTE>(std::min(255u, oG));
                px->rgbBlue = static_cast<BYTE>(std::min(255u, oB));
                px->rgbReserved = static_cast<BYTE>(oa);
            }
        }
    }

    // =========================================================================
    //  RenderTextToBuffer — single-pass (white on black).
    //
    //  КРИТИЧЕСКИЕ ИСПРАВЛЕНИЯ по сравнению с исходным кодом:
    //   1. scratchOptions = options & ~ETO_OPAQUE  — ETO_OPAQUE в scratch-DC
    //      заливает lpRect цветом BkColor мемDС (обычно унаследованный белый),
    //      что даёт белый прямоугольник → coverage=0 → прозрачный AlphaBlend
    //      СТИРАЕТ glass-фон. Это главная причина двоения.
    //   2. SetBkColor(memoryDC, RGB(bg,bg,bg)) — явная установка фона
    //      в scratch DC, иначе наследуется BkColor от оригинального DC.
    //   3. ETO_OPAQUE обрабатывается отдельно в Edit_WorkExtTextOutCore
    //      через ApplyEditBackgroundRect + g_sessionBlacknessRects.
    // =========================================================================
    static void RenderTextToBuffer(
        HDC memoryDC, RGBQUAD* buffer, int width, int height, int cxRow,
        int x, int y, UINT options, const RECT* lpRect,
        LPCWSTR lpString, UINT cbCount, const INT* lpDx,
        decltype(&::ExtTextOutW) origExtTextOut, COLORREF textColor,
        const ExtTextOutInkDrawParams& inkDraw) noexcept
    {
        if (!buffer || cxRow <= 0 || width <= 0 || height <= 0)
        {
            // Fallback: прямой рендер без alpha-обработки
            origExtTextOut(memoryDC, x, y, options, lpRect, lpString, cbCount, lpDx);
            return;
        }

        // ── Инициализация буфера фоновым цветом ────────────────────────────
        constexpr BYTE bgByte = 0;
        constexpr DWORD kBlackOpaquePixel = 0xFF000000u;
        if (cxRow == width)
        {
            std::fill_n(reinterpret_cast<DWORD*>(buffer),
                static_cast<size_t>(height) * static_cast<size_t>(width),
                kBlackOpaquePixel);
        }
        else
        {
            for (int row = 0; row < height; ++row)
            {
                RGBQUAD* px = buffer + static_cast<size_t>(row) * static_cast<size_t>(cxRow);
                for (int col = 0; col < width; ++col, ++px)
                {
                    px->rgbBlue = bgByte;
                    px->rgbGreen = bgByte;
                    px->rgbRed = bgByte;
                    px->rgbReserved = 255;  // A=255: маска для ProcessWhiteOnBlackPixels
                }
            }
        }

        SetTextColor(memoryDC, RGB(255, 255, 255));     // белый текст
        SetBkMode(memoryDC, TRANSPARENT);
        SetBkColor(memoryDC, RGB(bgByte, bgByte, bgByte)); // явно: не наследовать от oригинального DC

        // ETO_OPAQUE убираем из scratch-опций: он уже обработан в вызывающем коде.
        // Оставляем ETO_GLYPH_INDEX, ETO_CLIPPED и прочие.
        const UINT scratchOptions = options & ~static_cast<UINT>(ETO_OPAQUE);

        // Рендер белого текста на чёрном фоне
        origExtTextOut(memoryDC, x, y, scratchOptions, lpRect, lpString, cbCount, lpDx);
        GdiFlush();

        // Преобразование в premultiplied alpha
        ProcessWhiteOnBlackPixels(buffer, width, height, cxRow, textColor, inkDraw.coverageGamma);
    }

    // =========================================================================
    //  RenderTextDualPass — dual-pass рендер (black + white diff).
    //
    //  Точно воспроизводит механизм GetDrawTextWithGlow (uxtheme ordinal #126):
    //    Проход 1: рендер белого текста на чёрном фоне → bk[]
    //    Проход 2: рендер белого текста на белом фоне → wh[]
    //    Реконструкция per-channel alpha:
    //      aR = 255 - (wh.R - bk.R),  aG = ...  aB = ...
    //      A  = max(aR, aG, aB)       ← ClearType subpixel fringe сохраняется
    //      out = premul(textColor, A)
    //
    //  Результат помещается в blackBuf (первый буфер от BeginBufferedPaint).
    //  Второй буфер — tls_whiteDib (thread_local, переиспользуется).
    //
    //  textColor передаётся явно: SetTextColor(WHITE) будет вызван внутри
    //  до того как вызывающий код сможет его прочитать.
    // =========================================================================
    static void RenderTextDualPass(
        HDC memoryDC, RGBQUAD* blackBuf, int width, int height, int cxRow,
        int x, int y, UINT options, const RECT* lpRect,
        LPCWSTR lpString, UINT cbCount, const INT* lpDx,
        decltype(&::ExtTextOutW) origFn,
        COLORREF textColor,
        const ExtTextOutInkDrawParams& inkDraw) noexcept
    {
        // Пытаемся выделить второй DIB; при неудаче — single-pass
        if (!tls_whiteDib.EnsureSize(memoryDC, width, height))
        {
            DebugLog(DBG_WARNING,
                L"[GlassCore][TextInk] RenderTextDualPass: tls_whiteDib EnsureSize failed %dx%d → single-pass fallback",
                width, height);
            RenderTextToBuffer(memoryDC, blackBuf, width, height, cxRow,
                x, y, options, lpRect, lpString, cbCount, lpDx, origFn, textColor, inkDraw);
            return;
        }

        const UINT scratchOpts = options & ~static_cast<UINT>(ETO_OPAQUE);
        const HFONT srcFont = static_cast<HFONT>(GetCurrentObject(memoryDC, OBJ_FONT));

        // ── Проход 1: чёрный фон (результат в blackBuf) ─────────────────────────
        constexpr DWORD kBlackOpaquePixel = 0xFF000000u;
        if (cxRow == width)
        {
            std::fill_n(reinterpret_cast<DWORD*>(blackBuf),
                static_cast<size_t>(height) * static_cast<size_t>(width),
                kBlackOpaquePixel);
        }
        else
        {
            for (int row = 0; row < height; ++row)
            {
                RGBQUAD* px = blackBuf + static_cast<size_t>(row) * static_cast<size_t>(cxRow);
                for (int col = 0; col < width; ++col, ++px)
                {
                    px->rgbBlue = px->rgbGreen = px->rgbRed = 0;
                    px->rgbReserved = 255;
                }
            }
        }
        SelectObject(memoryDC, srcFont);
        SetTextColor(memoryDC, RGB(255, 255, 255));
        SetBkMode(memoryDC, TRANSPARENT);
        SetBkColor(memoryDC, RGB(0, 0, 0));

        origFn(memoryDC, x, y, scratchOpts, lpRect, lpString, cbCount, lpDx);
        GdiFlush();

        // ── Проход 2: белый фон (результат в tls_whiteDib) ──────────────────
        auto* whitePx = static_cast<RGBQUAD*>(tls_whiteDib.pBits);
        const int ww = tls_whiteDib.width;   // stride второго DIB (top-down, без padding)

        if (ww == width)
        {
            std::fill_n(reinterpret_cast<DWORD*>(whitePx),
                static_cast<size_t>(height) * static_cast<size_t>(width),
                0xFFFFFFFFu);
        }
        else
        {
            const size_t rowBytes = static_cast<size_t>(ww) * sizeof(RGBQUAD);
            for (int row = 0; row < height; ++row)
                memset(whitePx + row * ww, 0xFF, rowBytes);
        }

        SelectObject(tls_whiteDib.hDC, srcFont);
        SetTextColor(tls_whiteDib.hDC, RGB(255, 255, 255));
        SetBkMode(tls_whiteDib.hDC, TRANSPARENT);
        SetBkColor(tls_whiteDib.hDC, RGB(255, 255, 255));

        // tls_whiteDib.hDC — простой CompatibleDC без смещения viewport.
        // BeginBufferedPaint устанавливает SetWindowOrgEx для memoryDC, сохраняя
        // оригинальные координаты x, y. Для whiteDib применяем то же смещение.
        // Поскольку paintRect.left/top уже вычтены из origin BufferedPaint DC,
        // передаём x, y как есть — аналогично origFn(memoryDC,...).
        origFn(tls_whiteDib.hDC, x, y, scratchOpts, lpRect, lpString, cbCount, lpDx);
        GdiFlush();

        // ── Реконструкция per-channel premultiplied alpha ────────────────────
        const BYTE dR = GetRValue(textColor);
        const BYTE dG = GetGValue(textColor);
        const BYTE dB = GetBValue(textColor);
        tls_coverageGammaLut.EnsureFor(inkDraw.coverageGamma);
        const BYTE* const covLut = tls_coverageGammaLut.table;

        for (int row = 0; row < height; ++row)
        {
            RGBQUAD* bk = blackBuf + static_cast<size_t>(row) * static_cast<size_t>(cxRow);
            const RGBQUAD* wh = whitePx + row * ww;

            for (int col = 0; col < width; ++col, ++bk, ++wh)
            {
                // Дифференциальная реконструкция alpha per channel
                const int aB = 255 - (static_cast<int>(wh->rgbBlue) - static_cast<int>(bk->rgbBlue));
                const int aG = 255 - (static_cast<int>(wh->rgbGreen) - static_cast<int>(bk->rgbGreen));
                const int aR = 255 - (static_cast<int>(wh->rgbRed) - static_cast<int>(bk->rgbRed));

                int a = std::max({ aR, aG, aB });
                a = std::clamp(a, 0, 255);
                a = static_cast<int>(covLut[static_cast<BYTE>(a)]);

                if (a <= static_cast<int>(kCoverageFloor))
                {
                    bk->rgbBlue = bk->rgbGreen = bk->rgbRed = bk->rgbReserved = 0;
                    continue;
                }

                bk->rgbRed = static_cast<BYTE>((static_cast<unsigned>(dR) * static_cast<unsigned>(a) + 127u) / 255u);
                bk->rgbGreen = static_cast<BYTE>((static_cast<unsigned>(dG) * static_cast<unsigned>(a) + 127u) / 255u);
                bk->rgbBlue = static_cast<BYTE>((static_cast<unsigned>(dB) * static_cast<unsigned>(a) + 127u) / 255u);
                bk->rgbReserved = static_cast<BYTE>(a);
            }
        }
        // Результат в blackBuf → EndBufferedPaint сделает AlphaBlend
    }

    static void RenderTextInkDispatch(
        bool dualPass,
        HDC memDC, RGBQUAD* buffer, int width, int height, int cxRow,
        int x, int y, UINT options, const RECT* lpRect,
        LPCWSTR lpString, UINT cbCount, const INT* lpDx,
        decltype(&::ExtTextOutW) origFn,
        COLORREF textColor,
        const ExtTextOutInkDrawParams& inkDraw) noexcept
    {
        if (!dualPass)
        {
            RenderTextToBuffer(memDC, buffer, width, height, cxRow,
                x, y, options, lpRect, lpString, cbCount, lpDx, origFn, textColor, inkDraw);
        }
        else
        {
            RenderTextDualPass(memDC, buffer, width, height, cxRow,
                x, y, options, lpRect, lpString, cbCount, lpDx, origFn, textColor, inkDraw);
        }
        PostProcessPremulInkBuffer(buffer, width, height, cxRow, textColor, inkDraw,
            tls_inkAPlane, tls_inkGlowM, tls_inkBlurTmp);
    }

    // ── Выбор режима рендера ────────────────────────────────────────────────────
    // 0=auto, 1=single-pass, 2=dual-pass; для сессии передаётся override
    static bool ShouldUseDualPass(bool sessionDualPassOverride, bool sessionHasOverride) noexcept
    {
        if (sessionHasOverride) return sessionDualPassOverride;
        if (g_ExtTextOutRenderMode == 1) return false;
        if (g_ExtTextOutRenderMode == 2) return true;
        return true;    // auto → dual-pass (многострочный Edit / RichEdit / FullQuality)
    }

    // =========================================================================
    //  DrawThemeContent — обёртка BeginBufferedPaint (без изменений)
    // =========================================================================
    HRESULT DrawThemeContent(
        HDC hdc,
        const RECT& paintRect,
        const RECT* clipRect,
        const RECT* excludeRect,
        DWORD additionalFlags,
        const std::function<void(HDC, HPAINTBUFFER, RGBQUAD*, int)>& callback,
        BYTE alpha,
        bool useBlt,
        bool update,
        bool eraseBufferedPaintBackground)
    {
        if (!hdc || !callback) return E_INVALIDARG;

        BLENDFUNCTION blendFunction{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
        DWORD bpFlags = additionalFlags;
        if (eraseBufferedPaintBackground) bpFlags |= BPPF_ERASE;

        BP_PAINTPARAMS paintParams{
            sizeof(BP_PAINTPARAMS), bpFlags,
            excludeRect,
            !useBlt ? &blendFunction : nullptr
        };

        int savedDC = SaveDC(hdc);
        auto restoreDC = wil::scope_exit([&] { RestoreDC(hdc, savedDC); });

        if (clipRect)
            IntersectClipRect(hdc, clipRect->left, clipRect->top, clipRect->right, clipRect->bottom);
        if (excludeRect)
            ExcludeClipRect(hdc, excludeRect->left, excludeRect->top, excludeRect->right, excludeRect->bottom);

        HDC memoryDC = nullptr;
        HPAINTBUFFER bufferedPaint = BeginBufferedPaint(
            hdc, &paintRect, BPBF_TOPDOWNDIB, &paintParams, &memoryDC);
        if (!bufferedPaint || !memoryDC)
        {
            const int objType = GetObjectType(hdc);
            const int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
            const int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
            DebugLog(DBG_NORMAL,
                L"[GlassCore][TextInk] DrawThemeContent BeginBufferedPaint failed hdc=0x%p objType=%d "
                L"rect=(%ld,%ld,%ld,%ld) bufferedPaint=0x%p memoryDC=0x%p dpi=%d,%d "
                L"(A/B: same font/DPI/theme/window background as GlassSettings when comparing ink vs GDI)",
                hdc, objType,
                static_cast<long>(paintRect.left), static_cast<long>(paintRect.top),
                static_cast<long>(paintRect.right), static_cast<long>(paintRect.bottom),
                bufferedPaint, memoryDC, dpiX, dpiY);
            return E_FAIL;
        }
#if 0
        DebugLog(DBG_NORMAL,
            L"[GlassCore][TextInk] DrawThemeContent BeginBufferedPaint succeeded hdc=0x%p objType=%d "
            L"rect=(%ld,%ld,%ld,%ld) bufferedPaint=0x%p memoryDC=0x%p dpi=%d,%d erase=%d",
            hdc, GetObjectType(hdc),
            static_cast<long>(paintRect.left), static_cast<long>(paintRect.top),
            static_cast<long>(paintRect.right), static_cast<long>(paintRect.bottom),
            bufferedPaint, memoryDC, GetDeviceCaps(hdc, LOGPIXELSX), GetDeviceCaps(hdc, LOGPIXELSY), (bpFlags & BPPF_ERASE));
#endif
        auto selFont = wil::SelectObject(memoryDC, GetCurrentObject(hdc, OBJ_FONT));
        auto selBrush = wil::SelectObject(memoryDC, GetCurrentObject(hdc, OBJ_BRUSH));
        auto selPen = wil::SelectObject(memoryDC, GetCurrentObject(hdc, OBJ_PEN));
        SetTextColor(memoryDC, GetTextColor(hdc));
        SetBkMode(memoryDC, TRANSPARENT);

        RGBQUAD* buffer = nullptr;
        int cxRow = 0;
        GetBufferedPaintBits(bufferedPaint, &buffer, &cxRow);

        if (!eraseBufferedPaintBackground && buffer && cxRow > 0)
        {
            const int w = paintRect.right - paintRect.left;
            const int h = paintRect.bottom - paintRect.top;
            if (h > 0)
            {
                if (cxRow == w)
                {
                    memset(buffer, 0, static_cast<size_t>(h) * static_cast<size_t>(cxRow) * sizeof(RGBQUAD));
                }
                else
                {
                    const size_t rowBytes = static_cast<size_t>(cxRow) * sizeof(RGBQUAD);
                    for (int y = 0; y < h; ++y)
                        memset(buffer + static_cast<size_t>(y) * static_cast<size_t>(cxRow), 0, rowBytes);
                }
            }
        }

        bool shouldUpdate = update;
        auto cleanup = wil::scope_exit([&] {
            EndBufferedPaint(bufferedPaint, shouldUpdate ? TRUE : FALSE);
            });

        try { callback(memoryDC, bufferedPaint, buffer, cxRow); }
        catch (...) { shouldUpdate = false; return wil::ResultFromCaughtException(); }

        return S_OK;
    }

    /// Effective glow color для uxtheme #126 и policy (capsule активной темы ОС).
    static COLORREF ResolveEffectiveInkGlowColorRef() noexcept
    {
        return Shared::InkCapsule::ResolveGlowColorRef(detail::ResolveEffectiveGlobalTextGlow());
    }

    [[nodiscard]] static HWND ResolveInkGlowPolicyHwnd(HDC /*hdc*/) noexcept
    {
        if (HWND h = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd())
            return h;
        return GlassCore::Hooks::UxThemeHooks::GetCurrentMenuHwnd();
    }

    /// Меню/menubar: glow только от мастер-toggle, без spread/registry.
    [[nodiscard]] static bool IsMenuInkGlowDrawContext(HDC hdc) noexcept
    {
        if (GlassCore::Menu::MenuHandler::IsUxHooksEnabled())
            return true;

        const HWND hWnd = ResolveInkGlowPolicyHwnd(hdc);
        if (!hWnd)
            return false;

        if (Shared::MenuUtils::IsPopupMenu(hWnd))
            return true;

        MENUBARINFO mbi{ sizeof(mbi) };
        if (GetMenu(hWnd) && GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi) && !IsRectEmpty(&mbi.rcBar))
            return true;

        return false;
    }

    [[nodiscard]] static bool IsInkGlowSpreadAllowedForDraw(HDC hdc) noexcept
    {
        if (IsMenuInkGlowDrawContext(hdc))
            return IsInkGlowEffectActive();

        if (!IsInkGlowEffectActive())
            return false;
        const HWND hWnd = ResolveInkGlowPolicyHwnd(hdc);
        return GlassCore::TextGlow::IsTextGlowClientRegistered(hWnd);
    }

    /// Фон DC для glow: 0 / COLOR_WINDOW / COLOR_BTNFACE; меню — исключение (selected highlight).
    [[nodiscard]] static bool IsInkGlowBackgroundGlowAllowed(HDC hdc, COLORREF bkColor) noexcept
    {
        if (IsMenuInkGlowDrawContext(hdc))
            return IsInkGlowEffectActive();

        return IsInkGlowAllowedForBackgroundColor(bkColor);
    }

    /// Аргументы uxtheme #126: при выключенном мастере Glow радиус и интенсивность ореола — 0.
    static void GetDrawTextWithGlowApiArgs(COLORREF& glowCol, UINT& gRad, UINT& gInt) noexcept
    {
        const Shared::InkCapsule::GlobalTextGlow& g = detail::ResolveEffectiveGlobalTextGlow();
        glowCol = Shared::InkCapsule::ResolveGlowColorRef(g);
        if (!g.glowEnabled)
        {
            gRad = 0;
            gInt = 0;
            return;
        }
        gRad = static_cast<UINT>(std::min<uint32_t>(g.glowRadiusThousandths / 100u, 255u));
        gInt = static_cast<UINT>(std::min<uint32_t>(
            Shared::InkCapsule::ResolveGlowIntensityThousandths(g) / 100u, 255u));
    }

    bool IsInkGlowPolicyEnabledForEffectiveText(COLORREF effectiveTextColor) noexcept
    {
        const Shared::InkCapsule::GlobalTextGlow& g = detail::ResolveEffectiveGlobalTextGlow();
        if (!g.glowEnabled)
            return false;
        const COLORREF glowCol = Shared::InkCapsule::ResolveGlowColorRef(g);
        return !TextRgbEqualsInkGlowColor(effectiveTextColor, glowCol);
    }

    bool IsInkGlowEffectActive() noexcept
    {
        const Shared::InkCapsule::GlobalTextGlow& g = detail::ResolveEffectiveGlobalTextGlow();
        if (!g.glowEnabled)
            return false;
        const UINT gRad = static_cast<UINT>(std::min<uint32_t>(g.glowRadiusThousandths / 100u, 255u));
        const UINT gInt = static_cast<UINT>(std::min<uint32_t>(
            Shared::InkCapsule::ResolveGlowIntensityThousandths(g) / 100u, 255u));
        return (gRad | gInt) != 0;
    }

    bool IsInkGlowEffectActiveForWindow(HWND hWnd) noexcept
    {
        return IsInkGlowEffectActive()
            && GlassCore::TextGlow::IsTextGlowClientRegistered(hWnd);
    }

    bool IsInkGlowEffectActiveForPaintSession(HWND hWnd) noexcept
    {
        return IsInkGlowEffectActiveForWindow(hWnd);
    }

    /// Если BeginBufferedPaint недоступен, но есть uxtheme #126 — premul-текст в 32bpp DIB и GdiAlphaBlend на hdc.
    static bool TryDrawTextWithGlowOffscreenToHdc(
        HDC hdc,
        LPCWSTR lpchText,
        int cchText,
        LPRECT lprc,
        UINT format,
        COLORREF textColor,
        int& result) noexcept
    {
        auto pfn = GetDrawTextWithGlow();
        if (!pfn || !hdc || !lprc)
            return false;
        if (!IsInkGlowBackgroundGlowAllowed(hdc, GetBkColor(hdc)))
            return false;
        const int w = lprc->right - lprc->left;
        const int h = lprc->bottom - lprc->top;
        if (w <= 0 || h <= 0)
            return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC mem = CreateCompatibleDC(hdc);
        if (!mem)
            return false;
        HBITMAP hbmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbmp)
        {
            DeleteDC(mem);
            return false;
        }
        const HGDIOBJ oldBmp = SelectObject(mem, hbmp);
        if (bits)
            memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

        if (const HFONT hFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT)))
            SelectObject(mem, hFont);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, textColor);

        RECT localRc{ 0, 0, w, h };
        COLORREF glowCol;
        UINT gRad, gInt;
        GetDrawTextWithGlowApiArgs(glowCol, gRad, gInt);
        if (!IsMenuInkGlowDrawContext(hdc))
        {
            if (!IsInkGlowPolicyEnabledForEffectiveText(textColor))
            {
                gRad = 0;
                gInt = 0;
            }
            else if (!IsInkGlowSpreadAllowedForDraw(hdc))
            {
                gRad = 0;
                gInt = 0;
            }
        }

        const HRESULT hr = pfn(mem, lpchText, cchText, &localRc, format,
            static_cast<UINT>(textColor), static_cast<UINT>(glowCol), gRad, gInt, TRUE,
            DrawTextCallback, reinterpret_cast<LPARAM>(&result));
        SelectObject(mem, oldBmp);

        bool ok = false;
        if (SUCCEEDED(hr))
        {
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            ok = GdiAlphaBlend(hdc, lprc->left, lprc->top, w, h, mem, 0, 0, w, h, bf) == TRUE;
        }
        DeleteObject(hbmp);
        DeleteDC(mem);
        return ok;
    }

    // =========================================================================
    //  DrawTextWithAlpha: BeginBufferedPaint + uxtheme #126 (всегда при наличии pfn);
    //  при сбое — offscreen DIB + тот же pfn (premul-текст; ореол опционален по gRad/gInt).
    // =========================================================================
    HRESULT DrawTextWithAlpha(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format,
        int& result, bool transparentBufferBackground)
    {
        if (!hdc || !lprc) return E_INVALIDARG;

        if (s_drawTextWithAlphaDepth > 0)
        {
            if (s_drawTextWithAlphaDepth > 1)
            {
                thread_local DWORD s_reentryLogTick = 0;
                const DWORD nowTick = GetTickCount();
                if (s_reentryLogTick == 0 || (nowTick - s_reentryLogTick) > 2500)
                {
                    s_reentryLogTick = nowTick;
                    DebugLog(DBG_WARNING,
                        L"[GlassCore][RenderText] DrawTextWithAlpha: reentry depth=%u — plain DrawTextW",
                        s_drawTextWithAlphaDepth);
                }
            }
            result = InvokeUser32DrawTextW(hdc, lpchText, cchText, lprc, format);
            return S_OK;
        }

        if (IsDrawTextTrivialForPlainGdi(lpchText, cchText))
        {
            thread_local DWORD s_trivialLogTick = 0;
            const DWORD nowTick = GetTickCount();
            if (s_trivialLogTick == 0 || (nowTick - s_trivialLogTick) > 2500)
            {
                s_trivialLogTick = nowTick;
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][RenderText] DrawTextWithAlpha: trivial no-op cchText=%d",
                    cchText);
            }
            result = 0;
            return S_OK;
        }

        ++s_drawTextWithAlphaDepth;
        auto popDepth = wil::scope_exit([]() noexcept { --s_drawTextWithAlphaDepth; });

        const bool eraseBufferedPaintBackground = !transparentBufferBackground;
        COLORREF textColor = GetTextColor(hdc);
        const COLORREF bkGlowPolicy = GetBkColor(hdc);
        const HWND policyHwnd = ResolveInkGlowPolicyHwnd(hdc);
        if ((GlassCore::TextGlow::IsTextGlowClientRegistered(policyHwnd) || IsMenuInkGlowDrawContext(hdc))
            && (textColor & 0xFFFFFF) == 0)
        {
            textColor = GetSysColor(COLOR_WINDOWTEXT);
        }
#if 0
        DebugLog(DBG_NORMAL,
            L"[GlassCore][RenderText] DrawTextWithAlpha entry hdc=0x%p textColor=0x%06X bkColor=0x%06X "
            "rect=(%ld,%ld,%ld,%ld) format=0x%X lpchText=%s",
            hdc, static_cast<unsigned>(textColor & 0xFFFFFF), static_cast<unsigned>(bkGlowPolicy & 0xFFFFFF),
            static_cast<long>(lprc->left), static_cast<long>(lprc->top),
            static_cast<long>(lprc->right), static_cast<long>(lprc->bottom),
            format, lpchText);
#endif
        auto primaryCallback = [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int) -> void
            {
                auto pfn = GetDrawTextWithGlow();
                if (!pfn)
                {
                    result = InvokeUser32DrawTextW(memoryDC, lpchText, cchText, lprc, format);
                    return;
                }
                COLORREF glowCol;
                UINT gRad, gInt;
                GetDrawTextWithGlowApiArgs(glowCol, gRad, gInt);
                const bool allowBgGlow = IsInkGlowBackgroundGlowAllowed(hdc, bkGlowPolicy);
                if (!IsMenuInkGlowDrawContext(hdc))
                {
                    if (!IsInkGlowPolicyEnabledForEffectiveText(textColor) || !allowBgGlow)
                    {
                        gRad = 0;
                        gInt = 0;
                    }
                    else if (!IsInkGlowSpreadAllowedForDraw(hdc))
                    {
                        gRad = 0;
                        gInt = 0;
                    }
                }
                THROW_IF_FAILED(pfn(memoryDC, lpchText, cchText, lprc, format,
                    static_cast<UINT>(textColor), static_cast<UINT>(glowCol), gRad, gInt, TRUE,
                    DrawTextCallback, reinterpret_cast<LPARAM>(&result)));
            };

        auto fallbackCallback = [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD*, int) -> void
            {
                DTTOPTS opts{};
                opts.dwSize = sizeof(opts);
                opts.dwFlags = DTT_TEXTCOLOR | DTT_COMPOSITED | DTT_CALLBACK;
                opts.crText = textColor;
                opts.pfnDrawTextCallback = DrawTextCallback;
                opts.lParam = reinterpret_cast<LPARAM>(&result);
                wil::unique_htheme hTheme{ OpenThemeData(nullptr, L"Composited::Window") };
                THROW_HR_IF_NULL(E_FAIL, hTheme);
                THROW_IF_FAILED(DrawThemeTextEx(hTheme.get(), memoryDC, 0, 0,
                    lpchText, cchText, format, lprc, &opts));
            };

        const HRESULT hrPrimary = DrawThemeContent(
            hdc, *lprc, nullptr, nullptr, 0, primaryCallback, 0xFF, false, true, eraseBufferedPaintBackground);
        if (FAILED(hrPrimary))
        {
            if (GetDrawTextWithGlow()
                && TryDrawTextWithGlowOffscreenToHdc(hdc, lpchText, cchText, lprc, format, textColor, result))
                return S_OK;

            const HRESULT hrFb = DrawThemeContent(
                hdc, *lprc, nullptr, nullptr, 0, fallbackCallback, 0xFF, false, true, eraseBufferedPaintBackground);
            if (FAILED(hrFb))
            {
                thread_local DWORD    s_fbWindowStart = 0;
                thread_local unsigned s_fbInWindow = 0;
                constexpr DWORD kFbLogIntervalMs = 2500;
                const DWORD nowTick = GetTickCount();
                if (s_fbWindowStart == 0 || (nowTick - s_fbWindowStart) > kFbLogIntervalMs)
                {
                    if (s_fbInWindow > 1)
                        DebugLog(DBG_NORMAL, L"[GlassCore][RenderText] DrawTextWithAlpha: тот же fallback ещё %u раз", s_fbInWindow - 1);
                    s_fbWindowStart = nowTick; s_fbInWindow = 0;
                }
                ++s_fbInWindow;
                if (s_fbInWindow == 1)
                    DebugLog(DBG_WARNING, L"[GlassCore][RenderText] DrawTextWithAlpha: buffered paint недоступен — DrawTextW без alpha");
                result = InvokeUser32DrawTextW(hdc, lpchText, cchText, lprc, format);
            }
        }
        return S_OK;
    }

    namespace
    {
        // Параметры ink в момент входа в конвейер отрисовки (per-call inkCapsule / inkDraw).
        void LogTextInkDrawEffective(
            const wchar_t* /*path*/,
            HWND /*hwndOpt*/,
            int /*dualPass*/,
            unsigned /*modeTag*/,
            UINT /*cbCount*/) noexcept
        {
#if 0
            if (cbCount < 2u)
                return;
            DebugLog(DBG_NORMAL,
                L"[GlassCore][TextInk] DrawEffective path=%s hwnd=0x%p dualPass=%d modeTag=%u c=%u "
                L"rm=%u gamma=%.4f glowR=%u tblend=%u",
                path,
                hwndOpt,
                dualPass,
                modeTag,
                cbCount,
                static_cast<unsigned>(g_ExtTextOutRenderMode),
                g_ExtTextOutCoverageGamma,
                static_cast<unsigned>(g_ExtTextOutGlowRadiusThousandths),
                static_cast<unsigned>(g_ExtTextOutTargetBlendAlpha));
#endif
        }
    } // namespace

    // =========================================================================
    //  ExtTextOutWithAlpha — per-call рендеринг (для однострочного Edit, RichEdit)
    //  Параметры ink — из inkCapsule (per-call) или GetRuntimeInkCapsuleProfile(dualPass).
    // =========================================================================
    HRESULT ExtTextOutWithAlpha(
        HDC hdc, int x, int y, UINT options,
        const RECT* lpRect, LPCWSTR lpString, UINT cbCount, const INT* lpDx,
        decltype(&::ExtTextOutW) origExtTextOut,
        ExtTextOutAlphaMode mode,
        HWND plainEditHwnd,
        const Shared::InkCapsule::ProfilePayload* inkCapsule,
        const bool* dualPassOverride)
    {
        if (!hdc || !origExtTextOut)
        {
#if 0
            DebugLog(DBG_NORMAL,
                L"[GlassCore][TextInk] ExtTextOutWithAlpha earlyOut=E_INVALIDARG mode=%u c=%u hdc=0x%p orig=%p",
                static_cast<unsigned>(mode), cbCount, hdc,
                reinterpret_cast<const void*>(origExtTextOut));
#endif
            return E_INVALIDARG;
        }

        HRESULT bypassHr = E_FAIL;
        if (TryExtTextOutPlainDuringDrawTextWithAlpha(
                hdc, x, y, options, lpRect, lpString, cbCount, lpDx, origExtTextOut, bypassHr))
            return bypassHr;

        const COLORREF textColor = GetTextColor(hdc);
        RECT paintRect = (mode == ExtTextOutAlphaMode::EditSingleLinePlain)
            ? ComputePaintRectSessionLikeForEditPlain(hdc, x, y, lpRect)
            : ComputePaintRect(hdc, x, y, options, lpRect, lpString, cbCount);
        if (IsRectEmpty(&paintRect))
        {
#if 0
            const HWND w = plainEditHwnd ? plainEditHwnd : WindowFromDC(hdc);
            DebugLog(DBG_NORMAL,
                L"[GlassCore][TextInk] ExtTextOutWithAlpha earlyOut=E_FAIL emptyPaintRect mode=%u c=%u hwnd=0x%p",
                static_cast<unsigned>(mode), cbCount, w);
#endif
            return E_FAIL;
        }

        // Clip по текущему clip-региону: избегаем inflate за его пределы
        RECT clipBox{};
        if (GetClipBox(hdc, &clipBox) != NULLREGION)
            IntersectRect(&paintRect, &paintRect, &clipBox);
        if (IsRectEmpty(&paintRect))
        {
#if 0
            const HWND w = plainEditHwnd ? plainEditHwnd : WindowFromDC(hdc);
            // E_FAIL заставляет EditHooks вызвать origW (стандартный ExtTextOut).
            DebugLog(DBG_NORMAL,
                L"[GlassCore][TextInk] ExtTextOutWithAlpha earlyOut=E_FAIL emptyAfterClip mode=%u c=%u hwnd=0x%p "
                L"clip=(%ld,%ld,%ld,%ld) → origW fallback",
                static_cast<unsigned>(mode), cbCount, w,
                static_cast<long>(clipBox.left), static_cast<long>(clipBox.top),
                static_cast<long>(clipBox.right), static_cast<long>(clipBox.bottom));
#endif
            return E_FAIL;
        }

        // Интерактивный GetDC — single-pass; однострочный plain — PlainEditUseDualPassSingleLinePlain (как multiline без HSCROLL, не ветка HSCROLL).
        // Режим рендера для PlainEditUseDualPassSingleLinePlain должен совпадать с EditHooks (в т.ч. превью капсулы GlassSettings).
        // HWND: из EditHooks передаётся sessionHwnd; иначе WindowFromDC (совпадает с multiline GetDC при обычном DC окна).
        HWND editHwndForPlain = nullptr;
        if (mode == ExtTextOutAlphaMode::EditSingleLinePlain)
            editHwndForPlain = plainEditHwnd ? plainEditHwnd : WindowFromDC(hdc);
        bool dualPass = false;
        if (dualPassOverride)
            dualPass = *dualPassOverride;
        else if (mode == ExtTextOutAlphaMode::EditGetDcInteractive)
            dualPass = false;
        else if (mode == ExtTextOutAlphaMode::EditSingleLinePlain)
        {
            BYTE rm = g_ExtTextOutRenderMode;
            dualPass = PlainEditUseDualPassSingleLinePlain(editHwndForPlain, rm);
        }
        else
            dualPass = ShouldUseDualPass(false, false);

        {
            HWND logHwnd = nullptr;
            if (mode == ExtTextOutAlphaMode::EditSingleLinePlain)
                logHwnd = editHwndForPlain;
            else if (hdc)
                logHwnd = WindowFromDC(hdc);
            LogTextInkDrawEffective(L"ExtTextOutWithAlpha", logHwnd, dualPass ? 1 : 0,
                static_cast<unsigned>(mode), cbCount);
        }

        const Shared::InkCapsule::ProfilePayload& resolvedCapsule = inkCapsule
            ? *inkCapsule
            : GetRuntimeInkCapsuleProfile(dualPass);
        const ExtTextOutInkDrawParams inkDraw =
            BuildInkDrawParamsFromProfile(resolvedCapsule, dualPass);

        auto callback = [&](HDC memDC, HPAINTBUFFER, RGBQUAD* buffer, int cxRow) -> void
            {
                const int w = paintRect.right - paintRect.left;
                const int h = paintRect.bottom - paintRect.top;
                RenderTextInkDispatch(dualPass, memDC, buffer, w, h, cxRow,
                    x, y, options, lpRect, lpString, cbCount, lpDx, origExtTextOut, textColor, inkDraw);
            };

        return DrawThemeContent(
            hdc, paintRect, nullptr, nullptr, 0, callback,
            inkDraw.targetBlendAlpha, false, true, false);
    }

    HRESULT ExtTextOutWithAlphaMultilinePlainGetDc(
        HDC hdc, int x, int y, UINT options,
        const RECT* lpRect, LPCWSTR lpString, UINT cbCount, const INT* lpDx,
        decltype(&::ExtTextOutW) origExtTextOut,
        bool sessionLikeUseDualPass,
        const Shared::InkCapsule::ProfilePayload* inkCapsule)
    {
        if (!hdc || !origExtTextOut) return E_INVALIDARG;

        HRESULT bypassHr = E_FAIL;
        if (TryExtTextOutPlainDuringDrawTextWithAlpha(
                hdc, x, y, options, lpRect, lpString, cbCount, lpDx, origExtTextOut, bypassHr))
            return bypassHr;

        const COLORREF textColor = GetTextColor(hdc);

        RECT paintRect{};
        if (lpRect)
            paintRect = *lpRect;
        else
        {
            GetClipBox(hdc, &paintRect);
            if (x < paintRect.left) paintRect.left = x;
            if (y < paintRect.top)  paintRect.top = y;
        }

        if (IsRectEmpty(&paintRect)) return E_FAIL;

        RECT clipBox{};
        if (GetClipBox(hdc, &clipBox) != NULLREGION)
            IntersectRect(&paintRect, &paintRect, &clipBox);
        if (IsRectEmpty(&paintRect))
            return E_FAIL; // как ExtTextOutWithAlpha: не S_OK без отрисовки — иначе нет origW fallback

        const bool dualPass = ShouldUseDualPass(sessionLikeUseDualPass, true);

        LogTextInkDrawEffective(L"MultilinePlainGetDc", hdc ? WindowFromDC(hdc) : nullptr,
            dualPass ? 1 : 0, 4u, cbCount);

        const Shared::InkCapsule::ProfilePayload& resolvedCapsule = inkCapsule
            ? *inkCapsule
            : GetRuntimeInkCapsuleProfile(dualPass);
        const ExtTextOutInkDrawParams inkDraw =
            BuildInkDrawParamsFromProfile(resolvedCapsule, dualPass);

        auto callback = [&](HDC memDC, HPAINTBUFFER, RGBQUAD* buffer, int cxRow) -> void
            {
                const int w = paintRect.right - paintRect.left;
                const int h = paintRect.bottom - paintRect.top;
                RenderTextInkDispatch(dualPass, memDC, buffer, w, h, cxRow,
                    x, y, options, lpRect, lpString, cbCount, lpDx, origExtTextOut, textColor, inkDraw);
            };

        return DrawThemeContent(
            hdc, paintRect, nullptr, nullptr, 0, callback,
            inkDraw.targetBlendAlpha, false, true, false);
    }

    // =========================================================================
    //  ExtTextOutWithAlphaToSession — рендер в сессионный DIB.
    //
    //  Изменения:
    //  - Принимает TextSessionBuffer::useDualPass (из EditHooks).
    //  - Для session-пути kGlyphMargin НЕ применяется к paint rect:
    //    inflate приводит к перекрытию соседних сегментов и двойному
    //    AlphaBlend поверх уже записанных пикселей DIB → двоение.
    //  - ETO_OPAQUE уже обработан в Edit_WorkExtTextOutCore; здесь
    //    удалён из options на уровне вызова (drawOptions = options & ~ETO_OPAQUE).
    // =========================================================================
    HRESULT ExtTextOutWithAlphaToSession(
        HDC sessionDC,
        int x, int y,
        UINT options,
        const RECT* lpRect,
        LPCWSTR lpString, UINT cbCount, const INT* lpDx,
        decltype(&::ExtTextOutW) origExtTextOut,
        const TextSessionBuffer& session,
        const Shared::InkCapsule::ProfilePayload* inkCapsule)
    {
        if (!sessionDC || !origExtTextOut || !session.bits) return E_INVALIDARG;

        HRESULT bypassHr = E_FAIL;
        if (TryExtTextOutPlainDuringDrawTextWithAlpha(
                sessionDC, x, y, options, lpRect, lpString, cbCount, lpDx, origExtTextOut, bypassHr))
            return bypassHr;

        const COLORREF textColor = GetTextColor(sessionDC);

        // Для сессионного пути: paintRect без inflate (в отличие от per-call пути).
        // inflate нужен для BeginBufferedPaint (чтобы subpixel fringe не обрезался),
        // но в сессионном DIB границы задаются размером буфера, а не paintRect.
        RECT paintRect{};
        if (lpRect)
            paintRect = *lpRect;
        else
        {
            GetClipBox(sessionDC, &paintRect);
            if (x < paintRect.left) paintRect.left = x;
            if (y < paintRect.top)  paintRect.top = y;
        }

        // Небольшой запас только для subpixel fringe (1px, не 4px)
        InflateRect(&paintRect, 1, 1);

        // Ограничение границами сессионного буфера
        if (paintRect.left < 0)               paintRect.left = 0;
        if (paintRect.top < 0)               paintRect.top = 0;
        if (paintRect.right > session.width)    paintRect.right = session.width;
        if (paintRect.bottom > session.height)   paintRect.bottom = session.height;
        if (IsRectEmpty(&paintRect)) return S_OK;

        const int width = paintRect.right - paintRect.left;
        const int height = paintRect.bottom - paintRect.top;

        const bool dualPass = ShouldUseDualPass(session.useDualPass, session.hasDualPassOverride);

        LogTextInkDrawEffective(L"ToSession", sessionDC ? WindowFromDC(sessionDC) : nullptr,
            dualPass ? 1 : 0, 3u, cbCount);

        const Shared::InkCapsule::ProfilePayload& resolvedCapsule = inkCapsule
            ? *inkCapsule
            : GetRuntimeInkCapsuleProfile(dualPass);
        const ExtTextOutInkDrawParams inkDraw =
            BuildInkDrawParamsFromProfile(resolvedCapsule, dualPass);

        auto callback = [&](HDC memoryDC, HPAINTBUFFER, RGBQUAD* innerBuf, int cxRow) -> void
            {
                RenderTextInkDispatch(dualPass, memoryDC, innerBuf, width, height, cxRow,
                    x, y, options, lpRect, lpString, cbCount, lpDx, origExtTextOut, textColor, inkDraw);

                if (!innerBuf || cxRow <= 0) return;

                // Compositing в сессионный DIB (premultiplied alpha over)
                for (int row = 0; row < height; ++row)
                {
                    const RGBQUAD* src = innerBuf + static_cast<size_t>(row) * static_cast<size_t>(cxRow);
                    const int dstRow = paintRect.top + row;
                    if (dstRow < 0 || dstRow >= session.height) continue;
                    RGBQUAD* dst = session.bits + dstRow * session.width + paintRect.left;

                    for (int col = 0; col < width; ++col, ++src, ++dst)
                    {
                        if (src->rgbReserved == 0) continue;

                        const unsigned sa = src->rgbReserved;
                        const unsigned sr = src->rgbRed;
                        const unsigned sg = src->rgbGreen;
                        const unsigned sb = src->rgbBlue;

                        if (dst->rgbReserved == 0 || sa >= 255)
                        {
                            dst->rgbRed = static_cast<BYTE>(sr);
                            dst->rgbGreen = static_cast<BYTE>(sg);
                            dst->rgbBlue = static_cast<BYTE>(sb);
                            dst->rgbReserved = static_cast<BYTE>(sa);
                        }
                        else
                        {
                            const unsigned invA = 255u - sa;
                            dst->rgbRed = static_cast<BYTE>(sr + (dst->rgbRed * invA + 127u) / 255u);
                            dst->rgbGreen = static_cast<BYTE>(sg + (dst->rgbGreen * invA + 127u) / 255u);
                            dst->rgbBlue = static_cast<BYTE>(sb + (dst->rgbBlue * invA + 127u) / 255u);
                            dst->rgbReserved = static_cast<BYTE>(sa + (dst->rgbReserved * invA + 127u) / 255u);
                        }
                    }
                }
            };

        return DrawThemeContent(
            sessionDC, paintRect, nullptr, nullptr, 0, callback,
            0xFF, false, false, false);
    }

    void SetRenderTextPreviewGlobalGlowBytes(const void* data, size_t byteCount) noexcept
    {
        if (!data)
            return;
        Shared::InkCapsule::GlobalTextGlow g{};
        if (byteCount >= sizeof(Shared::InkCapsule::GlobalTextGlow))
            Shared::InkCapsule::DeserializeGlobalTextGlow(g, data, byteCount);
        else
            return;
        detail::ClampGlobalGlowInPlace(g);
        detail::MutableRuntimeGlowForRegistryTheme() = g;
        detail::SyncEffectiveGlowToFlatGlobals();
        detail::LogGlobalGlowApplied(L"PreviewGlobalGlow RAM", g);
    }

    namespace
    {
        [[nodiscard]] static Shared::InkCapsule::ProfilePayload BuildPreviewInkProfileFromRequest(
            const GlassCoreTextPreviewRequest* request,
            bool dualPass) noexcept
        {
            using GlassInkProfilePayload = Shared::InkCapsule::ProfilePayload;
            if (!request)
                return GetRuntimeInkCapsuleProfile(dualPass);

            const uint32_t rm = request->ink
                ? std::min<uint32_t>(request->ink->renderMode, 2u)
                : static_cast<uint32_t>(g_ExtTextOutRenderMode);

            if (request->inkProfile)
            {
                GlassInkProfilePayload profile = *request->inkProfile;
                Shared::InkCapsuleQuery::ClampProfile(profile);
                return profile;
            }

            if (!request->ink)
                return GetRuntimeInkCapsuleProfile(dualPass);

            GlassInkProfilePayload overlay = rm == 0u
                ? detail::kProfileAutoDual
                : (rm == 1u ? detail::g_userProfileSingle : detail::g_userProfileDual);
            overlay.coverageGammaThousandths = static_cast<uint32_t>(std::clamp(
                static_cast<int>(request->ink->coverageGamma * 1000.f + 0.5f),
                static_cast<int>(Shared::InkCapsuleQuery::kUiGammaMinThousandths),
                static_cast<int>(Shared::InkCapsuleQuery::kUiGammaMaxThousandths)));
            overlay.targetBlendAlpha =
                std::min<uint32_t>(request->ink->targetBlendAlpha, 255u);
            overlay.pseudoGlowEnabled = request->ink->pseudoGlowEnabled != 0 ? 1u : 0u;
            overlay.pseudoGlowRadiusThousandths = request->ink->pseudoGlowRadiusThousandths;
            overlay.pseudoGlowIntensity = request->ink->pseudoGlowIntensity;
            Shared::InkCapsuleQuery::ClampProfile(overlay);
            return overlay;
        }

        [[nodiscard]] static decltype(&::ExtTextOutW) GetPreviewOrigExtTextOutW() noexcept
        {
            static const auto pfn = reinterpret_cast<decltype(&::ExtTextOutW)>(
                GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "ExtTextOutW"));
            return pfn ? pfn : ::ExtTextOutW;
        }

        struct PreviewLineSpan
        {
            const WCHAR* start;
            UINT         len;
        };

        [[nodiscard]] static bool PreviewMeasureTextWidth(
            HDC hdc,
            const WCHAR* text,
            UINT         cch,
            int&         outWidth) noexcept
        {
            SIZE extent{};
            if (!GetTextExtentPoint32W(hdc, text, cch, &extent))
                return false;

            outWidth = static_cast<int>(extent.cx);
            return true;
        }

        static void PreviewAppendLineSpan(
            std::vector<PreviewLineSpan>& lines,
            const WCHAR*                  start,
            UINT                          len) noexcept
        {
            if (len == 0)
                return;

            lines.push_back({ start, len });
        }

        static void PreviewWrapLongToken(
            HDC                           hdc,
            std::vector<PreviewLineSpan>& lines,
            const WCHAR*                  tokenStart,
            UINT                          tokenLen,
            int                           maxWidth) noexcept
        {
            UINT offset = 0;
            while (offset < tokenLen)
            {
                UINT chunkLen = 1;
                int  chunkWidth = 0;
                while (offset + chunkLen <= tokenLen)
                {
                    if (!PreviewMeasureTextWidth(
                            hdc,
                            tokenStart + offset,
                            chunkLen,
                            chunkWidth))
                    {
                        return;
                    }

                    if (chunkWidth > maxWidth && chunkLen > 1)
                    {
                        --chunkLen;
                        (void)PreviewMeasureTextWidth(
                            hdc,
                            tokenStart + offset,
                            chunkLen,
                            chunkWidth);
                        break;
                    }

                    if (offset + chunkLen == tokenLen)
                        break;

                    ++chunkLen;
                }

                PreviewAppendLineSpan(lines, tokenStart + offset, chunkLen);
                offset += chunkLen;
            }
        }

        static void PreviewWrapParagraph(
            HDC                           hdc,
            std::vector<PreviewLineSpan>& lines,
            const WCHAR*                  paraStart,
            UINT                          paraLen,
            int                           maxWidth) noexcept
        {
            if (paraLen == 0 || maxWidth <= 0)
                return;

            const WCHAR* cursor = paraStart;
            const WCHAR* const paraEnd = paraStart + paraLen;

            const WCHAR* lineStart = cursor;
            UINT         lineLen = 0;

            auto flushLine = [&]() noexcept
            {
                while (lineLen > 0 && lineStart[0] == L' ')
                {
                    ++lineStart;
                    --lineLen;
                }

                while (lineLen > 0 && lineStart[lineLen - 1] == L' ')
                    --lineLen;

                PreviewAppendLineSpan(lines, lineStart, lineLen);
                lineStart = cursor;
                lineLen = 0;
            };

            while (cursor < paraEnd)
            {
                while (cursor < paraEnd && *cursor == L' ')
                    ++cursor;

                if (cursor >= paraEnd)
                    break;

                const WCHAR* tokenStart = cursor;
                while (cursor < paraEnd && *cursor != L' ')
                    ++cursor;

                const UINT tokenLen = static_cast<UINT>(cursor - tokenStart);
                if (tokenLen == 0)
                    continue;

                int tokenWidth = 0;
                if (!PreviewMeasureTextWidth(hdc, tokenStart, tokenLen, tokenWidth))
                    continue;

                if (tokenWidth > maxWidth)
                {
                    if (lineLen > 0)
                        flushLine();

                    PreviewWrapLongToken(hdc, lines, tokenStart, tokenLen, maxWidth);
                    lineStart = cursor;
                    lineLen = 0;
                    continue;
                }

                const UINT candidateLen = lineLen + (lineLen > 0 ? 1u : 0u) + tokenLen;
                int        candidateWidth = 0;
                if (!PreviewMeasureTextWidth(hdc, lineStart, candidateLen, candidateWidth))
                    continue;

                if (lineLen > 0 && candidateWidth > maxWidth)
                {
                    flushLine();
                    lineStart = tokenStart;
                    lineLen = tokenLen;
                    continue;
                }

                lineLen = candidateLen;
            }

            if (lineLen > 0)
                flushLine();
        }

        void PaintPreviewTextLines(
            HDC hdc,
            const WCHAR* text,
            UINT cch,
            const RECT& bounds,
            ExtTextOutAlphaMode mode,
            const Shared::InkCapsule::ProfilePayload* inkCapsule,
            bool previewDualPass) noexcept
        {
            if (!text || cch == 0 || IsRectEmpty(&bounds))
                return;

            const auto orig = GetPreviewOrigExtTextOutW();
            TEXTMETRICW tm{};
            if (!GetTextMetricsW(hdc, &tm))
                return;

            const int lineHeight = tm.tmHeight + tm.tmExternalLeading;
            if (lineHeight <= 0)
                return;

            constexpr int kHorizontalInsetPx = 2;
            const int     boundsWidth = bounds.right - bounds.left;
            const int     boundsHeight = bounds.bottom - bounds.top;
            const int     maxLineWidth = std::max(1, boundsWidth - (kHorizontalInsetPx * 2));

            std::vector<PreviewLineSpan> lines;
            lines.reserve(16);

            const WCHAR* cursor = text;
            const WCHAR* const end = text + cch;
            while (cursor < end)
            {
                const WCHAR* lineEnd = cursor;
                while (lineEnd < end && *lineEnd != L'\n' && *lineEnd != L'\r')
                    ++lineEnd;

                const UINT paraLen = static_cast<UINT>(lineEnd - cursor);
                if (paraLen > 0)
                    PreviewWrapParagraph(hdc, lines, cursor, paraLen, maxLineWidth);

                cursor = lineEnd;
                while (cursor < end && (*cursor == L'\n' || *cursor == L'\r'))
                    ++cursor;
            }

            if (lines.empty())
                return;

            const int blockHeight = static_cast<int>(lines.size()) * lineHeight;
            int       y = bounds.top + std::max(0, (boundsHeight - blockHeight) / 2);

            const RECT lineClipBounds{
                bounds.left + kHorizontalInsetPx,
                bounds.top,
                bounds.right - kHorizontalInsetPx,
                bounds.bottom,
            };

            for (const PreviewLineSpan& line : lines)
            {
                if (y + lineHeight > bounds.bottom)
                    break;

                SIZE extent{};
                if (!GetTextExtentPoint32W(hdc, line.start, line.len, &extent))
                {
                    y += lineHeight;
                    continue;
                }

                const int innerWidth = lineClipBounds.right - lineClipBounds.left;
                const int x = lineClipBounds.left
                    + std::max(0, (innerWidth - static_cast<int>(extent.cx)) / 2);
                (void)ExtTextOutWithAlpha(
                    hdc,
                    x,
                    y,
                    0,
                    &lineClipBounds,
                    line.start,
                    line.len,
                    nullptr,
                    orig,
                    mode,
                    nullptr,
                    inkCapsule,
                    &previewDualPass);

                y += lineHeight;
            }
        }
    } // namespace

    HRESULT PaintTextPreviewToBuffer(
        BYTE* bgraPremul,
        int   width,
        int   height,
        int   stride,
        const GlassCoreTextPreviewRequest* request,
        bool  interactiveDuringGesture) noexcept
    {
        if (!bgraPremul || !request || width <= 0 || height <= 0 || stride < width * 4)
            return E_INVALIDARG;
        if (!request->sampleText || request->sampleCch == 0)
            return E_INVALIDARG;

        RECT paintRect = request->paintRect;
        if (IsRectEmpty(&paintRect))
        {
            paintRect.left = 0;
            paintRect.top = 0;
            paintRect.right = width;
            paintRect.bottom = height;
        }

        paintRect.left = std::max<LONG>(paintRect.left, 0);
        paintRect.top = std::max<LONG>(paintRect.top, 0);
        paintRect.right = std::min<LONG>(paintRect.right, width);
        paintRect.bottom = std::min<LONG>(paintRect.bottom, height);
        if (IsRectEmpty(&paintRect))
            return E_INVALIDARG;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = nullptr;
        HDC hdcScreen = GetDC(nullptr);
        if (!hdcScreen)
            return E_OUTOFMEMORY;
        const auto releaseScreenDc = wil::scope_exit([&]() noexcept { ReleaseDC(nullptr, hdcScreen); });

        wil::unique_hbitmap dib(CreateDIBSection(
            hdcScreen, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0));
        if (!dib || !dibBits)
            return E_OUTOFMEMORY;

        std::memset(dibBits, 0, static_cast<size_t>(stride) * static_cast<size_t>(height));

        wil::unique_hdc memDC(CreateCompatibleDC(nullptr));
        if (!memDC)
            return E_OUTOFMEMORY;

        const HGDIOBJ oldBmp = SelectObject(memDC.get(), dib.get());
        const auto restoreBmp = wil::scope_exit([&]() noexcept
            {
                if (oldBmp)
                    SelectObject(memDC.get(), oldBmp);
            });

        wil::unique_hfont font(CreateFontIndirectW(&request->font));
        if (!font)
            return E_OUTOFMEMORY;

        const HGDIOBJ oldFont = SelectObject(memDC.get(), font.get());
        const auto restoreFont = wil::scope_exit([&]() noexcept
            {
                if (oldFont)
                    SelectObject(memDC.get(), oldFont);
            });

        SetBkMode(memDC.get(), TRANSPARENT);
        SetTextColor(memDC.get(), GetSysColor(COLOR_WINDOWTEXT));

        const uint32_t previewRenderMode = request->ink
            ? std::min<uint32_t>(request->ink->renderMode, 2u)
            : static_cast<uint32_t>(g_ExtTextOutRenderMode);
        const bool allowInteractiveSinglePass =
            interactiveDuringGesture && previewRenderMode == 1u;
        const ExtTextOutAlphaMode mode = allowInteractiveSinglePass
            ? ExtTextOutAlphaMode::EditGetDcInteractive
            : ExtTextOutAlphaMode::FullQuality;

        const bool previewDualPass = mode != ExtTextOutAlphaMode::EditGetDcInteractive
            && (previewRenderMode != 1u);
        const Shared::InkCapsule::ProfilePayload previewProfile =
            BuildPreviewInkProfileFromRequest(request, previewDualPass);
        PaintPreviewTextLines(
            memDC.get(),
            request->sampleText,
            request->sampleCch,
            paintRect,
            mode,
            &previewProfile,
            previewDualPass);

        const auto* src = static_cast<const BYTE*>(dibBits);
        for (int row = 0; row < height; ++row)
        {
            std::memcpy(
                bgraPremul + static_cast<size_t>(row) * static_cast<size_t>(stride),
                src + static_cast<size_t>(row) * static_cast<size_t>(width) * 4u,
                static_cast<size_t>(width) * 4u);
        }

        auto previewBufferHasVisibleAlpha = [](const BYTE* pixels, int w, int h, int rowStride) noexcept -> bool
            {
                if (!pixels || w <= 0 || h <= 0 || rowStride < w * 4)
                    return false;
                for (int row = 0; row < h; ++row)
                {
                    const BYTE* rowPx = pixels + static_cast<size_t>(row) * static_cast<size_t>(rowStride);
                    for (int col = 0; col < w; ++col)
                    {
                        if (rowPx[col * 4 + 3] > 0)
                            return true;
                    }
                }
                return false;
            };

        if (!previewBufferHasVisibleAlpha(bgraPremul, width, height, stride))
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][RenderText] PaintTextPreviewToBuffer: ink buffer empty (%dx%d), plain GDI fallback",
                width,
                height);
            std::memset(dibBits, 0, static_cast<size_t>(stride) * static_cast<size_t>(height));
            RECT textBounds = paintRect;
            DrawTextW(
                memDC.get(),
                request->sampleText,
                static_cast<int>(request->sampleCch),
                &textBounds,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            for (int row = 0; row < height; ++row)
            {
                std::memcpy(
                    bgraPremul + static_cast<size_t>(row) * static_cast<size_t>(stride),
                    src + static_cast<size_t>(row) * static_cast<size_t>(width) * 4u,
                    static_cast<size_t>(width) * 4u);
            }
        }

        return S_OK;
    }
}