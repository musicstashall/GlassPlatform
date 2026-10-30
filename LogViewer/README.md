# LogViewer

**Связанный код:** `LogViewer/`

### LogViewer (EXE) — ориентиры для правок

- **Закрытие главного окна:** `LogViewer\UiMainWindow.cpp`, обработчик `WM_CLOSE`. Пока mutex `GlassPlatform.Instance.Mutex` существует — `MessageBoxW` с `MB_YESNOCANCEL` (по умолчанию «Отмена»): **Да** — `UiMainWindowImpl::StopPlatform()` и `DestroyWindow`; **Нет** — только `DestroyWindow` (платформа продолжает работу); **Отмена** — не закрывать. Если mutex нет — `break` и `DefWindowProc` (обычное закрытие).
- **Кнопка настроек Glass (`IDC_GLASS_SETTINGS`):** запускает `GlassSettings.exe` из каталога сборки (`UiMainWindow_WndProc.cpp`).
- **Изоляция от платформы:** в процессе LogViewer `Root\NoBackdrop` трактуется как **0** (backdrop всегда разрешён); `RootBackdropMode` фиксирован на SWCA и **не** следует за snapshot старта/стопа платформы (`ProcessBlockList::IsCurrentProcessLogViewerHost`, `RootPolicyQuery`). Старт/стоп GlassCompat на другие процессы не распространяется на локальные GlassCore/WndCompat LogViewer.
- **Окно «Параметры отрисовки Glass» (если открывается из LogViewer):** `UiPlatformSettings*.cpp`, `UiPlatformSettingsDlg.h`. Настройки **текста** и ink — в **GlassSettings**; для локального превью в LogViewer — `GlassCore_SetRenderTextPreviewCapsule` / `GlassCore_ClearRenderTextPreview`, `GlassCore_ReloadRenderTextInkFromRegistry`. Инициализация движка в процессе хоста — `GlassCore_HostUiEnsureEngine`. Живая альфа DrawTheme для кнопок/комбо/табов — `GlassCore_SetThemePaintPreviewForGlassSettingsUi` (см. `GlassHost.cpp`, `ThemePaintRegistry`). **Важно:** в `enum PlatformSettingsCtrlId` у каждого дочернего элемента диалога должен быть **свой** числовой ID; дубли дают неверный `GetDlgItem` и ломают маркировку HWND (нижний ряд OK/Отмена/Применить — отдельный диапазон, см. комментарий в заголовке).
