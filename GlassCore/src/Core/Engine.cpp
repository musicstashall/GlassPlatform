#include "CorePch.h"
#include "Core\Engine.h"

#include <cstdio>

#include <Core/Intercept/Hooks.h>
#include <Core/Intercept/IATRegistry.h>
#include "Subsystems\Animation.h"
#include "Subsystems\Menu.h"
#include "Subsystems\Tooltip.h"
#include "Subsystems\Controls.h"
#include "Subsystems\RenderText.h"
#include "Subsystems\ThemePaintRegistry.h"
#include "Subsystems\TextGlowClientRegistry.h"
#include "Subsystems\TextInkClientRegistry.h"
#include <Hooks/DropDown.h>
#include <Hooks/ComboLBox.h>
#include <Hooks/MonthCal.h>
#include <Hooks/DirectUI.h>
#include <Hooks/Button.h>
#include <Hooks/UpDown.h>
#include <Hooks/forms/FormsHooks.h>
#include <Subsystems/CtlColorProbe.h>
#include <Hooks/Static.h>
#include <Hooks/TreeView.h>
#include <Hooks/ListView.h>
#include <Hooks/ScrollBar.h>
#include <Hooks/Rebar.h>
#include <Hooks/Tab.h>
#include <Hooks/Header.h>
#include <Hooks/Toolbar.h>
#include <Subsystems/PaintDispatcher.h>
#include <Hooks/RootWindowHooks.h>
#include <Hooks/EditControl.h>
#include <Hooks/UiRibbonHooks.h>
#include <Hooks/IeServer.h>
#include <Hooks/ExplorerFrame.h>
#include <Hooks/ImmersiveHooks.h>
#include <Hooks/ProgressBar.h>
#include <Hooks/StatusBar.h>
#include <Hooks/DateTimePicker.h>
#include <Hooks/ListBox.h>
#include <Hooks/ComboBoxControl.h>
#include <Hooks/TaskbarHooks.h>
#include <DXHelper.h>

namespace GlassCore::Engine
{
    static std::atomic<bool> g_initialized{ false };
    static std::atomic<bool> g_initInProgress{ false };
    static std::atomic<bool> g_initFailed{ false };
    // Последний успешно начатый шаг Initialize (для диагностики AV / soft-abort).
    static std::atomic<unsigned> g_initStep{ 0 };
    static HMODULE g_module = nullptr;

    static void LogInitStep(unsigned step) noexcept
    {
        g_initStep.store(step, std::memory_order_release);
    }

