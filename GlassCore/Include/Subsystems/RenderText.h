#pragma once
#include <windows.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <wil.h>
#include <functional>
#include <cstdint>
#include <GlassInkAlphaCapsule.h>
#include <Core/TextPreviewExport.h>

namespace GlassCore::Text
{
    // Режим качества/скорости для ExtTextOutWithAlpha (см. EditHooks: GetDC многострочного Edit).
    enum class ExtTextOutAlphaMode : std::uint8_t
    {
        FullQuality = 0,           // двухпроходный diff (капсула: gamma и др.)
        EditGetDcInteractive = 1,  // single-pass при интерактивном GetDC
        // Plain однострочный Edit: auto (0) и dual (2) — двухпроходный diff; single (1) — single-pass с капсулой в ProcessWhiteOnBlackPixels.
        EditSingleLinePlain = 2,
    };

    // Новые глобальные параметры (определения в RenderText.cpp)
    extern BYTE g_ExtTextOutRenderMode;   // 0=auto, 1=single, 2=dual

    void Prepare();
    void Startup();
    void Shutdown();
    void Update();

    // Рисование текста с альфа-каналом через буферизацию (uxtheme DrawTextWithGlow / ordinal #126 при наличии).
    // Основная роль API — premultiplied-alpha; параметры «свечения» из капсулы опциональны.
    // transparentBufferBackground: без BPPF_ERASE (иначе весь paintRect заливается фоном окна непрозрачно);
    // буфер обнуляется — остаётся только альфа от глифов. Для сплит-стрелки Marlett на полном lprc.
    HRESULT DrawTextWithAlpha(
        HDC hdc,
        LPCWSTR text,
        int cchText,
        LPRECT rect,
        UINT format,
        int& outResult,
        bool transparentBufferBackground = false);

    // Тюнинг ExtTextOutWithAlpha — п.1 (кривая маски): гамма после извлечения coverage.
    // 1.0f — без изменений.
    // 0.85f … 0.95f — текст жирнее, мягче края (больше полутонов).
    // 1.05f … 1.2f — текст тоньше, жёстче края, меньше ореола.
    extern float g_ExtTextOutCoverageGamma;

    /// Глобальный glow (текст в контролах, theme / #126 / IAT). Не путать с псевдо-Glow.
    /// Мастер-включатель: при false горячие пути не читают радиус/интенсивность.
    extern bool g_ExtTextOutGlowEnabled;
    extern uint32_t g_ExtTextOutGlowRadiusThousandths;
    extern uint32_t g_ExtTextOutGlowIntensityThousandths;
    /// Распространение свечения: 0…2 (GlassSettings); в рендере подключается позже.
    extern uint32_t g_ExtTextOutGlowSpreadMode;

    /// Псевдо-Glow: только PostProcess premul в ExtTextOutWithAlpha (Edit/RichEdit). Цвет ореола = цвет текста; не отключается по GetBkColor (в t.ч. selection).
    extern bool g_ExtTextOutPseudoGlowEnabled;
    extern uint32_t g_ExtTextOutPseudoGlowRadiusThousandths;
    extern uint32_t g_ExtTextOutPseudoGlowIntensity;

    /// Единая политика ink-glow по цвету: мастер g_ExtTextOutGlowEnabled и RGB эффективного текста ≠ RGB цвета ореола
    /// (как в GetDrawTextWithGlowApiArgs: effective capsule по теме ОС). Сравнение по младшим 24 битам.
    [[nodiscard]] bool IsInkGlowPolicyEnabledForEffectiveText(COLORREF effectiveTextColor) noexcept;

    /// Мастер glow включён и радиус/интенсивность ненулевые (глобальное «эффект активен» для IAT DrawTextExW / EnableThemeHooks).
    [[nodiscard]] bool IsInkGlowEffectActive() noexcept;

    /// IsInkGlowEffectActive() + spread policy для конкретного HWND (Moderate/Global; popup — без spread).
    [[nodiscard]] bool IsInkGlowEffectActiveForWindow(HWND hWnd) noexcept;

    /// IAT paint-session: Minimal → только master glow; Moderate/Global → spread policy.
    [[nodiscard]] bool IsInkGlowEffectActiveForPaintSession(HWND hWnd) noexcept;

