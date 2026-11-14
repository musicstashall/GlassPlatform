# GlassCore::Text

**Связанный код:** `GlassShared/include/GlassInkAlphaCapsule.h`, `GlassCore/.../RenderText.*`, `GlassSettings/` (страница «Текст», мастер ink)

## GlassCore::Text — дерево параметров (реестр и UI)

Источник: `GlassShared\include\GlassInkAlphaCapsule.h`, `GlassCore\Include\Subsystems\RenderText.h`, реализация `ExtTextOutWithAlpha` / `RenderTextInkDispatch` в `RenderText.cpp`. UI **GlassSettings** (`GlassSettings/Core/Text/`, `GlassSettings/TextWizard/`) повторяет капсулу. Черновик в RAM — ко **всем** полям ввода под корнем host-окна настроек (`GlassCore_SetRenderTextPreviewCapsule` / `GlassCore_ClearRenderTextPreview`).

**Два независимых уровня (не путать):**

1. **Глобальный Glow** — структура `GlobalTextGlow` (отдельный ключ реестра `GlobalTextGlow`, REG_BINARY): `glowEnabled`, радиус/интенсивность в ‰, `glowColor`, `glowSpreadMode` (0…2). **Не** входит в `ProfilePayload` single/dual. В рендере `glowColor == 0` трактуется как `GetSysColor(COLOR_WINDOW)`. Используется **для текста в контролах** (theme, `DrawTextWithAlpha` / uxtheme #126, `IsInkGlowEffectActive`, IAT `DrawTextExW` в ListView, Button и т.д.). Внешняя группа на странице **«Текст»** в **GlassSettings** настраивает этот уровень; live-превью — через RAM-капсулу без записи в реестр.

2. **Псевдо-Glow / контур (ExtTextOut)** — поля в `ProfilePayload`: `pseudoGlowEnabled`, `pseudoGlowRadiusThousandths` (0…1000 = 0…1 px), `pseudoGlowIntensity` (0…255). Только **дополнительная** постобработка premul после single/dual DIB в **`ExtTextOutWithAlpha`**: цвет сглаживания = **цвет текста**. Третья **дочерняя** вкладка `TabControl` в блоке настроек профиля.

**Прочие поля в профиле:** `coverageGammaThousandths` (подвкладка «Гамма», UI **250…5000** ‰), `targetBlendAlpha` (подвкладка «Смешивание»). Устаревшие слоты (preserve, fringe и т.д.) пишутся нейтралями — см. `WriteNeutralDeprecatedInkSlots` в заголовке капсулы.

### Coverage gamma: UI и dual-pass

| Слой | Поведение |
|------|-----------|
| **Слайдер / REG_BINARY** | Единый диапазон **250…5000** ‰ (`InkCapsuleQuery::kUiGammaMinThousandths` … `kUiGammaMaxThousandths`). Значение в капсуле — то, что видит пользователь. |
| **Dual-pass рендер** | При применении профиля для dual-pass (режим «Два прохода», «Авто», `ScopedAutoEditInkProfile` с `useDualPassPath`) effective gamma = **stored × 2** (`kDualPassCoverageGammaMultiplier`), cap **10000** ‰ — см. `EffectiveCoverageGammaThousandths`. На слайдере **не** отображается. |
| **Single-pass** | Множитель **не** применяется. |

**«Авто»** — factory baseline (`EnterAutoMode` / `kFactoryUserProfileSingle` + `kFactoryUserProfileDual`); движок сам выбирает single/dual-путь. При переключении **один проход ↔ два прохода** пресеты **не** подставляются: слайдеры сохраняют текущие значения; отличие dual — только ×2 гаммы в рантайме.

Factory dual: в капсуле **1775** ‰ (слайдер) → effective **3550** ‰ при dual-pass; **pseudoGlowEnabled=1**, R/I **610 / 113**.

Константы и clamp: [`GlassShared/include/InkCapsuleQuery.h`](../../GlassShared/include/InkCapsuleQuery.h). Применение в рантайме: `ApplyProfileInkGenerationToGlobals` / `g_ExtTextOutCoverageGamma` в [`RenderText.cpp`](../../GlassCore/src/Subsystems/RenderText.cpp).
