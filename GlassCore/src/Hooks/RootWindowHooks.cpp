// GlassCore/src/Hooks/RootWindowHooks.cpp — классификация root-окон и провайдеры PaintDispatcher.
// GDI-хуки Root_My* и HookDispatcher — в RootGdiHooks.cpp.
//
// ═══════════════════════════════════════════════════════════════════════════════
//  Откуда «ноги растут» (связь с BWM и поток данных)
// ═══════════════════════════════════════════════════════════════════════════════
//
//  В BWM (WndList.cpp + PaintHandler.cpp) три явные функции:
//    • Paint(...)          — MyBeginPaint / MyEndPaint (канал BeginPaint).
//    • Dc(...)             — MyGetDC / MyReleaseDC.
//    • CompatibleDc(...)   — MyCreateCompatibleDC / MyDeleteDC.
//  Каждая включает/выключает «свою» группу OverallHooks по if (класс окна, модуль…).
//
//  В Glass то же разделение на ТРИ канала, но политика «какие слоты IAT в какую группу»
//  вынесена в ClassifyRootWindow: один раз при Root_Attach заполняются три списка
//  (paintHooks / dcHooks / compatDcHooks). Root_OnBeginPaint / Root_OnGetDC /
//  Root_OnCreateCompatDC не ветвятся по классу — только читают свой список и зовут
//  EnableRootHook(mod, OHK_*, true/false). Ref-count в HookDispatcher (RootGdiHooks.cpp).
//
//  Цепочка:
//    WndCompat Root_Attach → OnRootHooksAttach → ClassifyRootWindow → s_rootCache[HWND].
//    Engine::Startup → Root::Startup → Register*Provider в PaintDispatcher.
//    BeginPaint → HookBeginPaint → провайдеры → Root_OnBeginPaint → EnableSpecs(paintHooks).
//    «Как рисовать» — в Root_My* (RootGdiHooks.cpp), когда IAT-слот включён.
//
//  DirectUI: слоты в paint через AppendRootPaintSpecs / AppendDirectUiRootPaintSpecsForHwnd (DirectUI_Attach).
// ═══════════════════════════════════════════════════════════════════════════════

#include "CorePch.h"
#include <Hooks/RootWindowHooks.h>
#include <Hooks/RootGdiHooks.h>
#include <Hooks/ExplorerFrame.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Hooks/forms/FormsHooks.h>
#include <Subsystems/TextGlowClientRegistry.h>
#include <DebugLog.h>

#include <RootBackdropPolicy.h>
#include <GlassBackdropMark.h>

#include <string>
#include <unordered_map>
#include <vector>
#include <Core/Intercept/DebugDetourHooks.h>

using namespace std::literals;

namespace GlassCore::Root
{
        static std::atomic<bool> g_shutdown{ false };

        // Кеш по HWND: для этого root при сессии Begin/GetDC/CompatDC включать такие IAT-слоты.
        // Заполняется в OnRootHooksAttach (после SetWindowSubclass в WndCompat).
        // HookSpec = (база DLL, индекс OHK_*) — см. RootGdiHooks.h / RootGdiHooks.cpp.
        //
        // Три вектора = три функции BWM (Paint / Dc / CompatibleDc). Один OHK может быть
        // в двух списках, если сценарий требует (например ExtTextOut и в Paint, и в Dc).
        struct RootHookCache
        {
            std::vector<HookSpec> paintHooks;       // только сессия BeginPaint / EndPaint
            std::vector<HookSpec> dcHooks;          // только GetDC / ReleaseDC
            std::vector<HookSpec> compatDcHooks;    // только CreateCompatibleDC / DeleteDC
            bool winFormsDrawTextEx = false;        // не IAT: Detours в Button, вместе с paint-сессией
        };

        static wil::srwlock                                     s_cacheLock;
        // Ключ — HWND из Root_Attach (не обязательно top-level).
        static std::unordered_map<HWND, RootHookCache>          s_rootCache;

        // Сессии по потоку: закрытие сверяется по HWND (P4/P5), а не снимает вслепую
        // вершину. Диспетчер вырезает записи из середины по HDC — чистый LIFO здесь
        // расходился с ним после первого внеочередного закрытия и дальше каждое
        // закрытие выключало чужие спеки.
        struct RootPaintSession
        {
            HWND hWnd = nullptr;
            std::vector<HookSpec> specs;
            bool winFormsDrawTextEx = false;
            // BWM PaintHandler.cpp, ветка window, callerModule == EntryPointModule:
            // динамический SETBKCOLOR на точке входа (dfrgui и аналоги).
            // Статика Classify покрывает только instMod == entry-point,
            // а caller BeginPaint виден лишь в сессии. Только Paint-канал.
            bool entryPointSetBkColor = false;
        };
        struct RootHookSession
        {
            HWND hWnd = nullptr;
            std::vector<HookSpec> specs;
        };
        thread_local std::vector<RootPaintSession>              g_rootPaintStack;
        thread_local std::vector<RootHookSession>               g_rootDcStack;
        thread_local std::vector<RootHookSession>               g_rootCompatDcStack;

        // ── Утилиты (только для ClassifyRootWindow) ─────────────────────────

        static bool StrEqI(LPCWSTR a, LPCWSTR b) noexcept
        {
            return a && b && _wcsicmp(a, b) == 0;
        }

        [[nodiscard]] static std::wstring BasenamePtr(LPCWSTR path) noexcept
        {
            if (!path || !path[0])
                return {};
            const wchar_t* p = wcsrchr(path, L'\\');
            return std::wstring(p ? p + 1 : path);
        }

        // Без линковки на WndCompat — минимальная проверка WinForms «.Window».
        [[nodiscard]] static bool IsWinFormsDotWindow(HWND hWnd) noexcept
        {
            WCHAR raw[256]{};
            if (GetClassNameW(hWnd, raw, _countof(raw)) <= 0)
                return false;
            std::wstring cls = raw;
            if (cls.rfind(L"WindowsForms10.", 0) != 0)
                return false;
            constexpr size_t prefixLen = 15;
            const size_t pos = cls.find(L'.', prefixLen);
            if (pos == std::wstring::npos)
                return false;
            const std::wstring inner = L"." + cls.substr(prefixLen, pos - prefixLen);
            return _wcsicmp(inner.c_str(), L".Window") == 0;
        }