    // Дополнительная альфа при AlphaBlend на целевой hDC (BLENDFUNCTION.SourceConstantAlpha).
    // 255 — как раньше; 0 — полностью прозрачно; промежуточные — общая прозрачность текста.
    extern BYTE g_ExtTextOutTargetBlendAlpha;

    /// Смешивание для composite: из активного профиля капсулы (reload), не из RAM-превью GlassSettings.
    /// useDualPassPath — тот же выбор single/dual, что у RenderTextInkDispatch / FlushEditBuffer.
    [[nodiscard]] BYTE ResolveEffectiveTargetBlendAlpha(bool useDualPassPath) noexcept;

    /// Разрешённые параметры ink для одного вызова ExtTextOutWithAlpha (гамма уже с учётом dual-pass).
    struct ExtTextOutInkDrawParams
    {
        float    coverageGamma = 1.f;
        BYTE     targetBlendAlpha = 255;
        bool     pseudoGlowEnabled = false;
        uint32_t pseudoGlowRadiusThousandths = 0;
        uint32_t pseudoGlowIntensity = 0;
    };

    /// Активный профиль капсулы для штатного рендера (g_userProfile*; обновляется при старте и экспортах).
    [[nodiscard]] const Shared::InkCapsule::ProfilePayload& GetRuntimeInkCapsuleProfile(
        bool dualPassPath) noexcept;

    /// Построить draw-параметры из ProfilePayload и флага dual-pass (×2 гаммы при dual).
    [[nodiscard]] ExtTextOutInkDrawParams BuildInkDrawParamsFromProfile(
        const Shared::InkCapsule::ProfilePayload& profile,
        bool applyDualPassMultiplier) noexcept;

    // Реестр: HKCU|HKLM\…\GlassInkAlphaCapsule REG_BINARY (см. GlassShared\include\GlassInkAlphaCapsule.h).
    // При отсутствии или битом BINARY читается устаревший подключ …\RenderText (старые REG_DWORD).
    // SaveRenderTextSettings пишет только HKCU, один REG_BINARY в корне ключа.
    // Load вызывается из Startup(); при отсутствии данных остаются дефолты из кода.
    void LoadRenderTextSettings() noexcept;
    [[nodiscard]] HRESULT SaveRenderTextSettings() noexcept;
    /// Повторное чтение HKCU/HKLM капсулы и легаси RenderText (без RAM-превью GlassSettings).
    void ReloadRenderTextInkFromRegistry() noexcept;

    /// Переарм IAT-хуков DrawTextExW / DTTEx после смены GlobalTextGlow в реестре.
    void SyncTextGlowHooksFromRegistry() noexcept;

    // Рисование ExtTextOut (в т.ч. ETO_GLYPH_INDEX) с корректным альфа-каналом.
    // Два прохода: текст в GetTextColor на чёрном и белом 32bpp DIB → дифф. восстановление premul BGRA
    // (ближе к GetDrawTextWithGlow без свечения) → GdiAlphaBlend или слияние в сессионный буфер Edit.
    // origExtTextOut — сохранённый оригинал из IATRegistry.
    // plainEditHwnd: для EditSingleLinePlain — тот же HWND, что в EditHooks (PlainEditUseDualPassSingleLinePlain);
    // если nullptr, используется WindowFromDC(hdc) (может расходиться при нестандартном DC).
    // inkCapsule: nullptr → GetRuntimeInkCapsuleProfile(dualPass); иначе per-call профиль (preview, без мутации globals).
    // dualPassOverride: не nullptr → принудительный single/dual для preview (игнорирует g_ExtTextOutRenderMode).
    HRESULT ExtTextOutWithAlpha(
        HDC hdc,
        int x, int y,
        UINT options,
        const RECT* lpRect,
        LPCWSTR lpString,
        UINT cbCount,
        const INT* lpDx,
        decltype(&::ExtTextOutW) origExtTextOut,
        ExtTextOutAlphaMode mode = ExtTextOutAlphaMode::FullQuality,
        HWND plainEditHwnd = nullptr,
        const Shared::InkCapsule::ProfilePayload* inkCapsule = nullptr,
        const bool* dualPassOverride = nullptr);

