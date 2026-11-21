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

## Edit / TMemo / RichEdit: текущая политика фона

**Код:** `GlassCore/src/Hooks/EditHooks.cpp`, `WndCompat/Root/RootSubclass.cpp`.
Политика относится к существующим фоновым операциям, а не к новой замене
текстового renderer или распознаванию диапазона выделения.

| Контрол | Когда после BLACKNESS выполняется цветная заливка |
| --- | --- |
| Plain Edit и прежние Edit-алиасы, кроме точного `TMemo` | RGB отличен от чёрного, текущего `COLOR_WINDOW` и текущего `COLOR_BTNFACE`. |
| Точный `TMemo`, RichEdit20W / RICHEDIT50W и их маркированные варианты, включая `TRichEdit` | RGB отличен только от текущего `COLOR_WINDOW`. Чёрный и `COLOR_BTNFACE` также проходят через alpha-кисть. |

Если цветная заливка разрешена этой проверкой, её кисть получает **A=255**, если
24-битный RGB равен текущему `GetSysColor(COLOR_HIGHLIGHT)`, иначе **A=100**.
Сравнивается цвет, без selection-range classifier. Правило одинаково для
прямого `ApplyEditBackgroundRect`, отложенного `ReplayMemoBackgrounds` и
подтверждённого `PrimeMemoSolidBackground`. Условия priming, порядок очистки,
clip/inset, текстовый DIB, single/dual-pass и последующий `tblend` не меняются;
A кисти не является гарантией A итогового оконного пикселя после композиции.
Для exact TMemo CTLCOLOR сохраняет исходный RGB, чтобы настоящий чёрный фон
не смешивался с подстановкой прозрачного `COLOR_WINDOW`; у остальных Edit
прежняя CTLCOLOR-политика.

### Отложенная задача: выделение RICHEDIT50W и TRichEdit

**Не решено; дальнейшая работа — в отдельной сессии по решению пользователя.**
Выделение RICHEDIT50W (в сообщениях также RichEditW50) и смежного VCL
`TRichEdit` остаётся полупрозрачным. Смежный RichEdit20W также требует отдельной
проверки; одинаковый внутренний kind или имя VCL-класса не доказывают версию DLL.
Дополнительные RichEdit `FillRect`/`PatBlt`-хуки из `f409a21` не дали результата
и полностью отменены `9acedca`; не возвращать их как принятое исправление.

Пользователь сообщил цвет выделения `#9BCAEF` в текущей светлой теме, отличный
от системного `COLOR_HIGHLIGHT`. Если это запись RGB, соответствующий GDI
COLORREF — `0x00EFCA9B`, не `0x009BCAEF`. Microsoft описывает
[acetate selection](https://learn.microsoft.com/en-us/archive/blogs/murrays/richedit-colors)
как смешение базового выделения с фоном (типично около 39% COLOR_HIGHLIGHT).
Это объяснение наблюдаемого оттенка, не универсальная константа для кода.

Для следующей сессии: `EM_GETEDITSTYLEEX` / `SES_EX_NOACETATESELECTION`
сообщает режим на поддерживающем его движке, но не итоговый RGB;
`EM_GETCHARFORMAT(SCF_SELECTION)` / `ITextFont::GetBackColor` возвращает
форматирование текста, не UI-подсветку, а `GetEffectColor` — цвета подчёркивания.
Прямой документированный getter итогового цвета UI-выделения не найден.
`GetBkColor(hdc)` в уже существующем ETO_OPAQUE-пути даёт фактический цвет
операции, но сам по себе не доказывает, что операция — выделение. Не вводить
хардкод `#9BCAEF`, фиксированный коэффициент смешения, отключение acetate или
новый classifier без отдельного согласованного решения. В текущем шаге код
Edit/RichEdit не меняется.