    [[nodiscard]] bool IsReady() noexcept
    {
        return g_initialized.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool InitFailed() noexcept
    {
        return g_initFailed.load(std::memory_order_acquire);
    }

    bool Initialize(HMODULE module)
    {
        // H63: g_initialized только после успешного конца. H53 ставил true в начале —
        // AV в HWiNFO оставлял «полу-Engine», Shutdown зависал, Compat не выгружался.
        if (g_initialized.load(std::memory_order_acquire))
            return true;

        // H64: после SEH-abort не ретраить Initialize (повторный AV / CRT).
        if (g_initFailed.load(std::memory_order_acquire))
            return false;

        bool expected = false;
        if (!g_initInProgress.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            while (g_initInProgress.load(std::memory_order_acquire) &&
                   !g_initialized.load(std::memory_order_acquire) &&
                   !g_initFailed.load(std::memory_order_acquire))
            {
                Sleep(1);
            }
            return g_initialized.load(std::memory_order_acquire);
        }

        if (g_initialized.load(std::memory_order_acquire))
        {
            g_initInProgress.store(false, std::memory_order_release);
            return true;
        }

        if (g_initFailed.load(std::memory_order_acquire))
        {
            g_initInProgress.store(false, std::memory_order_release);
            return false;
        }

        g_module = module;
        LogInitStep(1);
        // Инициализация Buffered Paint API (требуется для BeginBufferedPaint)
        BufferedPaintInit();
        LogInitStep(2);
        // Адреса DrawThemeBackgroundEx/DrawThemeText в uxtheme — до Hooks::Install, иначе при
        // ScanExistingModules: Prepare должен выставить адреса uxtheme до первой paint-сессии DirectUI (DTBEx).
        GlassCore::DirectUI::Prepare();
        LogInitStep(3);
        // 1) Устанавливаем минихук (скан модулей + LdrDllNotification)
        GlassCore::Hooks::Install();
        LogInitStep(4);

        // 2) Подготовка подсистем (без включения хуков)
        GlassCore::Menu::Prepare();
        GlassCore::Tooltip::Prepare();
        GlassCore::Controls::Prepare();
        GlassCore::Text::Prepare();
        GlassCore::DropDown::Prepare();
        GlassCore::ComboLBox::Prepare();
        GlassCore::ComboBoxControl::Prepare();
        GlassCore::MonthCal::Prepare();
        GlassCore::ProgressBarControl::Prepare();
        GlassCore::StatusBarControl::Prepare();
        GlassCore::DateTimePickerControl::Prepare();
        GlassCore::ListBoxControl::Prepare();
        GlassCore::DirectUI::Prepare();
        GlassCore::Button::Prepare();
        GlassCore::UpDownControl::Prepare();
        GlassCore::Hooks::Immersive::Prepare();
        GlassCore::StaticControl::Prepare();
        GlassCore::TreeViewControl::Prepare();
        GlassCore::ListViewControl::Prepare();
        GlassCore::ScrollBar::Prepare();
        GlassCore::RebarControl::Prepare();
        GlassCore::TabControl::Prepare();
        GlassCore::HeaderControl::Prepare();
        GlassCore::ToolbarControl::Prepare();
        GlassCore::EditControl::Prepare();
        GlassCore::IeServerControl::Prepare();
        GlassCore::ExplorerFrameControl::Prepare();
        LogInitStep(5); // после всех Prepare

        // 3) Запуск подсистем (резолвим адреса, готовим trampolines)
        GlassCore::Menu::Startup();
        GlassCore::Tooltip::Startup();
        GlassCore::Controls::Startup();
        GlassCore::ThemePaint::LoadFromRegistry();
        GlassCore::ThemePaint::RefreshCachedForegroundWindow();
        GlassCore::Text::Startup();
        GlassCore::DropDown::Startup();
        GlassCore::ComboLBox::Startup();
        GlassCore::ComboBoxControl::Startup();
        GlassCore::DirectUI::Startup();
        LogInitStep(6); // до Button/Forms/Root Startup
        // Button, StaticControl и MonthCal регистрируют провайдеров до PaintDispatcher::Startup
        GlassCore::Button::Startup();
        GlassCore::UpDownControl::Startup();
        GlassCore::FormsHooks::Startup();
        GlassCore::Hooks::Immersive::Startup();
        GlassCore::StaticControl::Startup();
        GlassCore::TreeViewControl::Startup();
        GlassCore::ListViewControl::Startup();
        GlassCore::ScrollBar::Startup();
        GlassCore::RebarControl::Startup();
        GlassCore::TabControl::Startup();
        GlassCore::HeaderControl::Startup();
        GlassCore::ToolbarControl::Startup();
        GlassCore::MonthCal::Startup();
        GlassCore::ProgressBarControl::Startup();
        GlassCore::StatusBarControl::Startup();
        GlassCore::ListBoxControl::Startup();
        GlassCore::DateTimePickerControl::Startup();
        GlassCore::EditControl::Startup();
        GlassCore::UiRibbon::Startup();
        GlassCore::Root::Startup();
        GlassCore::ExplorerFrameControl::Startup();
        GlassCore::IeServerControl::Startup();
        // PaintDispatcher запускается последним среди Startup — провайдеры уже зарегистрированы
        GlassCore::PaintDispatcher::Startup();
        // Предпрогрев исполняемых страниц трамплинов — вне видимых paint-кадров.
        GlassCore::Hooks::IATRegistry::Instance().PreallocatePages(2);

        // WinUI backdrop hooks: включаются по жизненному циклу PopupWindowSiteBridge
        // (первый Attach / последний Detach) через WndCompat -> экспорт GlassCore.

        g_initialized.store(true, std::memory_order_release);
        g_initInProgress.store(false, std::memory_order_release);
        return true;
    }

    void AbortPartialInit() noexcept
    {
        if (g_initialized.load(std::memory_order_acquire))
            return;

        const unsigned step = g_initStep.load(std::memory_order_acquire);

        // H64/H66: AV после Hooks::Install (step>=4, доказано HWiNFO) — снять IAT.
        // H65 soft-without-Uninstall → UAF HookHelper/strcmp (дамп) + hang unload. Не повторять.
        if (step >= 4)
            GlassCore::Hooks::Uninstall();
        g_module = nullptr;
        g_initFailed.store(true, std::memory_order_release);
        g_initInProgress.store(false, std::memory_order_release);
    }

    bool InitializeSealed(HMODULE module) noexcept
    {
        if (g_initialized.load(std::memory_order_acquire))
            return true;
        if (g_initFailed.load(std::memory_order_acquire))
            return false;

        __try
        {
            return Initialize(module);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            AbortPartialInit();
            return false;
        }
    }

    void HostExitShutdown() noexcept
    {
        if (!g_initialized.exchange(false))
            return;

        GlassCore::Animation::ShutdownHostExit();
        GlassCore::Hooks::Uninstall();
        Shared::DXHelper::ReleaseAllLazyGraphicsResources();
        g_module = nullptr;
    }

    void Shutdown()
    {
        if (!g_initialized.exchange(false))
            return;


        // 0) Сначала останавливаем анимационный поток
        GlassCore::Animation::Shutdown();

        GlassCore::Hooks::TaskbarHooks::Shutdown();

        GlassCore::Root::Shutdown();
        GlassCore::UiRibbon::Shutdown();
        GlassCore::ExplorerFrameControl::Shutdown();

        // 1) Остановка подсистем
        GlassCore::Menu::Shutdown();
        GlassCore::Tooltip::Shutdown();
        GlassCore::Controls::Shutdown();
        GlassCore::Text::Shutdown();
        GlassCore::ThemePaint::ClearThemePaintPreviewAlpha();
        GlassCore::ThemePaint::ClearCachedForegroundWindow();
        GlassCore::ThemePaint::ClearAllThemePaintRootNcState();
        GlassCore::TextGlow::ClearAllTextGlowClients();
        GlassCore::TextInk::ClearAllTextInkClients();
        GlassCore::DropDown::Shutdown();
        GlassCore::ComboBoxControl::Shutdown();
        GlassCore::ComboLBox::Shutdown();
        GlassCore::MonthCal::Shutdown();
        GlassCore::DateTimePickerControl::Shutdown();
        GlassCore::ListBoxControl::Shutdown();
        GlassCore::StatusBarControl::Shutdown();
        GlassCore::ProgressBarControl::Shutdown();
        GlassCore::DirectUI::Shutdown();
        GlassCore::Button::Shutdown();
        GlassCore::UpDownControl::Shutdown();
        GlassCore::FormsHooks::Shutdown();
        GlassCore::CtlColorProbe::Shutdown();
        GlassCore::Hooks::Immersive::Shutdown();
        GlassCore::StaticControl::Shutdown();
        GlassCore::TreeViewControl::Shutdown();
        GlassCore::ScrollBar::Shutdown();
        GlassCore::ListViewControl::Shutdown();
        GlassCore::RebarControl::Shutdown();
        GlassCore::TabControl::Shutdown();
        GlassCore::HeaderControl::Shutdown();
        GlassCore::ToolbarControl::Shutdown();
        GlassCore::EditControl::Shutdown();
        GlassCore::IeServerControl::Shutdown();

        // PaintDispatcher выключается последним — после всех контролов
        GlassCore::PaintDispatcher::Shutdown();

        BufferedPaintUnInit();
        // 2) Снимаем минихук до освобождения DComp/D3D/D2D: иначе детур может обратиться к уже
        // уничтоженным COM-объектам (AV в d3d11/dcomp и т.п.).
        GlassCore::Hooks::Uninstall();

        Shared::DXHelper::ReleaseAllLazyGraphicsResources();

        g_module = nullptr;

#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER) && defined(_DEBUG)
        Shared::Log::GlassLogClient::Instance().Stop();
#endif
    }

    void Tick()
    {
    }
}