        // ── Классификация окна → RootHookCache (ветка «window» BWM, не control) ──
        //
        // PushPaint / PushDc / PushCompat — в какой канал положить (модуль, OHK).
        // Комментарии «BWM Paint / Dc / …» у if ниже — соответствие PaintHandler.cpp.

        static void PushPaint(void* m, size_t i, RootHookCache& c) noexcept
        { if (m) c.paintHooks.push_back({ m, i }); }

        static void PushDc(void* m, size_t i, RootHookCache& c) noexcept
        { if (m) c.dcHooks.push_back({ m, i }); }

        static void PushCompat(void* m, size_t i, RootHookCache& c) noexcept
        { if (m) c.compatDcHooks.push_back({ m, i }); }

        // comctl32 в mmc живёт в двух экземплярах (WinSxS v6 + legacy): слоты фазы 0
        // подтверждают два разных iatslot-адреса. GetModuleHandleW(L"comctl32.dll")
        // возвращает только один (неизвестно какой), а ресайзный путь дисковой утилиты
        // (AfxWnd42u через MFC42u) может рисовать через legacy-экземпляр.
        // Поэтому слоты на comctl32 кладём на оба известных экземпляра
        // (g_comctl32New + g_comctl32Legacy); fallback — GetModuleHandleW,
        // если оба ещё null (classify до скана ModuleHandler::Startup; Push отбрасывает null).
        // Дедупликация: New и Legacy могут совпасть в одноверсионном процессе.
        static void PushPaintComctl32(size_t i, RootHookCache& c) noexcept
        {
            HMODULE n = GlassCore::ModuleHandler::g_comctl32New;
            HMODULE l = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (n) PushPaint(n, i, c);
            if (l && l != n) PushPaint(l, i, c);
            if (!n && !l) PushPaint(GetModuleHandleW(L"comctl32.dll"), i, c);
        }

        static void PushDcComctl32(size_t i, RootHookCache& c) noexcept
        {
            HMODULE n = GlassCore::ModuleHandler::g_comctl32New;
            HMODULE l = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (n) PushDc(n, i, c);
            if (l && l != n) PushDc(l, i, c);
            if (!n && !l) PushDc(GetModuleHandleW(L"comctl32.dll"), i, c);
        }

        static void PushCompatComctl32(size_t i, RootHookCache& c) noexcept
        {
            HMODULE n = GlassCore::ModuleHandler::g_comctl32New;
            HMODULE l = GlassCore::ModuleHandler::g_comctl32Legacy;
            if (n) PushCompat(n, i, c);
            if (l && l != n) PushCompat(l, i, c);
            if (!n && !l) PushCompat(GetModuleHandleW(L"comctl32.dll"), i, c);
        }

        [[nodiscard]] static bool IsRarextWindowModule(HWND hWnd) noexcept
        {
            wchar_t wndModPath[MAX_PATH]{};
            if (GetWindowModuleFileNameW(hWnd, wndModPath, _countof(wndModPath)) <= 0)
                return false;
            return StrEqI(BasenamePtr(wndModPath).c_str(), L"rarext.dll");
        }

        // IAT rarext: hot path фильтрует CallerBaseIs(rarext.dll) — слоты можно класть на shell property sheet.
        static void PushRarextHooksIfLoaded(RootHookCache& cache) noexcept
        {
            void* m = GetModuleHandleW(L"rarext.dll");
            if (!m)
                return;
            PushPaint(m, OHK_GRADIENTFILL, cache);
            PushPaint(m, OHK_LINETO, cache);
            PushPaint(m, OHK_TEXTOUT, cache);
            PushDc(m, OHK_GRADIENTFILL, cache);
            PushDc(m, OHK_LINETO, cache);
            PushDc(m, OHK_TEXTOUT, cache);
        }

        // Страница WinRAR рисует из rarext.dll, но Root_Attach часто на comctl32/SHELL32 #32770.
        static void EnsureRarextHookCache(HWND hWnd) noexcept
        {
            if (!IsRarextWindowModule(hWnd))
                return;
            auto lock = s_cacheLock.lock_exclusive();
            auto  it = s_rootCache.find(hWnd);
            if (it != s_rootCache.end()
                && (!it->second.paintHooks.empty() || !it->second.dcHooks.empty()))
                return;
            RootHookCache tmp;
            PushRarextHooksIfLoaded(tmp);
            auto& entry = s_rootCache[hWnd];
            if (entry.paintHooks.empty())
                entry.paintHooks = std::move(tmp.paintHooks);
            if (entry.dcHooks.empty())
                entry.dcHooks = std::move(tmp.dcHooks);
        }

        // Диагностика MMC: сессии GetDC/BeginPaint для графика/счётчиков.
        [[nodiscard]] static bool IsMmcChartSessionClass(LPCWSTR cn) noexcept
        {
            if (!cn)
                return false;
            if (StrEqI(cn, L"ChartGraphClass") || StrEqI(cn, L"MMCViewWindow")
                || StrEqI(cn, L"PerfStatus") || StrEqI(cn, L"SysmonCtrl") || StrEqI(cn, L"SysmonReport")
                || StrEqI(cn, L"PerfLegend"))
                return true;
            // Дочерний хост графика MMC (ATL thunk под ChartGraphClass) — отдельный BeginPaint.
            return wcsncmp(cn, L"ATL:", 4) == 0;
        }

        // MMC: кеш для ChartGraphClass лежит на контейнере графика; дочерние ATL:* делают свой BeginPaint/GetDC
        // без собственной записи в s_rootCache — без наследования IAT не включаются, фон не перехватывается.
        struct RootCacheLookupResult
        {
            const RootHookCache* cache = nullptr;
            bool inheritedFromChartGraph = false;
        };

