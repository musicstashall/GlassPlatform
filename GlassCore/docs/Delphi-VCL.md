# Delphi VCL: поддержка в GlassPlatform

Документ описывает текущую реализацию поддержки Delphi/VCL-приложений в `WndCompat` и `GlassCore`.

## Область поддержки

Поддержка ориентирована на VCL-классы (Delphi), обрабатываемые через классификацию окон в `WndCompat` и IAT/paint-хуки в `GlassCore`. Реализация работает в контексте существующих режимов GlassPlatform (`Full`/`PopupOnly`) и политик backdrop/attach.

### Классы, обрабатываемые специальными путями

- **Контейнеры/панели VCL:** `TPanel`, `TFrame`, `TPage`, `TTabSheet`, `TGroupBox`, `TRadioGroup`, `TCard`, `TSampleFrame`, `TEmbeddedForm` и др. из `GlassShared/GlassDelphiPanel.h`; панели обрабатываются через bridge `WndCompat/Root/DelphiPanelPaint.h` → `GlassCore_BeginDelphiPanelPaint/EndDelphiPanelPaint` → `GlassCore::DelphiPanel`.
- **Hint/tooltip VCL:** `THintWindow` классифицируется как tooltip/popup; attach в `WndCompat::Tooltip_AttachHintWindow`; отрисовка через `WndCompat/Popup/Tooltip/DelphiHintPaint.h` → `GlassCore::DelphiHint`.
- **Delphi CoolBar:** `TCoolBar`/`CoolBar` — отдельный путь `WndCompat/Controls/TCoolBarSubclass.*` + `GlassCore::CoolBarControl` (`Hooks/delphi/TCoolBarDraw.*`), не generic ReBar.
- **Стандартные контролы VCL-обёртки:** `TButton`, `TBitBtn`, `TCheckBox`, `TRadioButton`, `TGroupButton`, `TEdit`, `TMemo`, `TLabeledEdit`, `TButtonedEdit`, `TComboBox`, `TListBox`, `TCheckListBox`, `TColorListBox`, `TFileListBox`, `TDirectoryListBox`, `TPageControl`, `TTabControl`, `THeaderControl`, `TMonthCalendar`, `TTrackBar`, `TScrollBar`, `TUpDown`, `TStatusBar`, `TProgressBar`, `TToolBar` и др. маршрутизируются в соответствующие существующие control-пути WndCompat/GlassCore.
- **Delphi-контейнеры:** `TGroupBox`, `TRadioGroup`, `TCard`, `TTabSheet`, классы из `GlassDelphiPanel.h`, `GlassDelphiButtonGroups.h` классифицируются как `ControlKind::DelphiContainer`.
- **Специальные Delphi-хуки GlassCore:** `TBackgroundHooks`, `TInputHooks/TInputIcons`, `TListBoxDraw`, `TBitBtnHooks`, `TToggleSwitchHooks`, `THexDumpDraw`, `TResVersionEditorDraw`, `BomeMemoDraw`, `TButtonGroupsDraw`, `TPanelDraw`, `THintWindowDraw`, `RestoratorImages`, `RestoratorCheckerboard`.

## Точки интеграции

### WndCompat

- **Классификация:** `WndCompat/Tracker/WindowClassifier.cpp` определяет VCL/Delphi классы (`THintWindow`, `TPageControl`, `DelphiContainer`, CoolBar, панели и т.д.).
- **Attach/dispatch:** `WndCompat/Tracker/WinEventHandler.cpp` выполняет attach/detach для hint, CoolBar, контейнеров и стандартных контролов по ролям/видам.
- **Root subclass:** `WndCompat/Root/RootSubclass.cpp` обрабатывает Delphi-specific WM-пути:
  - `DelphiShowErase.h` — одноразовая очистка поверхности top-level VCL окна при show/restore, чтобы избежать показа устаревшего содержимого до WM_PAINT дочерних контролов.
  - `DelphiPanelPaint.h` — bridge для WM_PAINT/WM_ERASEBKGND/WM_PRINTCLIENT точных Delphi panel/page-классов.
  - owner-draw Delphi BitBtn/ListBox/Combo-поле через exports GlassCore.
- **Hint paint:** `WndCompat/Popup/Tooltip/DelphiHintPaint.h` перехватывает WM_PAINT/WM_ERASEBKGND/WM_NCPAINT для `THintWindow` и вызывает GlassCore hint bridge.

### GlassCore

- **Lifecycle:** Delphi-хуки запускаются/останавливаются из `GlassCore/src/Core/Engine.cpp` и связанных control startup/shutdown (например, `DelphiListBox` из `ListBoxHooks`).
- **Exports/WndCompat bridge:** `GlassCore/src/Core/Exports.cpp` и `GlassCore/GlassCore.def` экспортируют narrow bridge-функции для BitBtn draw, ListBox draw, Panel paint, Hint paint.
- **IAT/paint hooks:** `GlassCore/src/Hooks/RootGdiHooks.cpp` содержит Delphi-specific маршруты (panel/grid/group/bome memo/theme text и др.). Сессионные IAT-хуки устанавливаются через IATRegistry/HookDispatcher с учётом refcount.
- **Специализированные модули:** `GlassCore/src/Hooks/delphi/*` реализуют поведение конкретных VCL-классов/семейств (фон, текст, owner-draw, images/checkerboard, hint, toggle, input icons и т.д.).

## Ограничения

- Поддержка не является универсальной для всех сторонних VCL-компонентов/наследников; для нестандартных paint-путей могут требоваться отдельные маршруты.
- Вызовы через GetProcAddress/динамические импорты/BPL, нестандартные memory-DC/WM_PRINTCLIENT-пути и owner-draw вне покрытых сценариев могут обходить текущие IAT-маршруты.
- Поведение зависит от режима инжекции, backdrop-политики, класса корневого окна и фактического пути отрисовки конкретного контрола.
- Диагностический VCL draw probe из предыдущих итераций не входит в текущую реализацию.

## Связанные документы

- [hooks-and-paint.md](hooks-and-paint.md) — IAT/HookDispatcher/PaintDispatcher.
- [RootAdvancedIconBackground.md](RootAdvancedIconBackground.md) — advanced icon/image background и Restorator-специфичные пути.
- [GlassShared/docs/gdi-alpha.md](../../GlassShared/docs/gdi-alpha.md) — GDI/альфа-хелперы.