    // Многострочный plain Edit в GetDC: тот же узкий paint rect (+1px), что ExtTextOutWithAlphaToSession,
    // и тот же dual/single, что у сессии WM_PAINT (!WS_HSCROLL → dual при override).
    HRESULT ExtTextOutWithAlphaMultilinePlainGetDc(
        HDC hdc,
        int x, int y,
        UINT options,
        const RECT* lpRect,
        LPCWSTR lpString,
        UINT cbCount,
        const INT* lpDx,
        decltype(&::ExtTextOutW) origExtTextOut,
        bool sessionLikeUseDualPass,
        const Shared::InkCapsule::ProfilePayload* inkCapsule = nullptr);

    // Сессионный 32bpp-буфер (только многострочный Edit в EditHooks).
    struct TextSessionBuffer
    {
        RGBQUAD* bits;
        int      width;
        int      height;
        // false: однопроходный путь; true: dual-pass (совпадает с PlainEditUseDualPass для данного HWND).
        bool useDualPass = true;
        bool hasDualPassOverride = false;
    };

    // refWindowDc: DC окна (GetDC / BeginPaint), тот же что у ExtTextOut. Не memory DC сессии —
    // иначе DrawThemeContent → BeginBufferedPaint может AV в uxtheme.
    HRESULT ExtTextOutWithAlphaToSession(
        HDC          sessionDC,
        int x, int y,
        UINT         options,
        const RECT* lpRect,
        LPCWSTR      lpString,
        UINT         cbCount,
        const INT* lpDx,
        decltype(&::ExtTextOutW) origExtTextOut,
        const TextSessionBuffer& session,
        const Shared::InkCapsule::ProfilePayload* inkCapsule = nullptr);

    // Пул DIB для многострочного plain Edit: один экземпляр на вложенную цепочку GetDC (ClaudeTextPlan §8).
    void BeginEditGetDcAlphaScratchSession() noexcept;
    void EndEditGetDcAlphaScratchSession() noexcept;
    void ForceResetEditGetDcAlphaScratchSession() noexcept;

    // Black glow: реестр — отдельный ключ.
    void SetRenderTextPreviewGlobalGlowBytes(const void* data, size_t byteCount) noexcept;

    // ── Реестровые настройки (добавить в документацию/README): ──
//
// HKCU\SOFTWARE\...\RenderText\
//   RenderMode    : REG_DWORD
//     0 = auto (dual-pass, кроме GetDC WS_HSCROLL Edit)
//     1 = always single-pass
//     2 = always dual-pass
//
    // Или через GlassInkAlphaCapsule REG_BINARY (renderMode в шапке капсулы).

    // Многострочный plain и сессия WM_PAINT: rm + при Авто учёт WS_HSCROLL (как comctl: с прокруткой — single-pass).
    [[nodiscard]] bool PlainEditUseDualPass(HWND hWnd, BYTE renderMode) noexcept;

    // Однострочный plain Edit: при Авто — как многострочный без WS_HSCROLL (dual-pass), без «адаптации» по HSCROLL.
    [[nodiscard]] bool PlainEditUseDualPassSingleLinePlain(HWND hWnd, BYTE renderMode) noexcept;

    // Обёртка для рисования контента с буферизацией (использует BeginBufferedPaint).
    // eraseBufferedPaintBackground: false — не BPPF_ERASE, буфер обнуляется вручную (прозрачная подложка).
    HRESULT DrawThemeContent(
        HDC hdc,
        const RECT& paintRect,
        const RECT* clipRect,
        const RECT* excludeRect,
        DWORD additionalFlags,
        const std::function<void(HDC, HPAINTBUFFER, RGBQUAD*, int)>& callback,
        BYTE alpha = 0xFF,
        bool useBlt = false,
        bool update = true,
        bool eraseBufferedPaintBackground = true);

    /// Hero-превью GlassSettings: offscreen premul BGRA через ExtTextOutWithAlpha.
    HRESULT PaintTextPreviewToBuffer(
        BYTE* bgraPremul,
        int   width,
        int   height,
        int   stride,
        const GlassCoreTextPreviewRequest* request,
        bool  interactiveDuringGesture) noexcept;
}