        [[nodiscard]] static RootCacheLookupResult LookupRootHookCacheLocked(HWND hWnd) noexcept
        {
            RootCacheLookupResult r{};
            auto it = s_rootCache.find(hWnd);
            if (it != s_rootCache.end())
            {
                r.cache = &it->second;
                return r;
            }
            bool skipChartWalk = false;
            const HWND firstParent = GetParent(hWnd);
            static const ATOM chartMemoParentAtom = GlobalAddAtomW(L"GlassPlatform.ChartMemoParent");
            static const ATOM chartMemoResultAtom = GlobalAddAtomW(L"GlassPlatform.ChartMemoResult");
            if (firstParent)
            {
                const HANDLE seenParent = GetPropW(hWnd, MAKEINTATOM(chartMemoParentAtom));
                const HANDLE memo = GetPropW(hWnd, MAKEINTATOM(chartMemoResultAtom));
                if (seenParent == reinterpret_cast<HANDLE>(firstParent) && memo)
                {
                    if (memo == reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1)))
                    {
                        skipChartWalk = true;
                    }
                    else
                    {
                        const HWND chart = static_cast<HWND>(memo);
                        if (IsWindow(chart))
                        {
                            it = s_rootCache.find(chart);
                            if (it != s_rootCache.end())
                            {
                                r.cache = &it->second;
                                r.inheritedFromChartGraph = true;
                                return r;
                            }
                        }
                    }
                }
            }
            if (!skipChartWalk)
            {
            bool sawChartClass = false;
            for (HWND par = GetParent(hWnd); par; par = GetParent(par))
            {
                wchar_t pcls[128]{};
                if (GetClassNameW(par, pcls, _countof(pcls)) <= 0)
                    continue;
                if (!StrEqI(pcls, L"ChartGraphClass"))
                    continue;
                sawChartClass = true;
                it = s_rootCache.find(par);
                if (it != s_rootCache.end())
                {
                    if (firstParent)
                    {
                        SetPropW(hWnd, MAKEINTATOM(chartMemoParentAtom), reinterpret_cast<HANDLE>(firstParent));
                        SetPropW(hWnd, MAKEINTATOM(chartMemoResultAtom), reinterpret_cast<HANDLE>(par));
                    }
                    r.cache = &it->second;
                    r.inheritedFromChartGraph = true;
                    return r;
                }
            }
            if (firstParent && !sawChartClass)
            {
                SetPropW(hWnd, MAKEINTATOM(chartMemoParentAtom), reinterpret_cast<HANDLE>(firstParent));
                SetPropW(hWnd, MAKEINTATOM(chartMemoResultAtom),
                    reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1)));
            }
            }
            if (IsRarextWindowModule(hWnd))
            {
                for (HWND par = GetParent(hWnd); par; par = GetParent(par))
                {
                    it = s_rootCache.find(par);
                    if (it != s_rootCache.end())
                    {
                        r.cache = &it->second;
                        return r;
                    }
                }
                const HWND gaRoot = GetAncestor(hWnd, GA_ROOT);
                if (gaRoot && gaRoot != hWnd)
                {
                    it = s_rootCache.find(gaRoot);
                    if (it != s_rootCache.end())
                    {
                        r.cache = &it->second;
                        return r;
                    }
                }
            }
            return r;
        }

        // Один проход: класс + GetWindowModuleFileName. Не вызывает EnableRootHook — только наполняет cache.
        static void ClassifyRootWindow(HWND hWnd, RootHookCache& cache) noexcept
        {
            if (!IsWindow(hWnd) || !GlassCore::ModuleHandler::g_entryPointModule)
                return;

            wchar_t wndModPath[MAX_PATH]{};
            GetWindowModuleFileNameW(hWnd, wndModPath, _countof(wndModPath));
            const std::wstring wndBase = BasenamePtr(wndModPath);

            wchar_t cls[256]{};
            GetClassNameW(hWnd, cls, _countof(cls));

            const HMODULE instMod = reinterpret_cast<HMODULE>(GetWindowLongPtrW(hWnd, GWLP_HINSTANCE));
            const ATOM dlgAtom = static_cast<ATOM>(GetClassLongPtrW(hWnd, GCW_ATOM));
            const bool is32770 = (dlgAtom == static_cast<ATOM>(32770));

            // USER32.dll + класс диалога #32770 — BWM Paint
            if (StrEqI(wndBase.c_str(), L"USER32.dll") && StrEqI(cls, L"#32770"))
                PushPaint(GetModuleHandleW(L"USER32.dll"), OHK_CREATESOLIDBRUSH, cache);

            // DirectUIHWND + DUser + wdc — см. DirectUI::OnDirectUIRootHooksAttach (WndCompat).

            // IEUI + DirectUIHWND — BWM Paint (IEFRAME IAT)
            if (StrEqI(wndBase.c_str(), L"IEUI.dll") && StrEqI(cls, L"DirectUIHWND"))
            {
                std::vector<HookSpec> ieSpecs;
                GetIeUiDirectUiPaintSpecs(ieSpecs);
                for (const auto& sp : ieSpecs)
                    PushPaint(sp.module, sp.index, cache);
            }

            // COMDLG32 диалог — BWM: Paint (CreateSolidBrush/SelectObject/ExtTextOut) + Dc (CreateSolidBrush/SelectObject/Rectangle)
            if (StrEqI(wndBase.c_str(), L"COMDLG32.dll") && is32770)
            {
                void* m = GetModuleHandleW(L"COMDLG32.dll");
                // Paint
                PushPaint(m, OHK_CREATESOLIDBRUSH, cache);
                PushPaint(m, OHK_SELECTOBJECT, cache);
                PushPaint(m, OHK_EXTTEXTOUT, cache);
                // Dc
                PushDc(m, OHK_CREATESOLIDBRUSH, cache);
                PushDc(m, OHK_SELECTOBJECT, cache);
                PushDc(m, OHK_RECTANGLE, cache);
            }

            // ACLUI: список прав доступа — BWM Dc only
            if (StrEqI(wndBase.c_str(), L"ACLUI.dll") && StrEqI(cls, L"CHECKLIST_ACLUI"))
            {
                void* m = GetModuleHandleW(L"ACLUI.dll");
                PushDc(m, OHK_FRAMERECT, cache);
            }

            // inetcpl.cpl — BWM Paint
            if (StrEqI(wndBase.c_str(), L"inetcpl.cpl") && is32770)
                PushPaint(GetModuleHandleW(L"inetcpl.cpl"), OHK_EXTTEXTOUT, cache);

            // rarext.dll (WinRAR zip properties) — custom GDI: GradientFill, LineTo, TextOutW
            if (StrEqI(wndBase.c_str(), L"rarext.dll"))
            {
                PushRarextHooksIfLoaded(cache);
            }
            else if (is32770
                || StrEqI(wndBase.c_str(), L"comctl32.dll")
                || StrEqI(wndBase.c_str(), L"SHELL32.dll"))
            {
                PushRarextHooksIfLoaded(cache);
            }

            // wvc.dll — BWM Paint
            if (StrEqI(wndBase.c_str(), L"wvc.dll"))
            {
                void* m = GetModuleHandleW(L"wvc.dll");
                HWND par = GetParent(hWnd);
                wchar_t pcls[128]{};
                if (par)
                    GetClassNameW(par, pcls, _countof(pcls));
                if (StrEqI(pcls, L"ChartGraphClass"))
                {
                    PushPaint(m, OHK_LINETO, cache);
                    PushPaint(m, OHK_POLYLINE, cache);
                    PushPaint(m, OHK_PATBLT, cache);
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][Root][Cache] wvc.dll дочернее под ChartGraphClass hwnd=0x%p: в paintHooks LinTo/Polyline/PatBlt+ExtTextOut",
                        hWnd);
                }
                PushPaint(m, OHK_EXTTEXTOUT, cache);
            }

            // NetUI / NetUiToolWindow — GlassControlKind + PaintDispatcher (UiRibbonHooks).

            // sysmon.ocx — BWM: Paint (все классы) + Dc (PerfStatus/SysmonReport: ExtTextOut)
            if (StrEqI(cls, L"SysmonCtrl") || StrEqI(cls, L"SysmonReport")
                || StrEqI(cls, L"PerfStatus") || StrEqI(cls, L"PerfLegend"))
            {
                HMODULE sm = GetModuleHandleW(L"sysmon.ocx");
                if (!sm && wndModPath[0])
                    sm = GetModuleHandleW(wndModPath);
                PushPaint(sm, OHK_CREATESOLIDBRUSH, cache);
                PushPaint(sm, OHK_EXTTEXTOUT, cache);
                // Dc — только PerfStatus / SysmonReport
                if (StrEqI(cls, L"PerfStatus") || StrEqI(cls, L"SysmonReport"))
                    PushDc(sm, OHK_EXTTEXTOUT, cache);
            }

            // charmap.exe — BWM: Paint + Dc
            if (StrEqI(wndBase.c_str(), L"charmap.exe"))
            {
                void* m = instMod ? static_cast<void*>(instMod) : static_cast<void*>(GetModuleHandleW(nullptr));
                PushPaint(m, OHK_EXTTEXTOUT, cache);
                PushDc(m, OHK_EXTTEXTOUT, cache);
            }

            // wdc.dll + WdcGraphWindow — BWM: Paint + CompatDc
            if (StrEqI(wndBase.c_str(), L"wdc.dll") && StrEqI(cls, L"WdcGraphWindow"))
            {
                void* m = GetModuleHandleW(L"wdc.dll");
                PushPaint(m, OHK_LINETO, cache);
                PushPaint(m, OHK_POLYGON, cache);
                PushPaint(m, OHK_CREATESOLIDBRUSH, cache);
                PushCompat(m, OHK_LINETO, cache);
                PushCompat(m, OHK_POLYGON, cache);
                PushCompat(m, OHK_CREATESOLIDBRUSH, cache);
            }

            // MFC42u + MMCViewWindow — BWM Paint
            if (StrEqI(wndBase.c_str(), L"MFC42u.dll") && StrEqI(cls, L"MMCViewWindow"))
                PushPaint(GlassCore::ModuleHandler::g_entryPointModule, OHK_GETSYSCOLORBR, cache);

            // ChartGraphClass (MMC): слоты на IAT того модуля, откуда реально вызываются GDI (wvc и/или sysmon.ocx).
            // Снимок «Производительность» часто рисует контейнер графика из sysmon — без IAT на sysmon PatBlt/линии не перехватываются.
            if (StrEqI(cls, L"ChartGraphClass"))
            {
                const auto pushChartForModule = [&](void* mod) noexcept
                {
                    if (!mod)
                        return;
                    PushDc(mod, OHK_PATBLT, cache);
                    PushDc(mod, OHK_SETBKCOLOR, cache);
                    PushDc(mod, OHK_SELECTOBJECT, cache);
                    PushDc(mod, OHK_EXTTEXTOUT, cache);
                    PushDc(mod, OHK_POLYLINE, cache);
                    PushDc(mod, OHK_LINETO, cache);
                    PushPaint(mod, OHK_PATBLT, cache);
                    PushPaint(mod, OHK_SETBKCOLOR, cache);
                    PushPaint(mod, OHK_SELECTOBJECT, cache);
                    PushPaint(mod, OHK_EXTTEXTOUT, cache);
                    PushPaint(mod, OHK_POLYLINE, cache);
                    PushPaint(mod, OHK_LINETO, cache);
                };
                void* wvcM = GetModuleHandleW(L"wvc.dll");
                void* smM = GetModuleHandleW(L"sysmon.ocx");
                pushChartForModule(wvcM);
                pushChartForModule(smM);
                if (!wvcM && !smM)
                {
                    DebugLog(DBG_WARNING,
                        L"[GlassCore][Root][Cache] ChartGraphClass hwnd=0x%p: нет wvc.dll и sysmon.ocx — график без root IAT",
                        hWnd);
                }
                else
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][Root][Cache] ChartGraphClass hwnd=0x%p: paint+dc для модулей wvc=%d sysmon=%d",
                        hWnd, wvcM ? 1 : 0, smM ? 1 : 0);
                }
            }

            // MFC AfxWnd42u: BitBlt на comctl32 — BWM CompatibleDc only.
            // Оба экземпляра (v6 + legacy): ресайз дисковой утилиты рисует через
            // неперехваченный экземпляр, если хук только на GetModuleHandleW.
            if (StrEqI(cls, L"AfxWnd42u"))
            {
                PushCompatComctl32(OHK_BITBLT, cache);
            }

            // AfxWnd42u (Disk Management, dmview.ocx): сессионные IAT на реальных
            // рисующих модулях. BWM: ModuleHandler load-time EXTTEXTOUT+FILLRECT на
            // dmview/dmdskmgr + SETBKCOLOR на MFC42u (по MSCTF); в Glass (D1) — только
            // сессии BeginPaint/GetDC этого HWND.
            if (StrEqI(cls, L"AfxWnd42u"))
            {
                if (void* mfc = GetModuleHandleW(L"MFC42u.dll"))
                {
                    PushPaint(mfc, OHK_SETBKCOLOR, cache);
                    PushPaint(mfc, OHK_EXTTEXTOUT, cache);
                    PushPaint(mfc, OHK_CREATESOLIDBRUSH, cache);
                    PushDc(mfc, OHK_SETBKCOLOR, cache);
                    PushDc(mfc, OHK_EXTTEXTOUT, cache);
                    PushDc(mfc, OHK_CREATESOLIDBRUSH, cache);
                    // Ловля серого фона: тела log-only (passthrough вне chart).
                    PushPaint(mfc, OHK_PATBLT, cache);
                    PushPaint(mfc, OHK_RECTANGLE, cache);
                    PushDc(mfc, OHK_PATBLT, cache);
                    PushDc(mfc, OHK_RECTANGLE, cache);
                    // BITBLT-проба: тело log-only вне 16x16-comctl32.
                    PushPaint(mfc, OHK_BITBLT, cache);
                    PushDc(mfc, OHK_BITBLT, cache);
                    // GRADIENTFILL-проба: тело log-only вне rarext.
                    PushPaint(mfc, OHK_GRADIENTFILL, cache);
                    PushDc(mfc, OHK_GRADIENTFILL, cache);
                    // FILLRECT на MFC42u: ищем заливки фона мимо dmview
                    // (тело для MFC42u+AfxWnd42u — passthrough, только лог).
                    PushPaint(mfc, OHK_FILLRECT, cache);
                    PushDc(mfc, OHK_FILLRECT, cache);
                }
                if (void* dmv = GetModuleHandleW(L"dmview.ocx"))
                {
                    PushPaint(dmv, OHK_EXTTEXTOUT, cache);
                    PushPaint(dmv, OHK_FILLRECT, cache);
                    PushDc(dmv, OHK_EXTTEXTOUT, cache);
                    PushDc(dmv, OHK_FILLRECT, cache);
                    // Ловля серого фона: тела log-only.
                    PushPaint(dmv, OHK_PATBLT, cache);
                    PushPaint(dmv, OHK_RECTANGLE, cache);
                    PushDc(dmv, OHK_PATBLT, cache);
                    PushDc(dmv, OHK_RECTANGLE, cache);
                    // BITBLT-проба: тело log-only вне 16x16-comctl32.
                    PushPaint(dmv, OHK_BITBLT, cache);
                    PushDc(dmv, OHK_BITBLT, cache);
                    // GRADIENTFILL-проба: тело log-only вне rarext.
                    PushPaint(dmv, OHK_GRADIENTFILL, cache);
                    PushDc(dmv, OHK_GRADIENTFILL, cache);
                }
                if (void* dmd = GetModuleHandleW(L"dmdskmgr.dll"))
                {
                    PushPaint(dmd, OHK_EXTTEXTOUT, cache);
                    PushDc(dmd, OHK_EXTTEXTOUT, cache);
                }
                // Слоты на comctl32 — на оба экземпляра (v6 + legacy), см. PushComctl32*.
                PushPaintComctl32(OHK_SETBKCOLOR, cache);
                PushPaintComctl32(OHK_EXTTEXTOUT, cache);
                PushDcComctl32(OHK_SETBKCOLOR, cache);
                PushDcComctl32(OHK_EXTTEXTOUT, cache);
                // FILLRECT на comctl32: тело для comctl32+AfxWnd42u — passthrough.
                PushPaintComctl32(OHK_FILLRECT, cache);
                PushDcComctl32(OHK_FILLRECT, cache);
            }

            // WinForms .Window — BWM Paint (Detours-хук, не IAT)
            if (StrEqI(wndBase.c_str(), L"System.Windows.Forms.ni.dll") && IsWinFormsDotWindow(hWnd))
                cache.winFormsDrawTextEx = true;

            // Точка входа процесса — типичные диалоги / сценарии владельца — BWM Paint (+ HEX: Paint+Dc)
            if (instMod == GlassCore::ModuleHandler::g_entryPointModule)
            {
                void* ep = GlassCore::ModuleHandler::g_entryPointModule;
                PushPaint(ep, OHK_SETBKCOLOR, cache);
                PushPaint(ep, OHK_DRAWTEXT, cache);
                PushPaint(ep, OHK_GETSYSCOLORBR, cache);
                PushPaint(ep, OHK_CREATESOLIDBRUSH, cache);
                // BWM Paint, ветка «else // window», «callerModule == EntryPointModule»:
                // «else if (szModuleName == msinfo32.exe)» — там szModuleName это basename
                // caller-модуля BeginPaint (т.е. имя EXE точки входа), а не модуль окна.
                // wndBase для #32770-диалога обычно USER32.dll, поэтому проверка по wndBase
                // здесь никогда не срабатывала и слот MFC42u!DrawTextW не включался —
                // заголовок «Выберите подкатегорию» (DrawTextW из msinfo32.exe) уходил мимо.
                wchar_t epPath[MAX_PATH]{};
                GetModuleFileNameW(
                    GlassCore::ModuleHandler::g_entryPointModule,
                    epPath, _countof(epPath));
                const std::wstring epBase = BasenamePtr(epPath);
                if (StrEqI(cls, L"HEX"))
                {
                    PushPaint(ep, OHK_EXTTEXTOUT, cache);
                    PushDc(ep, OHK_EXTTEXTOUT, cache);
                }
                else if (StrEqI(epBase.c_str(), L"msinfo32.exe"))
                {
                    void* mfc = GetModuleHandleW(L"MFC42u.dll");
                    if (mfc)
                        PushPaint(mfc, OHK_DRAWTEXT, cache);
                }
            }
        }

        // ── Провайдеры PaintDispatcher (подписка в Root::Startup) ───────────────
        //
        // HookBeginPaint обходит g_paintProviders вместе с Button/Static/… — порядок регистрации
        // до PaintDispatcher::Startup см. CLAUDE.md. Здесь нет if по классу окна — только s_rootCache.

        // EnableRootHook держит ref-count на паре (module, OHK) в HookDispatcher.
        static void EnableSpecs(const std::vector<HookSpec>& specs, bool enable) noexcept
        {
            for (auto& [mod, idx] : specs)
                EnableRootHook(mod, idx, enable);
        }

        // --- Канал 1: BeginPaint / EndPaint (аналог PaintHandler::Paint) ---

        static void Root_OnBeginPaint(HWND hWnd, HMODULE callerMod) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire)) return;
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd)) return;

            if (IsRarextWindowModule(hWnd))
                EnsureRarextHookCache(hWnd);

            RootPaintSession session{};
            session.hWnd = hWnd;
            bool chartInherit = false;
            {
                auto lock = s_cacheLock.lock_shared();
                const RootCacheLookupResult lr = LookupRootHookCacheLocked(hWnd);
                if (!lr.cache)
                {
                    // Catch-up: CREATE окна потерян (гонка холодного старта) — top-level
                    // окно рисуется без attach: ни сабкласса, ни хуков, ни бэкдропа.
                    // Просим WndCompat аттачить (идемпотентно, Full only внутри).
                    if (GetAncestor(hWnd, GA_ROOT) == hWnd)
                        GlassCore::Root::RequestRootAttachForHwnd(hWnd);
                    return;
                }
                chartInherit = lr.inheritedFromChartGraph;
                session.specs = lr.cache->paintHooks;
                session.winFormsDrawTextEx = lr.cache->winFormsDrawTextEx;
            }
            // BWM PaintHandler.cpp, ветка «window», callerModule == EntryPointModule:
            // фон окон и #32770-диалогов, рисуемых кодом самого EXE (dfrgui.exe),
            // идёт через SetBkColor из точки входа. Classify статичен по HINSTANCE
            // окна и такой caller не видит — включаем динамически, per-сессия.
            // Только Paint-канал, только OHK_SETBKCOLOR (scope задачи).
            bool wantEntrySetBk = false;
            if (callerMod && GlassCore::ModuleHandler::g_entryPointModule
                && callerMod == GlassCore::ModuleHandler::g_entryPointModule)
            {
                void* ep = GlassCore::ModuleHandler::g_entryPointModule;
                bool already = false;
                for (const auto& sp : session.specs)
                {
                    if (sp.module == ep && sp.index == OHK_SETBKCOLOR)
                    {
                        already = true;
                        break;
                    }
                }
                if (!already)
                    wantEntrySetBk = true;
            }
            if (session.specs.empty() && !session.winFormsDrawTextEx && !wantEntrySetBk)
                return;
            wchar_t cn[256]{};
            GetClassNameW(hWnd, cn, _countof(cn));
            if (IsMmcChartSessionClass(cn))
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][Root][Session] BeginPaint: hwnd=0x%p class=\"%s\" paintSpecs=%zu winFormsDT=%d chartInherit=%d → EnableSpecs(true)",
                    hWnd, cn, session.specs.size(), session.winFormsDrawTextEx ? 1 : 0, chartInherit ? 1 : 0);
            }
            wchar_t wndModPathGlow[MAX_PATH]{};
            GetWindowModuleFileNameW(hWnd, wndModPathGlow, _countof(wndModPathGlow));
            if (StrEqI(BasenamePtr(wndModPathGlow).c_str(), L"rarext.dll"))
            {
                TextGlow::RegisterTextGlowClient(
                    hWnd,
                    TextGlow::TextGlowSpreadCategory::Global);
            }
            EnableSpecs(session.specs, true);
            if (wantEntrySetBk)
            {
                EnableRootHook(GlassCore::ModuleHandler::g_entryPointModule, OHK_SETBKCOLOR, true);
                session.entryPointSetBkColor = true;
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][Root][Session] BeginPaint: hwnd=0x%p entryPoint SetBkColor on (caller==entry-point)",
                    hWnd);
            }
            if (session.winFormsDrawTextEx)
                GlassCore::FormsHooks::DrawTextExAttach(true);
            g_rootPaintStack.push_back(std::move(session));
        }

        static void Root_OnEndPaint(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            // P5: снимаем сессию именно этого HWND (поиск с конца — обычный случай O(1)).
            // Вершина чужого окна означает внеочередное закрытие: её не трогаем.
            // Пары enable/disable сохраняются — чужие IAT больше не мигают.
            for (auto it = g_rootPaintStack.rbegin(); it != g_rootPaintStack.rend(); ++it)
            {
                if (it->hWnd != hWnd) continue;
                auto session = std::move(*it);
                g_rootPaintStack.erase((it + 1).base());
                EnableSpecs(session.specs, false);
                if (session.entryPointSetBkColor)
                    EnableRootHook(GlassCore::ModuleHandler::g_entryPointModule, OHK_SETBKCOLOR, false);
                if (session.winFormsDrawTextEx)
                    GlassCore::FormsHooks::DrawTextExAttach(false);
                return;
            }
            // Пары для этого HWND нет (открытие было без кеша/спеков) — тихий выход.
        }

        // --- Канал 2: GetDC / ReleaseDC (аналог PaintHandler::Dc) ---

        static void Root_OnGetDC(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire)) return;
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd)) return;
            if (IsRarextWindowModule(hWnd))
                EnsureRarextHookCache(hWnd);
            RootHookSession session{};
            session.hWnd = hWnd;
            {
                auto lock = s_cacheLock.lock_shared();
                const RootCacheLookupResult lr = LookupRootHookCacheLocked(hWnd);
                if (!lr.cache) return;
                session.specs = lr.cache->dcHooks;
            }
            if (session.specs.empty()) return;
            wchar_t cn[256]{};
            GetClassNameW(hWnd, cn, _countof(cn));
            if (IsMmcChartSessionClass(cn))
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][Root][Session] GetDC: hwnd=0x%p class=\"%s\" dcSpecs=%zu → EnableSpecs(true) (канал Dc)",
                    hWnd, cn, session.specs.size());
            }
            EnableSpecs(session.specs, true);
            g_rootDcStack.push_back(std::move(session));
        }

        static void Root_OnReleaseDC(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            // P5: как в Root_OnEndPaint — пара по HWND, чужую вершину не трогаем.
            for (auto it = g_rootDcStack.rbegin(); it != g_rootDcStack.rend(); ++it)
            {
                if (it->hWnd != hWnd) continue;
                auto session = std::move(*it);
                g_rootDcStack.erase((it + 1).base());
                EnableSpecs(session.specs, false);
                return;
            }
            // Пары для этого HWND нет — тихий выход.
        }

        // --- Канал 3: CreateCompatibleDC / DeleteDC (аналог CompatibleDc) ---

        static void Root_OnCreateCompatDC(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire)) return;
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd)) return;
            RootHookSession session{};
            session.hWnd = hWnd;
            {
                auto lock = s_cacheLock.lock_shared();
                const RootCacheLookupResult lr = LookupRootHookCacheLocked(hWnd);
                if (!lr.cache) return;
                session.specs = lr.cache->compatDcHooks;
            }
            if (session.specs.empty()) return;
            EnableSpecs(session.specs, true);
            g_rootCompatDcStack.push_back(std::move(session));
        }

        static void Root_OnDeleteDC(HWND hWnd, HMODULE /*callerMod*/) noexcept
        {
            // P5: как в Root_OnEndPaint — пара по HWND, чужую вершину не трогаем.
            for (auto it = g_rootCompatDcStack.rbegin(); it != g_rootCompatDcStack.rend(); ++it)
            {
                if (it->hWnd != hWnd) continue;
                auto session = std::move(*it);
                g_rootCompatDcStack.erase((it + 1).base());
                EnableSpecs(session.specs, false);
                return;
            }
            // Пары для этого HWND нет — тихий выход.
        }

    // ── Lifecycle ────────────────────────────────────────────────────────

    // RootEventProvider: RootSubclass вызывает сразу после SetWindowSubclass.
    // Только кеш; IAT включатся при первом BeginPaint / GetDC / CreateCompatibleDC для HWND.
    void OnRootHooksAttach(HWND hRoot) noexcept
    {
        if (!hRoot || g_shutdown.load(std::memory_order_acquire))
            return;
        if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            GlassCore::ExplorerFrameControl::OnExplorerRootAttach(hRoot);
        
        RootHookCache cache;
        ClassifyRootWindow(hRoot, cache);

        if (cache.paintHooks.empty() && cache.dcHooks.empty()
            && cache.compatDcHooks.empty() && !cache.winFormsDrawTextEx)
            return;

        // Предпрогрев IAT-кешей модулей из спеков (только чтение) — холодный скан PE
        // уходит из первого paint-кадра в attach окна. Пустые модули пропускаются.
        // Прогрев — строго после освобождения s_cacheLock (PrewarmRootHook берёт
        // свой g_rootLock; вложенность запрещена).
        std::vector<void*> prewarmModules;
        size_t paintCount = 0, dcCount = 0, compatCount = 0;
        bool winForms = false;
        {
            auto lock = s_cacheLock.lock_exclusive();
            auto& entry = s_rootCache[hRoot];
            // Мержим — DirectUI может добавить через AppendRootPaintSpecs раньше
            entry.paintHooks.insert(entry.paintHooks.end(), cache.paintHooks.begin(), cache.paintHooks.end());
            entry.dcHooks.insert(entry.dcHooks.end(), cache.dcHooks.begin(), cache.dcHooks.end());
            entry.compatDcHooks.insert(entry.compatDcHooks.end(), cache.compatDcHooks.begin(), cache.compatDcHooks.end());
            entry.winFormsDrawTextEx |= cache.winFormsDrawTextEx;
            prewarmModules.reserve(
                entry.paintHooks.size() + entry.dcHooks.size() + entry.compatDcHooks.size());
            for (auto& [mod, idx] : entry.paintHooks) { (void)idx; if (mod) prewarmModules.push_back(mod); }
            for (auto& [mod, idx] : entry.dcHooks) { (void)idx; if (mod) prewarmModules.push_back(mod); }
            for (auto& [mod, idx] : entry.compatDcHooks) { (void)idx; if (mod) prewarmModules.push_back(mod); }
            paintCount = entry.paintHooks.size();
            dcCount = entry.dcHooks.size();
            compatCount = entry.compatDcHooks.size();
            winForms = entry.winFormsDrawTextEx;
        }
        for (void* mod : prewarmModules)
            PrewarmRootHook(mod);

        wchar_t cn[256]{};
        GetClassNameW(hRoot, cn, _countof(cn));
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Root] Attach: hWnd=0x%p class=\"%s\" paint=%zu dc=%zu compat=%zu winForms=%d",
            hRoot, cn,
            paintCount, dcCount, compatCount,
            winForms ? 1 : 0);
        if (IsMmcChartSessionClass(cn))
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Root][Attach][MMC] итог кеша для hwnd=0x%p: paint=%zu dc=%zu compat=%zu (проверка сессий BeginPaint/GetDC)",
                hRoot, paintCount, dcCount, compatCount);
        }
    }

    // Стереть кеш; активные сессии на потоке должны завершиться EndPaint / ReleaseDC / DeleteDC.
    void OnRootHooksDetach(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            GlassCore::ExplorerFrameControl::OnExplorerRootDetach(hRoot);
        auto lock = s_cacheLock.lock_exclusive();
        s_rootCache.erase(hRoot);
    }

    // Аналог BWM WndList::IsDataAttach: окно тематизируется, только если attach'нуто.
    // Используется always-on хуками (D2) как guard вместо сессионного контекста.
    bool HasRootHookCache(HWND hWnd) noexcept
    {
        if (!hWnd)
            return false;
        auto lock = s_cacheLock.lock_shared();
        return s_rootCache.find(hWnd) != s_rootCache.end();
    }

    // Engine::Startup: регистрируем три канала в PaintDispatcher.
    void Startup() noexcept
    {
        /*using namespace Hooks::DebugDetourHooks;
        (void)SetEnabled(DDH_SetBkColor | DDH_FillRect, true);*/
        // Предпрогрев IAT точки входа для динамического SETBKCOLOR
        // (BWM window / caller == EntryPoint): холодный скан PE уходит из первого кадра.
        if (void* ep = GlassCore::ModuleHandler::g_entryPointModule)
            PrewarmRootHook(ep);
        GlassCore::PaintDispatcher::RegisterPaintProvider({ Root_OnBeginPaint, Root_OnEndPaint });
        GlassCore::PaintDispatcher::RegisterGetDCProvider({ Root_OnGetDC, Root_OnReleaseDC });
        GlassCore::PaintDispatcher::RegisterCompatDCProvider({ Root_OnCreateCompatDC, Root_OnDeleteDC });
        DebugLog(DBG_IMPORTANT, L"[GlassCore][Root] Startup: провайдеры PaintDispatcher зарегистрированы");
    }

    // Платформа: очистить кеш, стеки текущего потока, снять все root IAT (DisableAllRootHooks).
    void Shutdown() noexcept
    {
        /*using namespace Hooks::DebugDetourHooks;
        (void)SetEnabled(DDH_SetBkColor | DDH_FillRect, false);*/
        g_shutdown.store(true, std::memory_order_release);
        SetRootGdiShutdown(true);

        {
            auto lock = s_cacheLock.lock_exclusive();
            s_rootCache.clear();
        }

        // Стеки — thread_local, очищаем текущий поток
        g_rootPaintStack.clear();
        g_rootDcStack.clear();
        g_rootCompatDcStack.clear();

        DisableAllRootHooks();
        SetRootGdiShutdown(false);
        
        g_shutdown.store(false, std::memory_order_release);
    }

    // DirectUI: WndCompat передаёт спеку в paint через AppendRootPaintSpecs.
    void GetDUserWdcPaintSpecs(void* duserModule, std::vector<HookSpec>& out) noexcept
    {
        if (!duserModule)
            return;
        out.push_back({ duserModule, OHK_LINETO });
        out.push_back({ duserModule, OHK_POLYLINE });
        out.push_back({ duserModule, OHK_POLYGON });
        out.push_back({ duserModule, OHK_PATBLT });
    }

    void GetIeUiDirectUiPaintSpecs(std::vector<HookSpec>& out) noexcept
    {
        void* const frame = GetModuleHandleW(L"IEFRAME.dll");
        if (!frame)
            return;
        out.push_back({ frame, OHK_GETSYSCOLORBR });
        out.push_back({ frame, OHK_DRAWTEXTEX });
        out.push_back({ frame, OHK_DRAWTEXT });
    }

    // BWM CompatibleDc: wdc + DirectUIHWND → WinHooks DrawThemeText на comctl32.
    // В Glass нет DrawThemeText в g_rootHookDep; тот же сценарий покрывается Detours на uxtheme
    // (DirectUIHooks) с фильтром IsDuiModule(wdc) — отдельный PushCompat не добавляем, чтобы не дублировать.
    void AppendDirectUiRootPaintSpecsForHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || g_shutdown.load(std::memory_order_acquire) || !GlassCore::ModuleHandler::g_entryPointModule)
            return;

        WCHAR cls[256]{};
        if (GetClassNameW(hWnd, cls, _countof(cls)) <= 0)
            return;
        if (!StrEqI(cls, L"DirectUIHWND"))
            return;

        wchar_t wndModPath[MAX_PATH]{};
        if (GetWindowModuleFileNameW(hWnd, wndModPath, _countof(wndModPath)) <= 0)
            wndModPath[0] = L'\0';
        const std::wstring wndBase = BasenamePtr(wndModPath);

        std::vector<HookSpec> chunk;

        if (StrEqI(wndBase.c_str(), L"IEUI.dll"))
            GetIeUiDirectUiPaintSpecs(chunk);
        else
        {
            HMODULE duser = GetModuleHandleW(L"DUser.dll");
            if (duser && GetModuleHandleW(L"wdc.dll"))
                GetDUserWdcPaintSpecs(reinterpret_cast<void*>(duser), chunk);
        }

        if (!chunk.empty())
            AppendRootPaintSpecs(hWnd, std::move(chunk));
    }

    // Дописать в paintHooks уже закешированного root (мерж с ClassifyRootWindow в OnRootHooksAttach).
    void AppendRootPaintSpecs(HWND hRoot, std::vector<HookSpec> specs) noexcept
    {
        if (!hRoot || specs.empty() || g_shutdown.load(std::memory_order_acquire))
            return;

        auto lock = s_cacheLock.lock_exclusive();
        auto& entry = s_rootCache[hRoot];
        entry.paintHooks.insert(entry.paintHooks.end(), specs.begin(), specs.end());
    }

} // namespace GlassCore::Root
