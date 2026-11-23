"""Portable source-contract checks; Windows paint/lifecycle tests are still required."""
from pathlib import Path
import os
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]

def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

class DelphiPipelineContract(unittest.TestCase):
    def test_vcl_control_classification(self):
        source = read("WndCompat/Tracker/WindowClassifier.cpp")
        expected = {
            "TPageControl": "TabControl", "TStaticText": "Static",
            "TTrackBar": "TrackBar", "TScrollBar": "ScrollBar",
            "TDateTimePicker": "DateTimePicker", "TToolBar": "ToolBar",
            "TGroupBox": "DelphiContainer", "TTabSheet": "DelphiContainer",
            "TCard": "DelphiContainer", "TRadioGroup": "DelphiContainer",
            "TGroupButton": "Button",
        }
        for name, kind in expected.items():
            with self.subTest(name=name):
                self.assertRegex(source, r'(?s)if\s*\([^;{}]*L"' + name +
                                 r'"[^;{}]*\)\s*return ControlKind::' + kind + r'\s*;')

    def test_button_groups_have_shared_exact_gate_and_scoped_exe_slots(self):
        classes = read('GlassShared/include/GlassDelphiButtonGroups.h')
        for cls in ('TButtonGroup', 'TCategoryButtons'):
            self.assertIn('_wcsicmp(name, L"' + cls + '") == 0', classes)
        for forbidden in ('wcsstr', 'wcsncmp', 'find(', 'TGroupButton', 'TButton"'):
            self.assertNotIn(forbidden, classes)
        self.assertIn('GlassShared::IsDelphiButtonGroupsClassName(cls.c_str())',
                      read('WndCompat/Tracker/WindowClassifier.cpp'))
        root = read('GlassCore/src/Hooks/RootWindowHooks.cpp')
        gate = root.split('// Custom-painted VCL groups:', 1)[1].split('// VCL-таблицы', 1)[0]
        self.assertIn('instMod == GlassCore::ModuleHandler::g_entryPointModule', gate)
        self.assertIn('GlassShared::IsDelphiButtonGroupsClassName(cls)', gate)
        for slot in ('TEXT', 'FILL', 'GRADIENT', 'GDIGRADIENT'):
            self.assertIn('OHK_DELPHI_GROUP_' + slot, gate)
            self.assertIn('OHK_DELPHI_GROUP_' + slot + ',', read('GlassCore/Include/Hooks/RootGdiHooks.h'))
        self.assertIn('PushPaint(instMod, slot, cache)', gate)
        self.assertIn('PushDc(instMod, slot, cache)', gate)
        self.assertNotIn('PushCompat', gate)
        code = read('GlassCore/src/Hooks/delphi/TButtonGroupsDraw.cpp')
        for guard in ('WindowFromDC(dc)', 'GetStackOwnerForDc(dc)', 'IsTopWindowDrawingSession(hwnd)',
                      'GlassIsBackdropAllowedForHwnd(hwnd)', 'IsFullInjectionMode()',
                      'GetMapMode(dc) != MM_TEXT', 'g_inside', 'IsDelphiButtonGroupsWindow(hwnd)'):
            self.assertIn(guard, code)
        self.assertNotIn('GetPaintSessionHostHwnd', code)
        self.assertNotIn('TButtonGroup', read('GlassShared/include/GlassDelphiPanel.h'))

    def test_button_group_thunks_resolve_original_from_each_installed_exe_slot(self):
        code = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        wrappers = code.split('// Dedicated EXE slots:', 1)[1].split('BOOL WINAPI Root_MyExtTextOutW(', 1)[0]
        self.assertEqual(wrappers.count('GlassCore::ModuleHandler::g_entryPointModule'), 3)
        for marker in ('GetRootOrg<OHK_DELPHI_GROUP_TEXT, decltype(&::DrawTextExW)>',
                       'GetRootOrg<OHK_DELPHI_GROUP_FILL, decltype(&::FillRect)>',
                       'GetRootOrg<slot, decltype(&::GdiGradientFill)>', 'g_shutdown.load',
                       'GlassCore::DelphiButtonGroups::DrawCaptionText', 'GlassCore::DelphiButtonGroups::Fill',
                       'GlassCore::DelphiButtonGroups::Gradient'):
            self.assertIn(marker, wrappers)
        self.assertNotIn('_ReturnAddress', wrappers)
        self.assertNotIn('GetCallerModule', wrappers)
        for api, hook in (('DrawTextExW', 'Root_MyDelphiGroupText'), ('FillRect', 'Root_MyDelphiGroupFill'),
                          ('GradientFill', 'Root_MyDelphiGroupGradient<OHK_DELPHI_GROUP_GRADIENT>'),
                          ('GdiGradientFill', 'Root_MyDelphiGroupGradient<OHK_DELPHI_GROUP_GDIGRADIENT>')):
            self.assertIn('"' + api + '", reinterpret_cast<PVOID>(' + hook + ')', code)
        enum = read('GlassCore/Include/Hooks/RootGdiHooks.h').split('enum RootHookIndex : size_t', 1)[1].split('OHK_COUNT', 1)[0]
        table = code.split('g_rootHookDep', 1)[1].split('using RootDispatcher', 1)[0]
        for release in (True, False):
            names = re.findall(r'\bOHK_\w+', enum.split('#if defined(_DEBUG)', 1)[0] if release else enum)
            rows = re.findall(r'"([A-Za-z][A-Za-z0-9_]*)",\s*reinterpret_cast<PVOID>',
                              table.split('#if defined(_DEBUG)', 1)[0] if release else table)
            self.assertEqual(len(names), len(rows), ('release' if release else 'debug', names, rows))
            start = names.index('OHK_DELPHI_GROUP_TEXT')
            self.assertEqual(rows[start:start + 4], ['DrawTextExW', 'FillRect', 'GradientFill', 'GdiGradientFill'])

    def test_button_groups_keep_native_gradient_rgb_and_transparent_text_overlay(self):
        code = read('GlassCore/src/Hooks/delphi/TButtonGroupsDraw.cpp')
        for marker in ('kBackgroundAlpha = 100', 'GetSysColor(COLOR_WINDOW)', 'GetSysColor(COLOR_BTNFACE)',
                       'BS_SOLID', 'COLOR_MENUBAR + 1u', 'Blacken(dc, *rect)',
                       'DrawTextWithAlpha(dc, text, count, rect, flags, result, true)',
                       'DT_CALCRECT | DT_MODIFYSTRING', 'count == 0 || params',
                       'vertexCount != 2 || meshCount != 1',
                       'GRADIENT_FILL_RECT_H', 'GRADIENT_FILL_RECT_V',
                       'pair.UpperLeft >= vertexCount', 'pair.LowerRight >= vertexCount',
                       'original(memory, vertices, vertexCount, mesh, meshCount, mode)',
                       'BufferedPaintSetAlpha(buffer, nullptr, 255)', 'GdiFlush()',
                       'kBackgroundAlpha, false, true, false'):
            self.assertIn(marker, code)
        for forbidden in ('CreateSolidColorBrushWithAlpha', 'ApplyBlackKeyAlpha', 'vertices[0].Alpha =',
                          'ColorTreatAsTransparent', 'RegisterPaintProvider', 'RegisterCompatDCProvider'):
            self.assertNotIn(forbidden, code)
        for project in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            self.assertIn(r'src\Hooks\delphi\TButtonGroupsDraw.cpp', read(project))
            self.assertIn(r'Include\Hooks\delphi\TButtonGroupsDraw.h', read(project))
        for project in ('GlassShared/GlassShared.vcxproj', 'GlassShared/GlassShared.vcxproj.filters'):
            self.assertIn(r'include\GlassDelphiButtonGroups.h', read(project))

    def test_button_groups_keep_actual_rgb_even_under_nested_generic_root_paint(self):
        root = read('GlassCore/src/Hooks/RootWindowHooks.cpp')
        self.assertIn('&& !GlassShared::IsDelphiButtonGroupsClassName(cls)', root)
        self.assertIn('&& !GlassShared::IsDelphiButtonGroupsWindow(hWnd)', root)
        gdi = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        bk = gdi.split('COLORREF WINAPI Root_MySetBkColor(HDC hdc', 1)[1].split('HBRUSH WINAPI Root_MyGetSysColorBrush(', 1)[0]
        self.assertIn('GlassShared::IsDelphiButtonGroupsClassName(cls)) return o(hdc, color)', bk)
        for start, end, passthrough in (
            ('HBRUSH WINAPI Root_MyGetSysColorBrush(int nIndex)', 'HBRUSH WINAPI Root_MyCreateSolidBrush(COLORREF color)', 'return o(nIndex)'),
            ('HBRUSH WINAPI Root_MyCreateSolidBrush(COLORREF color)', 'HGDIOBJ WINAPI Root_MySelectObject(', 'return o(color)'),
        ):
            body = gdi.split(start, 1)[1].split(end, 1)[0]
            self.assertIn('caller == GlassCore::ModuleHandler::g_entryPointModule', body)
            self.assertIn('IsDelphiButtonGroupsWindow(GlassCore::PaintDispatcher::GetPaintSessionHostHwnd())', body)
            self.assertIn(passthrough, body)

    def test_iat_walk_searches_every_descriptor_with_the_same_dll_name(self):
        code = read('GlassCore/src/Core/Intercept/HookHelper.cpp')
        walk = code.split('void WalkIAT(', 1)[1].split('void WalkDelayloadIAT(', 1)[0]
        delay = code.split('void WalkDelayloadIAT(', 1)[1].split('void** GetIAT(', 1)[0]
        for body, head in ((walk, 'for (auto desc = importDesc; desc->Name; ++desc)'),
                           (delay, 'for (auto desc = importDesc; desc->DllNameRVA; ++desc)')):
            self.assertIn(head, body)
            self.assertNotIn('found = true', body)
            self.assertIn('if (!SafeCStringEqualI(moduleName, dllName.data()))\n                continue;', body)
            self.assertIn('if (stop)\n                return;', body)
        self.assertIn('if (desc->Attributes.RvaBased != 1)\n                continue;', delay)
        self.assertEqual(code.count('if (stop)\n                return;'), 2)

    def test_bome_memo_exact_process_classes_and_no_generic_half_route(self):
        code = read('GlassCore/src/Hooks/delphi/BomeMemoDraw.cpp')
        gate = code.split('bool IsClassName(', 1)[1].split('void BeginSession(', 1)[0]
        for cls in ('TBomeMemo', 'TBMGutter'):
            self.assertIn('_wcsicmp(name, L"' + cls + '")', gate)
        self.assertIn('GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)', gate)
        self.assertNotIn('Restorator.exe', gate)
        self.assertNotIn('GetModuleFileNameW', gate)
        for forbidden in ('wcsstr', 'wcsncmp', 'find(', 'L"TMemo"', 'L"Edit"'):
            self.assertNotIn(forbidden, gate)
        root = read('GlassCore/src/Hooks/RootWindowHooks.cpp')
        self.assertIn('cache.bomeMemo = instMod == GlassCore::ModuleHandler::g_entryPointModule', root)
        self.assertIn('&& !cache.bomeMemo)', root)
        self.assertIn('if (!cache.bomeMemo)\n                    PushPaint(ep, OHK_DRAWTEXT, cache)', root)
        self.assertIn('&& !GlassCore::BomeMemo::IsWindow(hWnd)', root)
        self.assertIn('entry.bomeMemo |= cache.bomeMemo', root)
        gdi = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        self.assertIn('BomeMemo::IsWindow(hFromDc)) return o(hdc, color)', gdi)
        self.assertEqual(gdi.count('BomeMemo::IsWindow(GlassCore::PaintDispatcher::GetPaintSessionHostHwnd())'), 3)
        thunks = gdi.split('// Bome memo EXE thunks:', 1)[1].split('// Dedicated EXE slots:', 1)[0]
        self.assertEqual(thunks.count('GlassCore::ModuleHandler::g_entryPointModule'), 4)
        self.assertNotIn('_ReturnAddress', thunks)
        self.assertNotIn('GetCallerModule', thunks)
        for slot, api in (('TEXT', 'DrawTextA'), ('BRUSH', 'CreateBrushIndirect'),
                          ('COLOR', 'GetSysColor'), ('FILL', 'FillRect')):
            self.assertIn('GetRootOrg<OHK_BOME_MEMO_' + slot + ', decltype(&::' + api + ')>', thunks)
        enum = read('GlassCore/Include/Hooks/RootGdiHooks.h').split('enum RootHookIndex : size_t', 1)[1].split('#if defined(_DEBUG)', 1)[0]
        names = re.findall(r'\bOHK_\w+', enum)
        rows = re.findall(r'"([A-Za-z][A-Za-z0-9_]*)",\s*reinterpret_cast<PVOID>',
                          gdi.split('g_rootHookDep', 1)[1].split('#if defined(_DEBUG)', 1)[0])
        start = names.index('OHK_BOME_MEMO_TEXT')
        self.assertEqual(rows[start:start+4], ['DrawTextA', 'CreateBrushIndirect', 'GetSysColor', 'FillRect'])

    def test_bome_memo_transaction_native_barrier_and_both_channels(self):
        gdi = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        acquire = gdi.split('unsigned AcquireBomeMemoHooks()', 1)[1].split('void ReleaseBomeMemoHooks()', 1)[0]
        for required in ('g_rootLock.lock_exclusive()', 'const int before = it->second.GetRef(slot)',
                         'it->second.GetRef(slot) > before', 'mask != GlassCore::BomeMemo::kReadyMask',
                         'if (mask & (1u << i)) it->second.EnableHook(kBomeMemoSlots[i], false)'):
            self.assertIn(required, acquire)
        root = read('GlassCore/src/Hooks/RootWindowHooks.cpp')
        for begin, end, paint in (('Root_OnBeginPaint', 'Root_OnEndPaint', 'true'),
                                  ('Root_OnGetDC', 'Root_OnReleaseDC', 'false')):
            body = root.split('static void ' + begin + '(', 1)[1].split('static void ' + end + '(', 1)[0]
            self.assertIn('callerMod == GlassCore::ModuleHandler::g_entryPointModule', body)
            self.assertIn('? AcquireBomeMemoHooks() : 0', body)
            self.assertIn('BomeMemo::BeginSession(hWnd, ' + paint + ', mask)', body)
            self.assertIn('BomeMemo::EndSession(hWnd, ' + paint + ')', root)
        self.assertIn('RegisterPostPaintProvider({ Root_OnBomeMemoAfterBeginPaint })', root)
        self.assertIn('GlassCore::BomeMemo::ClearSessions()', root)
        code = read('GlassCore/src/Hooks/delphi/BomeMemoDraw.cpp')
        # VCL keeps canvas DCs until the message ends, so sessions of the memo and of its gutter are open side by
        # side. Readiness is per window (its newest session decides, an unarmed one is a barrier); a call is
        # authorized by the DC it draws on, not by the session that opened last.
        served = code.split('Session* Served(HWND hwnd) noexcept', 1)[1].split('Session* Drawing() noexcept', 1)[0]
        for marker in ('for (auto it = g_sessions.rbegin(); it != g_sessions.rend(); ++it)',
                       'if (it->hwnd != hwnd) continue;',
                       'return it->ready && GlassCore::BomeMemo::IsWindow(hwnd)',
                       '&& GlassShared::GlassIsBackdropAllowedForHwnd(hwnd) ? &*it : nullptr;'):
            self.assertIn(marker, served)
        drawing = code.split('Session* Drawing() noexcept', 1)[1].split('bool Open() noexcept', 1)[0]
        self.assertIn('if (GlassCore::PaintDispatcher::IsTopWindowDrawingSession(it->hwnd))', drawing)
        self.assertIn('return Served(it->hwnd);', drawing)
        self.assertIn('{ return !g_inside && !g_sessions.empty() && GlassCore::Root::IsFullInjectionMode(); }', code)
        active = code.split('HWND ActiveHost() noexcept', 1)[1].split('HWND TargetDc(', 1)[0]
        self.assertIn('const Session* drawing = Open() ? Drawing() : nullptr;', active)
        target = code.split('HWND TargetDc(HDC dc, LPCWSTR& reason) noexcept', 1)[1].split('void Count(', 1)[0]
        order = ('if (!Open()) return nullptr;',
                 'if (GetMapMode(dc) != MM_TEXT) { reason = L"map"; return nullptr; }',
                 'if (const HWND window = WindowFromDC(dc))',
                 'if (!Served(window)) { reason = L"window"; return nullptr; }',
                 'if (!GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(window, dc))',
                 '{ reason = L"window-dc"; return nullptr; }',
                 'return window;',
                 'if (GetObjectType(dc) != OBJ_MEMDC) { reason = L"dc"; return nullptr; }',
                 'const HWND owner = GlassCore::PaintDispatcher::GetStackOwnerForDc(dc);',
                 'if (Served(owner)) return owner;',
                 'if (const Session* drawing = Drawing()) return drawing->hwnd;',
                 'reason = L"buffer";')
        positions = [target.index(marker) for marker in order]
        self.assertEqual(positions, sorted(positions))
        # The dispatcher's ambient host is the context a buffer was created in, not its owner: it decides nothing
        # here, and the newest session of the thread is consulted only by the post-BeginPaint erase.
        self.assertNotIn('GetPaintSessionHostHwnd', code)
        self.assertEqual(code.count('g_sessions.back().ready'), 1)
        for api in ('int Fill(decltype(&::FillRect) original', 'int DrawAnsi(decltype(&::DrawTextA) original'):
            body = code.split(api, 1)[1]
            self.assertIn('const HWND host = TargetDc(dc, skipped);', body)
            self.assertIn('++g_sessions.back().skip;', body)
        for api in ('DWORD SystemColor(', 'HBRUSH CreateBrush('):
            self.assertIn('ActiveHost()', code.split(api, 1)[1].split('\n    }\n', 1)[0])
        # Counters and reasons are metadata: what a session drew and why a DC stayed native, never the text.
        for line in ('L"[GlassCore][BomeMemo] hooks-end hwnd=0x%p channel=%s ready=%d text=%u fill=%u skip=%u open=%zu",',
                     'L"[GlassCore][BomeMemo] text-skip dc=0x%p dcWnd=0x%p stackOwner=0x%p sessions=%zu reason=%s",',
                     'Count(host, &Session::text);', 'Count(host, &Session::fill);'):
            self.assertEqual(code.count(line), 1, line)
        post = code.split('bool AfterBeginPaint(', 1)[1].split('DWORD SystemColor(', 1)[0]
        for guard in ('g_sessions.back().ready', 'g_sessions.back().hwnd != hwnd',
                      'GlassCore::Root::IsFullInjectionMode()', 'GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)',
                      'caller != GlassCore::ModuleHandler::g_entryPointModule'):
            self.assertIn(guard, post)
        self.assertIn('g_sessions.back().paint', post)
        self.assertIn('Clear(dc, ps->rcPaint)', post)
        self.assertNotIn('ActiveHost()', post)  # post runs before the dispatcher push
        dispatcher = read('GlassCore/src/Subsystems/PaintDispatcher.cpp').split('static HDC WINAPI HookBeginPaint(', 1)[1].split('static BOOL WINAPI HookEndPaint(', 1)[0]
        self.assertLess(dispatcher.index('g_origBeginPaint(hWnd, lpPaint)', dispatcher.index('HMODULE callerMod')),
                        dispatcher.index('p.onAfterBeginPaint(hWnd, hdc, lpPaint, callerMod)'))
        self.assertLess(dispatcher.index('p.onAfterBeginPaint(hWnd, hdc, lpPaint, callerMod)'),
                        dispatcher.index('DcEntryType::Paint'))
        self.assertIn('if (backgroundCleared && lpPaint)\n                lpPaint->fErase = 0', dispatcher)
        self.assertIn('for (auto& p : g_paintProviders) p.onEnd(hWnd, callerMod)', dispatcher)

    def test_bome_memo_ansi_counts_operation_backgrounds_and_diagnostic_not_bpp_gate(self):
        code = read('GlassCore/src/Hooks/delphi/BomeMemoDraw.cpp')
        for required in ('GetTextCharset(dc)', 'TranslateCharsetInfo(', 'TCI_SRCCHARSET', 'CP_ACP',
                         'MultiByteToWideChar(cp, 0, text, count', 'if (count == -1) wide.pop_back()',
                         'static_cast<int>(wide.size())', 'DT_CALCRECT | DT_MODIFYSTRING | DT_NOCLIP',
                         'SaveDC(dc)', 'const auto fallback = [&](LPCWSTR reason) { restoreNow(); return nativeReason(reason); }',
                         'GetSysColor(COLOR_WINDOW)', 'GetSysColor(COLOR_BTNFACE)',
                         'copy.lbStyle = BS_SOLID', 'copy.lbColor = 0', 'copy.lbHatch = 0', 'original(&copy)',
                         'info.bmiHeader.biBitCount = 32', 'CreateDIBSection(', 'CreatePatternBrush(',
                         'FillRect(dc, &r, brush.get())',
                         'flags, result, true)', 'bitmap.bmBitsPixel', 'ansiCount=%d wideCount=%d cp=%u',
                         '[BomeMemo] hooks route=2', 'mask=0x%X ready=%d', 'text-skip'):
            self.assertIn(required, code)
        for forbidden in ('CreateSolidColorBrushWithAlpha', 'BLACK_BRUSH', 'StretchBltAttach',
                          'GdipCreateBitmapAlpha', 'SetWindowLong', 'SetAlpha255', 'ApplyBlackKeyAlpha'):
            self.assertNotIn(forbidden, code)
        self.assertNotRegex(code, r'bmBitsPixel\s*(?:==|!=|<|>)')
        self.assertNotIn('100, false, true', code)
        for project in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            self.assertIn(r'src\Hooks\delphi\BomeMemoDraw.cpp', read(project))
            self.assertIn(r'Include\Hooks\delphi\BomeMemoDraw.h', read(project))
        runner = read('WndCompat/tests/bome_memo_rendering_test.py')
        for production in ('Root_OnBeginPaint', 'Root_OnEndPaint', 'Root_OnGetDC', 'Root_OnReleaseDC',
                           'kBomeMemoSlots', 'Bome memo EXE thunks:'):
            self.assertIn(production, runner)
        test = read('WndCompat/tests/bome_memo_rendering_tests.cpp')
        cases = re.findall(r'\{"([a-z]+)",\w+\}', test)
        self.assertEqual(len(cases), 15)
        self.assertEqual(sorted(cases), sorted(re.findall(r"self\.run_case\('([a-z]+)'\)", runner)))

    def test_bome_memo_scope_exit_matches_wil_callback_constraints(self):
        code = read('GlassCore/src/Hooks/delphi/BomeMemoDraw.cpp')
        self.assertIn('wil::scope_exit([&]() noexcept { restoreNow(); })', code)
        self.assertNotIn('wil::scope_exit(restoreNow)', code)
        shim = read('WndCompat/tests/coolbar_test_shim.h')
        self.assertIn('auto scope_exit(F&& fn)', shim)
        self.assertIn('!std::is_reference_v<F>', shim)
        self.assertIn('std::is_same_v<decltype(fn()), void>', shim)

    def test_new_vcl_input_and_bitbtn_use_existing_providers(self):
        classifier = read("WndCompat/Tracker/WindowClassifier.cpp")
        for cls, kind in (("TBitBtn", "Button"), ("TLabeledEdit", "Edit"),
                          ("TButtonedEdit", "Edit"), ("TMaskEdit", "Edit"),
                          ("TSearchBox", "Edit")):
            with self.subTest(cls=cls):
                self.assertRegex(classifier, r'(?s)if\s*\([^;{}]*L"' + cls +
                                 r'"[^;{}]*\)\s*return ControlKind::' + kind + r'\s*;')
        event = read("WndCompat/Tracker/WinEventHandler.cpp")
        button = event.split('case ControlKind::Button:', 1)[1].split('case ControlKind::WinForms_Button:', 1)[0]
        self.assertIn('isFull && allowGlassForHwnd', button)
        self.assertIn('BS_BITMAP | BS_ICON', button)
        self.assertIn('Button_Attach(hWnd, GlassShared::GlassControlKind::Button)', button)
        edit_case = event.split('case ControlKind::Edit:', 1)[1].split('case ControlKind::RichEdit20W:', 1)[0]
        self.assertIn('isFull && allowGlassForHwnd', edit_case)
        self.assertIn('WndCompat::Edit_Attach(hWnd)', edit_case)
        # Только диагностика существующих сессий, без нового IAT/общего зонда.
        dispatcher = read('GlassCore/src/Subsystems/PaintDispatcher.cpp')
        for cls in ("TLabeledEdit", "TButtonedEdit", "TMaskEdit", "TSearchBox"):
            self.assertIn('L"' + cls + '"', dispatcher)
        edit = read('GlassCore/src/Hooks/EditHooks.cpp')
        self.assertIn('[GlassCore][DelphiInput] ExtTextOut', edit)
        self.assertIn('[GlassCore][DelphiInput] TextResult', edit)
        self.assertIn('static thread_local unsigned budget[2]', edit)
        self.assertIn('[GlassCore][DelphiBitBtn] BeginPaint', read('GlassCore/src/Hooks/delphi/TBitBtnHooks.cpp'))
        self.assertIn('LogDelphiBitBtnBeginPaint(hWnd', read('GlassCore/src/Hooks/ButtonHooks.cpp'))

    def test_six_vcl_controls_use_requested_paths(self):
        classifier = read('WndCompat/Tracker/WindowClassifier.cpp')
        for cls, kind in (("TNumberBox", "Edit"), ("THotKey", "Edit"),
                          ("TRichEdit", "RichEdit50W"), ("TListBox", "ListBox"),
                          ("TCheckListBox", "ListBox"), ("TColorListBox", "ListBox")):
            with self.subTest(cls=cls):
                self.assertRegex(classifier, r'(?s)if\s*\([^;{}]*L"' + cls +
                                 r'"[^;{}]*\)\s*return ControlKind::' + kind + r'\s*;')
        edit = read('WndCompat/Controls/EditSubclass.cpp')
        detach = edit.split('void RichEdit_Detach(HWND hWnd)', 1)[1]
        self.assertNotIn('EM_SETBKGNDCOLOR', detach)
        for cls in ('TNumberBox', 'THotKey', 'TRichEdit'):
            self.assertIn('L"' + cls + '"', read('GlassCore/src/Hooks/EditHooks.cpp'))

    def test_vcl_input_icons_use_exact_controls_and_proven_destination_dc(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        for guard in ('IsFullInjectionMode()', 'GetMapMode(dc) != MM_TEXT',
                      'WindowFromDC(dc)', 'GetStackOwnerForDc(dc)',
                      'IsTopWindowDrawingSession(window)', 'bitmap.bmBitsPixel <= 1',
                      'L"TButtonedEdit"', 'L"TSearchBox"', 'L"TEditButton.TGlyph"', 'L"TGlyph"', 'GetParent(window)',
                      'GlassControlKind::Edit', 'GlassIsBackdropAllowedForHwnd(owner)',
                      'GlassIsBackdropAllowedForHwnd(window)', 'g_inside', 'g_shutdown'):
            self.assertIn(guard, code)
        glyph = code.split('if (HasClass(window, L"TEditButton.TGlyph") || HasClass(window, L"TGlyph"))', 1)[1].split('else if', 1)[0]
        self.assertIn('owner = GetParent(window)', glyph)
        self.assertIn('if (!HasClass(owner, L"TButtonedEdit")) return nullptr', glyph)
        self.assertIn('_wcsicmp(cls, name) == 0', code)
        for forbidden in ('wcsstr', 'wcsncmp', 'GetAncestor', 'GetPaintSessionHostHwnd', 'ApplyBlackKeyAlpha',
                          'ColorTreatAsTransparent', 'ExtTextOut', 'reinterpret_cast<PVOID>(BitBlt)',
                          'BufferedPaintSetAlpha'):
            self.assertNotIn(forbidden, code)
        self.assertNotRegex(code, r'\.rgb(?:Red|Green|Blue)\s*=(?!=)')
        self.assertIn('g_entryPointModule', code)
        self.assertNotIn('RegisterModulePrewarmHandler', code)  # no IAT writes under loader lock

    def test_vcl_input_icons_keep_native_imagelist_parameters_and_alpha(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        for required in ('"ImageList_Draw"', '"ImageList_DrawEx"', '"ImageList_DrawIndirect"',
                         'ILD_MASK | ILD_ROP | ILD_DPISCALE', 'ILD_TRANSPARENT | ILD_PRESERVEALPHA',
                         'BPBF_TOPDOWNDIB', 'AC_SRC_OVER, 0, 255, AC_SRC_ALPHA',
                         'BufferedPaintClear(buffer, nullptr)', 'EndBufferedPaint(buffer, drawn)',
                         'GetBufferedPaintBits(buffer, &pixels, &stride)', 'if (!hasAlpha)',
                         'if (buffer) EndBufferedPaint(buffer, FALSE)', 'auto work = *params',
                         'work.hdcDst = memory; work.fStyle = flags', 'params->cbSize != sizeof(*params)',
                         'GetProcAddress(module, "ImageList_GetIconSize")', 'g_dispatcher.DisableAllHooks()'):
            self.assertIn(required, code)
        engine = read('GlassCore/src/Core/Engine.cpp')
        self.assertIn('GlassCore::DelphiInputIcons::Startup()', engine)
        self.assertIn('GlassCore::DelphiInputIcons::Shutdown()', engine)
        for path in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            for file in ('TInputIcons.cpp', 'TInputIcons.h'):
                self.assertIn(file, read(path))
        runner = read('WndCompat/tests/input_icons_test.py')
        self.assertIn("for debug in (False, True)", runner)
        self.assertIn('GlassCore/src/Hooks/delphi/TInputIcons.cpp', runner)

    def test_vcl_input_rgb_only_mask_branch_is_bounded_and_excludes_searchbox_and_existing_alpha(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        draw = code.split('template<typename Paint> std::optional<BOOL> TryDraw(', 1)[1].split('BOOL WINAPI Draw(HIMAGELIST images', 1)[0]
        for marker in ('width > 1024 || height > 1024', 'ILD_MASK | ILD_ROP | ILD_DPISCALE',
                       'if (!flushed) return std::nullopt', 'row < height && !hasAlpha', 'col < width',
                       'pixels[static_cast<size_t>(row) * stride + col].rgbReserved',
                       'if (!hasAlpha)', '!HasClass(owner, L"TButtonedEdit")',
                       '!RestoreLegacyMaskAlpha(dc, bounds, pixels, stride, width, height, style, paint)',
                       'legacyMask = true', 'legacyMask=%d'):
            self.assertIn(marker, draw)
        self.assertLess(draw.index('if (!hasAlpha)'), draw.index('!HasClass(owner, L"TButtonedEdit")'))
        self.assertLess(draw.index('!HasClass(owner, L"TButtonedEdit")'), draw.index('!RestoreLegacyMaskAlpha('))
        self.assertLess(draw.index('!RestoreLegacyMaskAlpha('), draw.index('EndBufferedPaint(buffer, drawn)'))
        self.assertEqual(code.count('!RestoreLegacyMaskAlpha('), 1)
        self.assertNotRegex(draw, r'\.rgb(?:Reserved|Red|Green|Blue)\s*=(?!=)')
        # Actual caller lambdas, not a second hand-built ImageList/bitmap path.
        for marker in ('return orig(images, index, memory, x, y, flags)',
                       'return orig(images, index, memory, x, y, width, height, background, foreground, flags)',
                       'auto work = *params', 'work.hdcDst = memory; work.fStyle = flags', 'return orig(&work)'):
            self.assertIn(marker, code)

    def test_vcl_input_mask_coverage_comes_from_the_same_native_draw_on_two_mattes(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        helper = code.split('template<typename Paint> bool RestoreLegacyMaskAlpha(', 1)[1].split('template<typename Paint> std::optional<BOOL> TryDraw(', 1)[0]
        for marker in ('BeginBufferedPaint(dc, &bounds, BPBF_TOPDOWNDIB, &params, &memory)',
                       'if (!matte || !memory)', 'EndBufferedPaint(matte, FALSE)',
                       'wil::scope_exit([matte]', 'BufferedPaintClear(matte, nullptr)',
                       'GetBufferedPaintBits(matte, &white, &whiteStride)',
                       '!white || whiteStride < width || !GdiFlush()',
                       'white[static_cast<size_t>(row) * whiteStride + col] = RGBQUAD{255, 255, 255, 0}',
                       '!paint(memory, style | ILD_TRANSPARENT | ILD_PRESERVEALPHA) || !GdiFlush()',
                       'RGBQUAD& dark = black[static_cast<size_t>(row) * blackStride + col]',
                       'const RGBQUAD& light = white[static_cast<size_t>(row) * whiteStride + col]',
                       'const int delta = light.rgbBlue - dark.rgbBlue', 'delta < 0',
                       'light.rgbGreen - dark.rgbGreen != delta', 'light.rgbRed - dark.rgbRed != delta',
                       'const int alpha = 255 - delta', 'dark.rgbBlue > alpha',
                       'dark.rgbReserved = static_cast<BYTE>(alpha)'):
            self.assertIn(marker, helper)
        self.assertEqual(helper.count('!GdiFlush()'), 2)
        self.assertLess(helper.index('!GdiFlush()'), helper.index('RGBQUAD{255, 255, 255, 0}'))
        self.assertLess(helper.index('!paint('), helper.index('const int delta'))
        self.assertLess(helper.index('const int delta'), helper.index('dark.rgbReserved ='))
        body = re.sub(r'//[^\n]*|/\*.*?\*/', '', helper, flags=re.S)
        for forbidden in ('EndBufferedPaint(matte, TRUE)', 'BufferedPaintSetAlpha', 'BitBlt(', 'MaskBlt(',
                          'ImageList_GetImageInfo', 'GetDIBits', 'ColorTreatAsTransparent',
                          'ILD_MASK', 'CreateSolidBrush', 'SelectClipRgn', 'IntersectClipRect'):
            self.assertNotIn(forbidden, body)
        self.assertNotRegex(helper, r'dark\.rgb(?:Blue|Green|Red)\s*=(?!=)')
        self.assertNotRegex(helper, r'\.rgb(?:Blue|Green|Red)\s*==')  # no RGB colour-key test

    def test_vcl_input_rgb_mask_behavioral_fixture_uses_independent_native_masks_and_failure_cases(self):
        runner = read('WndCompat/tests/input_icons_test.py')
        for marker in ('struct MaskIcon', 'std::vector<bool> solid', 'void NativeMask(',
                       '!icon.solid.at(source)', 'dst.rgbReserved=0',
                       'Mock::whitePadding=4', 'Mock::poisonPadding=true',
                       'test_rgb_only_native_mask_retains_opaque_black_white_colors_and_transparent_holes',
                       'test_rgb_only_mask_branch_preserves_index_overlay_crop_state_colors_and_caller_params',
                       'test_rgb_only_branch_leaves_searchbox_alpha_icons_unsupported_styles_and_reentry_alone',
                       'test_rgb_only_secondary_buffer_failures_discard_and_repeat_the_original_native_call'):
            self.assertIn(marker, runner)

    def test_vcl_input_button_background_uses_proven_direct_child_bounds_and_client_dc(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        plan = code.split('BackgroundPatches PlanButtonBackground(', 1)[1].split('BackgroundPatches PlanSearchBackground(', 1)[0]
        child = code.split('bool ButtonRect(', 1)[1].split('BackgroundPatches PlanButtonBackground(', 1)[0]
        for marker in ('HasClass(owner, L"TButtonedEdit")', 'WindowFromDC(dc) != target',
                       'GetGraphicsMode(dc) != GM_COMPATIBLE', 'GetLayout(dc) != 0',
                       'ReadSolidBrush(brush, color)', 'GetSysColor(COLOR_WINDOW)',
                       'GetClientRect(target, &client)', '!EqualRect(rect, &client)',
                       'ClientToScreen(target, &origin)', 'GetDCOrgEx(dc, &dcOrigin)',
                       'origin.x != dcOrigin.x', 'origin.y != dcOrigin.y',
                       'GetViewportOrgEx(dc, &viewport)', 'GetWindowOrgEx(dc, &windowOrigin)',
                       'viewport.x || viewport.y || windowOrigin.x || windowOrigin.y',
                       'GetClientRect(owner, &parentClient)', 'ClientToScreen(owner, &parentOrigin)',
                       'patches.rects[0] = client', 'GetWindow(owner, GW_CHILD)',
                       'GetWindow(child, GW_HWNDNEXT)', '++visited > 64',
                       'HasClass(child, L"TEditButton.TGlyph")', 'HasClass(child, L"TGlyph")',
                       'patches.count == patches.rects.size()', 'patches.rects[patches.count++] = button'):
            self.assertIn(marker, plan)
        for marker in ('GetParent(child) != owner', 'GlassIsBackdropAllowedForHwnd(child)',
                       'GetWindowRect(child, &screen)', 'GetClientRect(child, &client)',
                       'ClientToScreen(child, &origin)', 'origin.x != screen.left',
                       'client.right > 1024 || client.bottom > 1024',
                       'static_cast<long long>(screen.left) - parentOrigin.x',
                       'left < parentClient.left', 'bottom > parentClient.bottom',
                       '!EqualRect(&result, &parentClient)'):
            self.assertIn(marker, child)
        # Geometry comes from a current immediate child, including hidden ones.
        # No guessed edge width, descendants, cached RECT or post-BeginPaint gate.
        geometry = re.sub(r'//[^\n]*|/\*.*?\*/', '', plan + child, flags=re.S)
        for forbidden in ('IsWindowVisible', 'EnumChildWindows', 'GetAncestor',
                          'GetPaintSession', 'IsTopWindowDrawingSession', 'AfterPaint',
                          'MapWindowPoints', 'SelectClipRgn', 'IntersectClipRect'):
            self.assertNotIn(forbidden, geometry)
        brush = code.split('bool ReadSolidBrush(', 1)[1].split('bool ButtonRect(', 1)[0]
        for marker in ('COLOR_MENUBAR + 1u', 'GetSysColor(', 'GetObjectW(brush, sizeof(lb), &lb)',
                       'lb.lbStyle != BS_SOLID'):
            self.assertIn(marker, brush)

    def test_vcl_searchbox_background_proves_its_own_nc_dc_and_reserved_strip(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        plan = code.split('BackgroundPatches PlanSearchBackground(', 1)[1].split('int WINAPI Fill(HDC,', 1)[0]
        for marker in ('owner != target', '!HasClass(owner, L"TSearchBox")', 'WindowFromDC(dc) != owner',
                       'GetGraphicsMode(dc) != GM_COMPATIBLE', 'GetLayout(dc) != 0',
                       'ReadSolidBrush(brush, color)', 'GetSysColor(COLOR_WINDOW)',
                       'GetWindowRect(owner, &window)', 'GetClientRect(owner, &client)',
                       'client.left || client.top || IsRectEmpty(&client)',
                       'ClientToScreen(owner, &clientOrigin)', 'GetDCOrgEx(dc, &dcOrigin)',
                       'dcOrigin.x != window.left', 'dcOrigin.y != window.top',
                       'GetViewportOrgEx(dc, &viewport)', 'GetWindowOrgEx(dc, &windowOrigin)',
                       'viewport.x || viewport.y || windowOrigin.x || windowOrigin.y',
                       'static_cast<long long>(window.right) - window.left',
                       'static_cast<long long>(clientOrigin.x) - window.left',
                       'const long long right = left + client.right',
                       'std::numeric_limits<LONG>::max', 'right > width || bottom > height',
                       'const long long rightInset = width - right',
                       'const long long border = left < rightInset ? left : rightInset',
                       'border <= 0 || top != border || height - bottom != border || left == rightInset',
                       'left > rightInset ? border : right', 'left > rightInset ? left : width - border',
                       'buttonRight - buttonLeft > 1024 || bottom - top > 1024',
                       '!EqualRect(rect, &button)', 'patches.rects[0] = button', 'patches.count = 1'):
            self.assertIn(marker, plan)
        body = re.sub(r'//[^\n]*|/\*.*?\*/', '', plan, flags=re.S)
        # Not a fixed edge width, inferred image/clip bounds, descendant or DC host.
        for forbidden in ('GetSystemMetrics', 'GetClipBox', 'GetParent', 'GetWindow(',
                          'GetStackOwnerForDc', 'GetPaintSession', 'ImageList_',
                          'BeginBufferedPaint', 'PatBlt(', 'SelectClipRgn', 'IntersectClipRect',
                          'BufferedPaintSetAlpha', 'GetWindowLongPtr'):
            self.assertNotIn(forbidden, body)
        self.assertNotRegex(body, r'(?<![\w])(?:2|24)(?![\w])')

    def test_vcl_searchbox_background_is_only_an_additional_fill_plan_not_an_image_fix(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        fill = code.split('int WINAPI Fill(HDC dc, const RECT* rect, HBRUSH brush)', 1)[1].split('void ProbePixels(', 1)[0]
        self.assertIn('if (!patches.count) patches = PlanSearchBackground(owner, target, dc, rect, brush)', fill)
        self.assertLess(fill.index('PlanButtonBackground('), fill.index('PlanSearchBackground('))
        self.assertLess(fill.index('PlanSearchBackground('), fill.index('const int result = orig(dc, rect, brush)'))
        self.assertEqual(code.count('PlanSearchBackground(owner, target, dc, rect, brush)'), 1)
        images = code.split('template<typename Paint> std::optional<BOOL> TryDraw(', 1)[1]
        self.assertNotIn('PlanSearchBackground', images)
        self.assertNotIn('PatBlt(', images)
        self.assertIn('if (!hasAlpha)', images)
        self.assertIn('if (!HasClass(owner, L"TButtonedEdit")', images)  # SearchBox RGB-only still native
        for forbidden in ('RGBQUAD', 'ExtTextOut', 'DoubleBuffered', 'BeginBufferedPaint'):
            self.assertNotIn(forbidden, fill)

    def test_vcl_searchbox_behavioral_suite_separates_nc_background_from_existing_images(self):
        runner = read('WndCompat/tests/input_icons_test.py')
        for marker in ('RECT SearchGeometry(', 'void CompositeBuffer()', 'constexpr int x=108,y=3',
                       'for(int state=0;state<4;++state)', 'Images audio=f.images',
                       'r.left==104&&r.top==2&&r.right==128&&r.bottom==21',
                       'test_searchbox_background_uses_current_left_or_right_nc_reservation_not_fixed_width',
                       'test_searchbox_nc_background_keeps_partial_complex_empty_and_foreign_clips',
                       'test_searchbox_hot_pressed_and_custom_background_colors_stay_native',
                       'test_searchbox_nc_fill_and_existing_text_audio_image_alpha_remain_separate',
                       'test_searchbox_background_rejects_client_memory_transformed_foreign_and_unproven_geometry',
                       'test_searchbox_background_failures_reentry_missing_import_shutdown_and_effective_original'):
            self.assertIn(marker, runner)

    def test_vcl_input_button_fill_preserves_effective_original_result_error_and_clip(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        fill = code.split('int WINAPI Fill(HDC dc, const RECT* rect, HBRUSH brush)', 1)[1].split('void ProbePixels(', 1)[0]
        for marker in ('g_fillDispatcher.GetOrg<0, decltype(&::FillRect)>()',
                       'IconOwner(dc, &target)', 'g_inFillHook', 'TakeProbe(true)',
                       'PlanButtonBackground(owner, target, dc, rect, brush)',
                       'const RECT r = rect ? *rect : RECT{}', 'GetClipBox(dc, &clip)',
                       'GetClientRect(target, &client)', 'GetWindowRect(target, &window)',
                       'GetDCOrgEx(dc, &dcOrg)', 'ClientToScreen(target, &clientOrg)',
                       'GetViewportOrgEx(dc, &viewport)', 'GetWindowOrgEx(dc, &windowOrg)',
                       'const int result = orig(dc, rect, brush)', 'SetLastError(entryError)',
                       'const DWORD error = GetLastError()', 'SetLastError(error)',
                       'if (result)', 'i < patches.count',
                       'PatBlt(dc, p.left, p.top, p.right - p.left, p.bottom - p.top, BLACKNESS)',
                       'brushValid && lb.lbStyle == BS_SOLID', 'stage=fill',
                       'brushRGBValid=', 'brushRGB=', 'clipKind=', 'result=', 'patched=%u'):
            self.assertIn(marker, fill)
        for forbidden in ('BitBlt', 'AlphaBlend', 'BeginBufferedPaint',
                          'CreateSolidBrush', 'DeleteObject', 'SelectObject',
                          'SelectClipRgn', 'IntersectClipRect', 'BufferedPaintSetAlpha',
                          'BeginPaint(', 'RegisterPaintProvider'):
            self.assertNotIn(forbidden, fill)
        self.assertLess(fill.index('GetClipBox(dc, &clip)'), fill.index('const int result = orig'))
        self.assertLess(fill.index('const int result = orig'), fill.index('if (PatBlt'))
        self.assertLess(fill.index('if (PatBlt'), fill.index('stage=fill'))

    def test_vcl_input_icon_pixel_probe_is_read_only_before_commit_and_fallback(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        probe = code.split('void ProbePixels(', 1)[1].split('#endif', 1)[0]
        for marker in ('TakeProbe(false)',
                       'drawn && SUCCEEDED(bitsResult) && pixels && stride >= width && flushed',
                       'if (available)', 'row < height', 'col < width',
                       'const RGBQUAD& p = pixels[static_cast<size_t>(row) * stride + col]',
                       'alpha0=', 'alpha255=', 'alphaPartial=', 'whiteNonzeroAlpha=', 'nonPremul=',
                       'available=', 'bitsHr=', 'flushed=', 'stage=pixels',
                       'api=%s', 'hwnd=0x%p', 'target=0x%p', 'index=%d', 'stride=%d',
                       'GetClassNameW(target, cls, _countof(cls))', 'SetLastError(error)'):
            self.assertIn(marker, probe)
        for forbidden in ('GetBufferedPaintBits(', 'GdiFlush()', 'BufferedPaintSetAlpha', 'EndBufferedPaint'):
            self.assertNotIn(forbidden, probe)
        self.assertNotRegex(probe, r'\.rgb(?:Reserved|Red|Green|Blue)\s*=(?!=)')
        draw = code.split('template<typename Paint> std::optional<BOOL> TryDraw(', 1)[1].split('BOOL WINAPI Draw(HIMAGELIST images', 1)[0]
        for after in ('if (!flushed) return std::nullopt', 'if (!hasAlpha)',
                      'const HRESULT committed = EndBufferedPaint(buffer, drawn)'):
            self.assertLess(draw.index('ProbePixels(api,'), draw.index(after))
        for api in ('ImageList_Draw', 'ImageList_DrawEx', 'ImageList_DrawIndirect'):
            self.assertIn('L"' + api + '", ' + ('params->i' if api.endswith('Indirect') else 'index'), code)

    def test_vcl_input_button_fill_is_production_but_probe_lifecycle_stays_opt_in(self):
        code = read('GlassCore/src/Hooks/delphi/TInputIcons.cpp')
        guard = '#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)'
        startup = code.split('void Startup()', 1)[1].split('void Shutdown()', 1)[0]
        shutdown = code.split('void Shutdown()', 1)[1]
        self.assertIn('HookDispatcher<1, 3> g_fillDispatcher', code.split(guard, 1)[0])
        self.assertIn('"FillRect", reinterpret_cast<void*>(Fill)', code)
        self.assertIn('g_fillDispatcher.EnableHook(0, true)', startup.split(guard, 1)[0])
        self.assertIn('g_fillDispatcher.DisableAllHooks()', shutdown)
        self.assertNotIn(guard, shutdown)
        self.assertIn('if (!g_shutdown.load(std::memory_order_acquire)) return', startup)
        self.assertIn('g_entryPointModule', startup)
        for marker in ('stage=arm', 'g_fillDispatcher.GetRef(0)', 'imageRefs=[%d,%d,%d]'):
            self.assertIn(marker, startup)
        self.assertIn('static thread_local Budget budgets[2]', code)
        self.assertIn('budgets[fill ? 0 : 1]', code)
        self.assertIn('fill ? 24u : 12u', code)
        self.assertIn('now - b.epoch >= 1000', code)
        engine = read('GlassCore/src/Core/Engine.cpp')
        for later in ('GlassCore::Root::Startup()', 'GlassCore::PaintDispatcher::Startup()'):
            self.assertLess(engine.index('GlassCore::DelphiInputIcons::Startup()'), engine.index(later))
        runner = read('WndCompat/tests/input_icons_test.py')
        for marker in ("for probe in ((False, True) if debug else (False,))", "'-DGLASS_DELPHI_DRAW_PROBE'",
                       'test_qualified_class_all_indices_legacy_alias_and_no_substring_or_ancestor_match',
                       'test_separate_white_fill_and_mixed_black_white_partial_alpha_pixels_are_unchanged',
                       'test_fill_probe_records_original_arguments_geometry_results_and_error_without_drawing',
                       'test_pixel_probe_counts_full_buffer_without_padding_and_distinguishes_unavailable_bits',
                       'test_probe_budgets_are_separate_bounded_and_thread_local',
                       'test_fill_probe_calls_and_restores_supplied_effective_original_even_for_foreign_windows',
                       'test_button_background_blackens_only_the_full_direct_glyph_client',
                       'test_parent_full_and_partial_fill_restore_both_visible_and_hidden_button_regions',
                       'test_button_background_keeps_partial_complex_clip_and_untouched_pixels',
                       'test_button_background_rejects_foreign_colored_unproven_or_transformed_calls',
                       'test_button_background_native_failures_shutdown_missing_slot_and_reentry'):
            self.assertIn(marker, runner)

    def test_runtime_core_lookups_have_unique_undecorated_named_exports(self):
        # Check actual callers, not just declarations: WINAPI dllexport alone
        # leaves _Name@N on x86. NONAME and duplicate entries are not a fix.
        requested = {}
        for path in sorted((ROOT / 'WndCompat').rglob('*')):
            if path.suffix not in ('.cpp', '.h') or 'tests' in path.relative_to(ROOT).parts:
                continue
            source = read(path.relative_to(ROOT))
            for name in re.findall(r'\bGetProcAddress\s*\([^;]*?"(GlassCore_\w+)"', source):
                requested.setdefault(name, []).append(str(path.relative_to(ROOT)))
        required = {'GlassCore_' + phase + 'Delphi' + bridge
                    for phase in ('Begin', 'End')
                    for bridge in ('BitBtnDraw', 'ListBoxDraw', 'ListBoxProbe')}
        self.assertTrue(required <= requested.keys(), required - requested.keys())
        export_sections = re.split(r'(?m)^[ \t]*EXPORTS[ \t]*$', read('GlassCore/GlassCore.def'))
        self.assertEqual(len(export_sections), 2)
        entries = {}
        for line in export_sections[1].splitlines():
            declaration = line.split(';', 1)[0].strip()
            match = re.fullmatch(r'(GlassCore_\w+)([ \t=].*)?', declaration)
            if match:
                entries.setdefault(match[1], []).append((match[2] or '').split())
        for name, callers in sorted(requested.items()):
            with self.subTest(export=name, callers=callers):
                matches = entries.get(name, [])
                self.assertEqual(len(matches), 1, 'Expected one named .def export for ' + name)
                self.assertNotIn('NONAME', matches[0])

    def test_delphi_owner_draw_export_signatures_match_runtime_callers(self):
        exports = read('GlassCore/src/Core/Exports.cpp')
        for bridge in ('BitBtnDraw', 'ListBoxDraw', 'ListBoxProbe'):
            with self.subTest(bridge=bridge):
                prefix = r'extern\s+"C"\s+__declspec\(dllexport\)\s+'
                self.assertRegex(exports, prefix + r'BOOL\s+WINAPI\s+GlassCore_BeginDelphi' + bridge +
                                 r'\s*\(\s*HWND\s+\w+\s*,\s*const\s+DRAWITEMSTRUCT\s*\*\s*\w+\s*\)\s*noexcept\s*\{')
                self.assertRegex(exports, prefix + r'void\s+WINAPI\s+GlassCore_EndDelphi' + bridge +
                                 r'\s*\(\s*\)\s*noexcept\s*\{')

    def test_core_export_definition_is_used_in_all_four_build_configurations(self):
        project = ET.fromstring(read('GlassCore/GlassCore.vcxproj'))
        ns = {'msb': 'http://schemas.microsoft.com/developer/msbuild/2003'}
        configurations = {'Debug|Win32', 'Release|Win32', 'Debug|x64', 'Release|x64'}
        declared = {node.get('Include')
                    for node in project.findall('msb:ItemGroup/msb:ProjectConfiguration', ns)}
        self.assertEqual(declared, configurations)
        for config in sorted(configurations):
            with self.subTest(configuration=config):
                condition = "'$(Configuration)|$(Platform)'=='" + config + "'"
                groups = [node for node in project.findall('msb:ItemDefinitionGroup', ns)
                          if node.get('Condition') == condition]
                self.assertEqual(len(groups), 1)
                links = groups[0].findall('msb:Link', ns)
                self.assertEqual(len(links), 1)
                self.assertFalse(links[0].get('Condition'))
                definitions = links[0].findall('msb:ModuleDefinitionFile', ns)
                self.assertEqual(len(definitions), 1)
                self.assertFalse(definitions[0].get('Condition'))
                self.assertEqual((definitions[0].text or '').strip(), 'GlassCore.def')

    def test_vcl_list_owner_draw_probe_is_passthrough_and_scoped(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxProbe.cpp')
        root = read('WndCompat/Root/RootSubclass.cpp')
        for guard in ('#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)',
                      'ODT_LISTBOX', 'GetParent(dis->hwndItem) != parent',
                      'GlassIsBackdropAllowedForHwnd', 'dis->hDC',
                      'GlassControlKind::ListBox', 'g_entryPointModule'):
            self.assertIn(guard, code)
        for cls in ('TListBox', 'TCheckListBox', 'TColorListBox'):
            self.assertIn('L"' + cls + '"', code)
            self.assertIn('L"' + cls + '"', read('GlassCore/src/Hooks/RootGdiHooks.cpp'))
        self.assertIn('BeginVclDrawProbe(dis->hwndItem, true)', code)
        self.assertIn('EndVclDrawProbe(hwnd, true)', code)
        self.assertIn('[GlassCore][DelphiListBox] WM_DRAWITEM', code)
        self.assertNotIn('PatBlt(', code)
        self.assertNotIn('FillRect(', code)
        owner = root.split('GlassCore_BeginDelphiListBoxProbe', 1)[1].split('// Нас интересуют только пункты меню', 1)[0]
        self.assertIn('wil::scope_exit', owner)
        self.assertIn('return DefSubclassProc(hWnd, uMsg, wParam, lParam)', owner)
        for file in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            self.assertIn('TListBoxProbe.cpp', read(file))
            self.assertIn('TListBoxProbe.h', read(file))

    def test_vcl_list_draw_uses_exact_owner_dc_and_keeps_controls_intact(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        root = read('WndCompat/Root/RootSubclass.cpp')
        for guard in ('ODT_LISTBOX', 'GetParent(dis->hwndItem) != parent',
                      'GlassIsBackdropAllowedForHwnd', 'dis->hDC',
                      'GlassControlKind::ListBox', 'dc == s.dc',
                      'GetTextAlign(dc) & TA_UPDATECP', 'DT_CALCRECT | DT_MODIFYSTRING',
                      'IntersectClipRect(dc, s.bounds.left', 's.bounds = s.item',
                      'EqualRect(&clipped, r)',
                      'IsColorSample(s, *r)', 's.checkList',
                      'CreateSolidColorBrushWithAlpha', 'g_inHook',
                      'if (s.hooks[i]) g_dispatcher.EnableHook(i, false)'):
            self.assertIn(guard, code)
        self.assertIn('ExtTextOutWithAlpha', code)
        self.assertIn('DrawTextWithAlpha', code)
        self.assertIn('BLACKNESS', code)
        self.assertIn('GlassCore_BeginDelphiListBoxDraw', root)
        self.assertIn('GlassCore_EndDelphiListBoxDraw', root)
        self.assertIn('finishDraw = wil::scope_exit', root)
        self.assertIn('GlassCore::DelphiListBox::Shutdown()', read('GlassCore/src/Hooks/ListBoxHooks.cpp'))
        for file in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            self.assertIn('TListBoxDraw.cpp', read(file))
            self.assertIn('TListBoxDraw.h', read(file))

    def test_tlistbox_a100_is_exact_background_only_not_selection_or_other_lists(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        self.assertIn('s.plainList = list && _wcsicmp(cls, L"TListBox") == 0', code)
        self.assertIn('s.selected = (dis->itemState & ODS_SELECTED) != 0', code)
        alpha = code.split('BYTE BackgroundAlpha(', 1)[1].split('void RememberRowBackground(', 1)[0]
        for marker in ('s.plainList && !s.selected', 'GetSysColor(COLOR_HIGHLIGHT)', '? 100 : 255'):
            self.assertIn(marker, alpha)
        fill = code.split('int WINAPI MyFill(HDC dc', 1)[1].split('BOOL WINAPI MyExtText(HDC dc', 1)[0]
        self.assertIn('EqualRect(r, &s.item) ? BackgroundAlpha(s, lb.lbColor) : 255', fill)
        self.assertIn('CreateSolidColorBrushWithAlpha(lb.lbColor, opacity)', fill)
        self.assertIn('!sample && SystemBackground(lb.lbColor)', fill)
        combo = fill.split('if (s.combo)', 1)[1].split('const bool sample', 1)[0]
        self.assertNotIn('BackgroundAlpha', combo)
        self.assertIn('CreateSolidColorBrushWithAlpha(lb.lbColor, 255)', combo)
        ext = code.split('BOOL WINAPI MyExtText(HDC dc', 1)[1].split('int WINAPI MyDrawText(', 1)[0]
        self.assertIn('CreateSolidColorBrushWithAlpha(bk, BackgroundAlpha(s, bk))', ext)
        self.assertIn('drawOptions &= ~static_cast<UINT>(ETO_OPAQUE)', ext)

    def test_tlistbox_black_evidence_is_local_to_one_successful_full_row_fill(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        self.assertIn('COLORREF rowBackground{CLR_INVALID}', code)
        remember = code.split('void RememberRowBackground(', 1)[1].split('bool IsColorSample(', 1)[0]
        for marker in ('result && s.plainList', 'EqualRect(&r, &s.item)',
                       '!g_stack.empty()', 'g_stack.back().serial == s.serial',
                       'g_stack.back().rowBackground = color & 0xFFFFFFu'):
            self.assertIn(marker, remember)
        self.assertEqual(code.count('RememberRowBackground(s, *r, lb.lbColor, result)'), 2)
        self.assertIn('const bool confirmedBlack = s.plainList && s.rowBackground == 0', code)
        self.assertIn('((bk & 0xFFFFFFu) == 0 && !confirmedBlack) || SystemBackground(bk)', code)
        self.assertIn('if ((bk & 0xFFFFFFu) != 0) RememberRowBackground(s, *rect, bk, 1)', code)
        for forbidden in ('SetProp', 'GetProp', 'SetAlpha', 'RegisterCompatDCProvider', 'RegisterGetDCProvider'):
            self.assertNotIn(forbidden, code)

    def test_tlistbox_drawtext_keeps_the_painted_background_using_existing_glyph_only_mode(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        draw = code.split('int WINAPI MyDrawText(HDC dc', 1)[1].split('void Startup()', 1)[0]
        self.assertIn('dc, text, count, rect, format, result, s.plainList)', draw)
        for guard in ('DT_CALCRECT | DT_MODIFYSTRING', 'const Session s = *current',
                      'SaveDC(dc)', 'RestoreDC(dc, saved)', 'IntersectClipRect(dc, s.bounds.left',
                      'return orig(dc, text, count, rect, format)'):
            self.assertIn(guard, draw)
        self.assertNotIn('BackgroundAlpha', draw)
        self.assertIn('bool transparentBufferBackground = false', read('GlassCore/Include/Subsystems/RenderText.h'))
        shim = read('WndCompat/tests/listbox_background_test_shim.h')
        self.assertIn('int& result, bool transparent=false)', shim)
        self.assertIn('#define DrawTextWithAlpha ListDrawTextWithAlpha', shim)

    def test_tlistbox_background_suite_compiles_complete_production_in_both_modes(self):
        runner = read('WndCompat/tests/listbox_background_rendering_test.py')
        test = read('WndCompat/tests/listbox_background_rendering_tests.cpp')
        self.assertIn('#include "../../GlassCore/src/Hooks/delphi/TListBoxDraw.cpp"', test)
        self.assertIn('BeginDraw(&parent,&dis)', test)
        self.assertIn('module.hooks.at(name)', test)
        self.assertIn('#include "vcl_images_test_shim.h"', read('WndCompat/tests/listbox_background_test_shim.h'))
        for marker in ("'Release'", "'Debug'", "'-D_DEBUG'", "'-DGLASS_DELPHI_DRAW_PROBE'", "'-Werror'"):
            self.assertIn(marker, runner)
        for forbidden in ('read_text', 'source.split(', 'code.split(', '-Wno-error'):
            self.assertNotIn(forbidden, runner)

    def test_delphi_input_fixes_are_exact_and_balanced(self):
        edit = read('GlassCore/src/Hooks/EditHooks.cpp')
        code = read('GlassCore/src/Hooks/delphi/TInputHooks.cpp')
        root = read('WndCompat/Root/RootSubclass.cpp')
        for guard in ('HasDelphiInputDefaultBackground(hwnd)', 'GetFocus() == hwnd',
                      'ES_NOHIDESEL', 'ETO_GLYPH_INDEX | ETO_CLIPPED',
                      'IsCurrentPaintDcForHwnd(hwnd, dc)', 'EqualRect(&client, rect)',
                      'IsHotKey(hwnd)', 'GetSysColor(COLOR_WINDOW)', 'BS_SOLID'):
            self.assertIn(guard, code)
        self.assertIn('g_editSessionStack.back().paintDc == hdc', edit)
        self.assertIn('numberBackgroundCleared = true', edit)
        self.assertIn('paint.hotKeyFillHook && paint.paintDc == hdc', edit)
        self.assertIn('EnableHooksOnModule(callerMod, EHK_FILLRECT, true)', edit)
        self.assertIn('if (s.hotKeyFillHook) EnableHooksOnModule(s.mod, EHK_FILLRECT, false)', edit)
        self.assertIn('CaptureNumberBoxDefaultBackground', root)
        self.assertIn('color == (GetBkColor(dc) & 0xFFFFFFu)', root)
        self.assertIn('SetDelphiInputDefaultBackground(hWnd, false)', read('WndCompat/Controls/EditSubclass.cpp'))
        for path in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            self.assertIn('TInputHooks.cpp', read(path))
            self.assertIn('TInputHooks.h', read(path))

    def test_checklist_white_background_probe_is_opt_in_and_stage_correlated(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        probe = code.split('#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)', 2)[2].split('        template', 1)[0]
        for marker in ('CheckProbeBudget()', 'g_diagBudget = 96', 'GetPixel(s.dc',
                       'PtInRect(&s.item, points[i])', 'GetBufferedPaintBits', 'GdiFlush()',
                       'alpha0=%u alpha255=%u alphaPartial=%u whiteNonzeroAlpha=%u',
                       'rgbOutsideLeft', 'rgbOutsideRight', 'CLR_INVALID', 'SetLastError(lastError)'):
            self.assertIn(marker, probe)
        self.assertNotIn('SelectClipRgn', probe)
        self.assertNotIn('ExcludeClipRect', probe)
        for marker in ('L"theme.before"', 'L"theme.after"', 'L"item.end"',
                       'LogCheckBuffer(session, buffer, *rect, state)', 'L"system-background"',
                       'L"alpha-brush"'):
            self.assertIn(marker, code)
        bg = read('GlassCore/src/Hooks/delphi/TBackgroundHooks.cpp')
        self.assertNotIn('checklist initial background', bg)

    def test_checkbox_uses_scoped_theme_drawing_without_geometry_heuristics(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        theme = code.split('bool TryCheckListThemeBackground(', 1)[1].split('void EndDraw()', 1)[0]
        for guard in ('part != BP_CHECKBOX', '!current->checkList',
                      '!current->themeDetourAcquired', 'GetThemeClass(theme',
                      '_wcsicmp(themeClass, L"Button")', 'g_inCheckTheme',
                      'BeginBufferedPaint(dc, rect, BPBF_TOPDOWNDIB',
                      'AC_SRC_ALPHA', 'BufferedPaintClear(buffer, nullptr)',
                      'orig(theme, bufferDc, part, state, rect, clip)',
                      'EndBufferedPaint(buffer, SUCCEEDED(draw))',
                      'FAILED(draw) || FAILED(end)'):
            self.assertIn(guard, theme)
        for forbidden in ('ExcludeClipRect', 'IntersectClipRect', 'RectInRegion',
                          'GetClipRgn', 'WindowFromDC', 'ChromaKey'):
            self.assertNotIn(forbidden, theme)
        fill = code.split('int WINAPI MyFill(HDC dc', 1)[1].split('BOOL WINAPI MyExtText(HDC dc', 1)[0]
        for forbidden in ('GetClipBox', 'COMPLEXREGION', 'SIMPLEREGION', 'checkOutline'):
            self.assertNotIn(forbidden, fill)
        self.assertIn('SystemBackground(lb.lbColor)', fill)
        self.assertIn('if (s.themeDetourAcquired)', code)
        self.assertIn('EnableThemeHooks(true)', code)
        self.assertIn('EnableThemeHooks(false)', code)
        ux = read('GlassCore/src/Hooks/UxThemeHooks.cpp')
        self.assertIn('DelphiListBox::TryCheckListThemeBackground', ux)
        self.assertIn('Button::TryDelphiBitBtnThemeBgDetour', ux)

    def test_checklist_recolors_only_vcl_background_outline_after_theme(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        draw = code.split('BOOL WINAPI MyRectangle(HDC dc,', 1)[1].split('int WINAPI MyFill(HDC dc', 1)[0]
        for guard in ('Original<RectangleSlot, decltype(&::Rectangle)>()',
                      '!current->checkList', '!current->pendingCheckboxBackgroundOutline',
                      'g_stack.back().pendingCheckboxBackgroundOutline = false',
                      'EqualRect(&r, &session.checkboxThemeRect)',
                      'brush.lbStyle != BS_NULL', 'pen.lopnStyle != PS_SOLID',
                      'pen.lopnWidth.x != 1', 'GetSysColor(COLOR_WINDOW)',
                      'GetStockObject(BLACK_PEN)', 'SelectObject(dc, backgroundPen)',
                      'const BOOL result = orig(dc, left, top, right, bottom)',
                      'SelectObject(dc, oldPen)', 'SetLastError(error)',
                      'return orig(dc, left, top, right, bottom)'):
            self.assertIn(guard, draw)
        for forbidden in ('ExcludeClipRect', 'IntersectClipRect', 'PatBlt', 'RectInRegion'):
            self.assertNotIn(forbidden, draw)
        theme = code.split('bool TryCheckListThemeBackground(', 1)[1].split('void EndDraw()', 1)[0]
        self.assertLess(theme.index('pendingCheckboxBackgroundOutline = false'),
                        theme.index('const HRESULT draw = orig('))
        self.assertLess(theme.index('if (FAILED(draw) || FAILED(end)) return false;'),
                        theme.index('pendingCheckboxBackgroundOutline = true'))
        self.assertIn('g_stack.back().serial == session.serial', theme)
        self.assertIn('g_stack.back().checkboxThemeRect = *rect', theme)
        self.assertIn('if (i == RectangleSlot && !s.checkList) continue', code)
        self.assertIn('if (s.hooks[i]) g_dispatcher.EnableHook(i, false)', code)

    def test_vcl_draw_hook_slots_do_not_shadow_winapi_macros(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        self.assertIn('FillSlot, ExtTextSlot, DrawTextSlot, RectangleSlot,', code)
        self.assertIn('BitBltSlot, StretchBltSlot,', code)
        self.assertIn('Original<StretchBltSlot, decltype(&::StretchBlt)>()', code)
        self.assertIn('Original<BitBltSlot, decltype(&::BitBlt)>()', code)
        self.assertIn('Original<DrawTextSlot, decltype(&::DrawTextW)>()', code)
        self.assertIn('Original<FillSlot, decltype(&::FillRect)>()', code)
        self.assertIn('Original<ExtTextSlot, decltype(&::ExtTextOutW)>()', code)
        self.assertNotRegex(code, r'\bOriginal<DrawText\s*,')

    def test_spin_edit_uses_edit_and_input_color_probe_is_bounded(self):
        classifier = read('WndCompat/Tracker/WindowClassifier.cpp')
        self.assertRegex(classifier, r'(?s)if\s*\([^;{}]*L"TSpinEdit"[^;{}]*\)\s*return ControlKind::Edit\s*;')
        root = read('WndCompat/Root/RootSubclass.cpp')
        probe = root.split('static void LogDelphiInputCtlColor', 1)[1].split('#endif', 1)[0]
        for cls in ('TNumberBox', 'THotKey', 'TSpinEdit'):
            self.assertIn('L"' + cls + '"', probe)
        self.assertIn('budget = 24', probe)
        self.assertIn('[WndCompat][DelphiInputColor]', probe)

    def test_vcl_draw_probes_are_opt_in_and_balanced(self):
        header = read("GlassCore/Include/Hooks/RootGdiHooks.h")
        gdi = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        root = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        guard = '#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)'
        self.assertIn(guard + '\n        OHK_VCLPROBE_FILL', header)
        self.assertIn(guard + '\n        struct VclProbeSession', gdi)
        self.assertIn(guard + '\n    static void VclProbePaintBegin', root)
        self.assertIn('[GlassCore][DelphiProbeBuild] optional IAT probe compiled=0', root)
        self.assertIn('[GlassCore][DelphiProbeBuild] optional IAT probe compiled=1', root)
        self.assertIn('session.acquired[i] = AcquireVclProbeHook(ep, i)', gdi)
        self.assertIn('if (session.acquired[i]) EnableRootHook(session.module, i, false)', gdi)
        self.assertIn('it->second.GetRef(index) > before', gdi)
        self.assertIn('L"TBitBtn"', gdi)
        self.assertIn('L"TBitBtn"', read('GlassCore/src/Subsystems/PaintDispatcher.cpp'))
        for source in (gdi, read('GlassCore/src/Subsystems/PaintDispatcher.cpp')):
            self.assertIn('L"TToggleSwitch"', source)
        for api in ('StretchBlt', 'DrawFocusRect', 'Rectangle', 'FillRect', 'BitBlt', 'DrawTextW'):
            self.assertIn('"' + api + '"', gdi)
        self.assertIn('i <= OHK_VCLPROBE_FOCUSRECT', gdi)
        self.assertIn('[WndCompat][DelphiBitBtn] WM_DRAWITEM', read('WndCompat/Root/RootSubclass.cpp'))
        self.assertNotIn('DDH_All', root.split('static void VclProbePaintBegin', 1)[1].split('#endif', 1)[0])

    def test_vcl_probe_has_ansi_direct_and_tabbed_native_slots(self):
        code = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        header = read('GlassCore/Include/Hooks/RootGdiHooks.h')
        table = code.split('g_rootHookDep', 1)[1].split('using RootDispatcher', 1)[0]
        probe_enum = header.split('OHK_VCLPROBE_FILL', 1)[1].split('#endif', 1)[0]
        for api, slot, hook in (
            ('DrawTextA', 'DRAWTEXTA', 'VclProbeDrawTextA'),
            ('DrawTextExA', 'DRAWTEXTEXA', 'VclProbeDrawTextExA'),
            ('ExtTextOutA', 'TEXTA', 'VclProbeTextA'),
            ('TextOutA', 'TEXTOUTA', 'VclProbeTextOutA'),
            ('TextOutW', 'TEXTOUTW', 'VclProbeTextOutW'),
            ('TabbedTextOutA', 'TABBEDTEXTOUTA', 'VclProbeTabbedTextOutA'),
            ('TabbedTextOutW', 'TABBEDTEXTOUTW', 'VclProbeTabbedTextOutW'),
        ):
            with self.subTest(api=api):
                self.assertIn('OHK_VCLPROBE_' + slot, probe_enum)
                self.assertLess(probe_enum.index('OHK_VCLPROBE_' + slot),
                                probe_enum.index('OHK_VCLPROBE_FOCUSRECT'))
                self.assertIn('"' + api + '", reinterpret_cast<PVOID>(' + hook + ')', table)
                body = re.search(r'\b' + hook + r'\(HDC dc,.*?\n        \}', code, re.S).group()
                self.assertIn('VclProbeOriginal<OHK_VCLPROBE_' + slot + ', decltype(&' + api + ')>()', body)
                calls = re.search(r'return orig \? orig\((.*?)\) : ' + api + r'\((.*?)\);', body, re.S)
                self.assertIsNotNone(calls)
                self.assertEqual(calls.group(1), calls.group(2))
                for forbidden in ('strlen', 'wcslen', 'MultiByteToWideChar', 'DrawTextWithAlpha',
                                  'DrawThemeContent', 'PatBlt', 'DT_CALCRECT', 'text['):
                    self.assertNotIn(forbidden, body)

    def test_vcl_probe_tpage_is_exact_and_does_not_change_rendering_policy(self):
        code = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        begin = code.split('void BeginVclDrawProbe(HWND hwnd, bool paint)', 1)[1].split('void EndVclDrawProbe(', 1)[0]
        self.assertIn('_wcsicmp(cls, L"TPage") != 0', begin)
        self.assertNotIn('TPageControl', begin)
        self.assertNotIn('TPanel', begin)
        self.assertNotIn('StrStr', begin)
        self.assertIn('AcquireVclProbeHook(ep, i)', begin)
        root = read('GlassCore/src/Hooks/RootWindowHooks.cpp')
        providers = root.split('static void VclProbePaintBegin', 1)[1].split('#endif', 1)[0]
        self.assertEqual(providers.count('IsFullInjectionMode()'), 2)
        self.assertIn('BeginVclDrawProbe(hWnd, true)', providers)
        self.assertIn('BeginVclDrawProbe(hWnd, false)', providers)
        panel = read('GlassShared/include/GlassDelphiPanel.h').split(
            'inline bool IsDelphiPanelClassName(', 1)[1].split('inline bool IsDelphiPanelWindow(', 1)[0]
        self.assertNotIn('TPage', panel)  # text-only helper must not expand panel background policy
        self.assertNotIn('L"TPage"', root)

    def test_vcl_probe_lookups_logging_and_sessions_preserve_native_last_error(self):
        code = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        for start, end in (
            ('void BeginVclDrawProbe(HWND hwnd, bool paint)', 'void EndVclDrawProbe('),
            ('void EndVclDrawProbe(HWND hwnd, bool paint)', 'void ClearVclDrawProbe('),
            ('static Fn VclProbeOriginal()', 'static void LogVclProbe('),
            ('static void LogVclProbe(', 'int WINAPI VclProbeFill(HDC dc,'),
        ):
            with self.subTest(function=start):
                body = code.split(start, 1)[1].split(end, 1)[0]
                self.assertIn('const DWORD error = GetLastError()', body)
                self.assertIn('SetLastError(error)', body)
        original = code.split('static Fn VclProbeOriginal()', 1)[1].split('static void LogVclProbe(', 1)[0]
        self.assertIn('GetRootOrg<index, Fn>(GlassCore::ModuleHandler::g_entryPointModule)', original)
        probe = code.split('struct VclProbeSession', 1)[1].split('// ── EnableRootHook / DisableAllRootHooks', 1)[0]
        for forbidden in ('GetCallerModule', '_ReturnAddress', 'Detours::', 'WriteMemory',
                          'DrawTextWithAlpha', 'MultiByteToWideChar'):
            self.assertNotIn(forbidden, probe)
        for field in ('textColor=0x%08X', 'bkColor=0x%08X', 'bkMode=%d', 'c=%lld'):
            self.assertIn(field, probe)
        self.assertIn('budget = 160', probe)

    def test_toggle_restores_unknown_child_root_fallback(self):
        classifier = read('WndCompat/Tracker/WindowClassifier.cpp')
        classify = classifier.split('ClassifyResult WindowClassifier::Classify(HWND hwnd)', 1)[1]
        self.assertNotIn('L"TToggleSwitch"', classify)
        root = read('WndCompat/Root/RootSubclass.cpp')
        attach = root.split('void Root_Attach(HWND hRoot)', 1)[1].split('void Root_Detach(HWND hRoot)', 1)[0]
        notify = root.split('void Root_AttachDropDownNotify(HWND hWnd)', 1)[1].split('bool Root_DropDownNotifyAttached', 1)[0]
        self.assertNotIn('L"TToggleSwitch"', attach)
        self.assertNotIn('L"TToggleSwitch"', notify)
        event = read('WndCompat/Tracker/WinEventHandler.cpp')
        self.assertRegex(event, r'(?s)case ControlKind::Unknown:\s*if \(isFull && allowGlassForHwnd\).*?DispatchRootAttachToOwnerThread\(hWnd\)')

    def test_bitbtn_ownerdraw_is_scoped_to_drawitem_and_exact_dc(self):
        root = read('WndCompat/Root/RootSubclass.cpp')
        button = read('GlassCore/src/Hooks/ButtonHooks.cpp')
        code = read('GlassCore/src/Hooks/delphi/TBitBtnHooks.cpp')
        exports = read('GlassCore/src/Core/Exports.cpp')
        owner = root.split('using BeginFn = BOOL (WINAPI*)(HWND, const DRAWITEMSTRUCT*)', 1)[1].split('// Нас интересуют только пункты меню', 1)[0]
        self.assertIn('begin(hWnd, dis)', owner)
        self.assertIn('wil::scope_exit', owner)
        self.assertIn('return DefSubclassProc(hWnd, uMsg, wParam, lParam)', owner)
        self.assertIn('GlassCore_BeginDelphiBitBtnDraw', exports)
        begin = code.split('bool BeginDelphiBitBtnOwnerDraw', 1)[1].split('void EndDelphiBitBtnOwnerDraw', 1)[0]
        for guard in ('ODT_BUTTON', 'GetParent(dis->hwndItem) != parent',
                      'GlassIsBackdropAllowedForHwnd', 'BS_OWNERDRAW', 'L"TBitBtn"',
                      'dis->hDC', 'g_entryPointModule', 'AcquireDelphiBitBtnHook'):
            self.assertIn(guard, begin)
        self.assertIn('if (session.acquired[i]) ReleaseDelphiBitBtnHook(session.module, i)', code)
        self.assertIn('slotTheme=%d slotText=%d slotParent=%d', code)
        self.assertIn('hookInfoCache[hookIndex].first.IsValid()', button)
        self.assertIn('OHK_DELPHI_BITBTN_THEME + index', button)
        self.assertIn('importDtb=%d importDtbEx=%d importParent=%d importParentEx=%d', code)
        self.assertIn('static const std::array<bool, 4> namedImports', code)
        probe = code.split('static bool HasDelphiThemeImport', 1)[1].split('#endif', 1)[0]
        self.assertIn('Hooks::GetIAT', probe)
        self.assertIn('Hooks::GetDelayloadIAT', probe)
        self.assertNotIn('EnableHook', probe)
        self.assertIn('return dc && s.dc == dc ? &s : nullptr', code)
        self.assertIn('"DrawThemeBackground", reinterpret_cast<PVOID>(DelphiBitBtnThemeBg)', button)
        theme = code.split('HRESULT WINAPI DelphiBitBtnThemeBg(HTHEME theme', 1)[1].split('HRESULT WINAPI DelphiBitBtnThemeText(HTHEME theme', 1)[0]
        self.assertIn('GetDelphiBitBtnThemeOriginal', theme)
        self.assertIn('GetOverallOrigSafe<OHK_DELPHI_BITBTN_THEME', button)
        self.assertIn('GlassCore::ModuleHandler::g_entryPointModule', theme)
        shared = code.split('static bool TryDelphiBitBtnThemeBg(', 1)[1].split('HRESULT WINAPI DelphiBitBtnThemeBg(HTHEME theme', 1)[0]
        self.assertIn('IntersectRect(&overlap, rect, &session.rect)', shared)
        self.assertIn('IntersectRect(&drawClip, &session.rect, clip)', shared)
        self.assertIn('IntersectClipRect(dc, drawClip.left', shared)
        self.assertIn('RestoreDC(dc, saved)', shared)
        self.assertIn('g_delphiBitBtnInTheme', shared)
        self.assertIn('TryDelphiBitBtnThemeBg(orig,', theme)
        self.assertIn('themeDetourAcquired', shared)
        self.assertIn('TryDelphiBitBtnThemeBgDetour(', read('GlassCore/src/Hooks/UxThemeHooks.cpp'))
        self.assertIn('EnableThemeHooks(true)', begin)
        self.assertIn('EnableThemeHooks(false)', code.split('void EndDelphiBitBtnOwnerDraw()', 1)[1].split('[[nodiscard]]', 1)[0])
        self.assertIn('[GlassCore][DelphiBitBtn] DTB entry', theme)
        self.assertIn('part == BP_PUSHBUTTON', code)
        self.assertIn('DTT_CALCRECT | DTT_CALLBACK', code)
        self.assertIn('BLACKNESS', code.split('HRESULT WINAPI DelphiBitBtnParentBg(HWND hwnd', 1)[1].split('void ClearDelphiBitBtnDraw', 1)[0])
        # Ни glyph BitBlt, ни произвольные memory DC в owner-draw не подменяются.
        self.assertNotIn('OHK_BITBLT', begin)
        self.assertIn('!delphiBitBtn && !IsButtonIconOrBitmapStyle(hWnd)', button)
        self.assertIn('sess.delphiBitBtn || IsButtonIconOrBitmapStyle(sess.hWnd)', button)
        self.assertIn('s.kind, false, s.delphiBitBtn', button)
        self.assertIn('sess.kind == GK::Button && !ownerDrawSession', button)

    def test_delphi_hook_files_are_registered_in_the_windows_project(self):
        project = read('GlassCore/GlassCore.vcxproj')
        filters = read('GlassCore/GlassCore.vcxproj.filters')
        for file in (r'src\Hooks\delphi\TBitBtnHooks.cpp',
                     r'src\Hooks\delphi\TToggleSwitchHooks.cpp',
                     r'Include\Hooks\delphi\TBitBtnHooks.h',
                     r'Include\Hooks\delphi\TBitBtnBridge.h',
                     r'Include\Hooks\delphi\TToggleSwitchHooks.h'):
            self.assertIn(file, project)
            self.assertIn(file, filters)
        self.assertIn('#include <Subsystems/RootSwcaGradient.h>', read('GlassCore/src/Hooks/delphi/TBitBtnHooks.cpp'))
        self.assertNotIn('struct DelphiBitBtnDrawSession', read('GlassCore/src/Hooks/ButtonHooks.cpp'))
        self.assertNotIn('CaptureStackBackTrace', read('GlassCore/src/Subsystems/PaintDispatcher.cpp'))
        self.assertIn('ClearDelphiBitBtnDraw();', read('GlassCore/src/Hooks/ButtonHooks.cpp'))

    def test_containers_use_guarded_root_attach(self):
        source = read("WndCompat/Tracker/WinEventHandler.cpp")
        self.assertRegex(source, r'(?s)case ControlKind::DelphiContainer:\s*case ControlKind::GenericContainer:.*?if \(isFull && allowGlassForHwnd\).*?DispatchRootAttachToOwnerThread\(hWnd\);')

    def test_theme_text_is_session_scoped_and_balanced(self):
        root = read('GlassCore/src/Hooks/RootWindowHooks.cpp')
        gdi = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        panel = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        header = read('GlassCore/Include/Hooks/RootGdiHooks.h')
        self.assertIn('OHK_DELPHI_DRAWTHEMETEXTEX,', header)
        self.assertIn('"DrawThemeTextEx", reinterpret_cast<PVOID>(Root_MyDelphiDrawThemeTextEx)', gdi)
        gate = root.split('// VCL paints windowless labels and group captions', 1)[1].split('// VCL-таблицы', 1)[0]
        self.assertIn('instMod == GlassCore::ModuleHandler::g_entryPointModule', gate)
        self.assertIn('GlassShared::IsDelphiPanelClassName(cls)', gate)
        self.assertIn('PushPaint(instMod, OHK_DELPHI_DRAWTHEMETEXTEX, cache)', gate)
        self.assertIn('PushDc(instMod, OHK_DELPHI_DRAWTHEMETEXTEX, cache)', gate)
        text = panel.split('HRESULT DrawContainerThemeTextEx(', 1)[1].split('bool BeginPaint(', 1)[0]
        for guard in ('hwnd != owner', 'IsPanelDc(dc)', 'GlassIsBackdropAllowedForHwnd(hwnd)',
                      'IsFullInjectionMode()', 'DT_CALCRECT | DT_MODIFYSTRING',
                      'options->dwSize != sizeof(DTTOPTS)', 'DTT_CALCRECT | DTT_CALLBACK'):
            self.assertIn(guard, text)
        self.assertIn('GlassShared::IsDelphiPanelClassName(name)', text)
        for name in ('TGroupBox', 'TTabSheet', 'TCard', 'TRadioGroup'):
            self.assertIn('L"' + name + '"', gate)
            self.assertIn('L"' + name + '"', text)
        # Keep both existing panels and all three probe classes in the shared gate.
        classes = read('GlassShared/include/GlassDelphiPanel.h')
        for name in ('TPanel', 'TScrollBox', 'TFrame', 'TSampleFrame', 'TEmbeddedForm'):
            self.assertIn('_wcsicmp(name, L"' + name + '") == 0', classes)

    def test_panel_theme_text_keeps_options_and_reuses_transparent_compositor(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        text = code.split('HRESULT DrawContainerThemeTextEx(', 1)[1].split('bool BeginPaint(', 1)[0]
        for marker in ('if (options) work = *options', 'work.dwFlags |= DTT_COMPOSITED',
                       'RECT drawRect = *rect', 'GlassCore::Text::DrawThemeContent(dc, *rect',
                       'original(theme, memoryDc, part, state, text, count, flags, &drawRect, &work)',
                       '0xFF, false, true, false', 'g_inHook = true', 'wil::scope_exit',
                       'return original(theme, dc, part, state, text, count, flags, rect, options)',
                       'TakeDiag(2)', '[GlassCore][DelphiPanel] text api=DrawThemeTextEx'):
            self.assertIn(marker, text)
        for forbidden in ('PatBlt(', 'FillRect(', 'ExtTextOutWithAlpha', 'DrawTextWithAlpha',
                          'GetThemeColor(', 'GetWindowTheme(', 'work.crText =', 'work.iGlowSize ='):
            self.assertNotIn(forbidden, text)

    def test_panel_theme_original_is_resolved_by_the_installed_exe_slot(self):
        code = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        wrapper = code.split('HRESULT WINAPI Root_MyDelphiDrawThemeTextEx(\n', 1)[1].split('BOOL WINAPI Root_MyExtTextOutW(', 1)[0]
        self.assertIn('GetRootOrg<OHK_DELPHI_DRAWTHEMETEXTEX, decltype(&::DrawThemeTextEx)>', wrapper)
        self.assertIn('GlassCore::ModuleHandler::g_entryPointModule', wrapper)
        self.assertIn('GlassCore::DelphiPanel::DrawContainerThemeTextEx(', wrapper)
        self.assertIn('orig, theme, dc, part, state, text, count, flags, rect, options', wrapper)
        self.assertNotIn('_ReturnAddress', wrapper)
        self.assertNotIn('GetCallerModule', wrapper)

    def test_vcl_tab_paint_hooks_drawing_modules_symmetrically(self):
        source = read("GlassCore/src/Hooks/TabHooks.cpp")
        self.assertIn('L"TPageControl"', source)
        self.assertIn('L"TTabControl"', source)
        self.assertIn("vclComctlNew == callerMod", source)
        self.assertIn("vclComctlLegacy == callerMod || vclComctlLegacy == vclComctlNew", source)
        for module in ("vclComctlNew", "vclComctlLegacy"):
            self.assertIn(f"EnableTabHooks(session.{module}, true, session)", source)
            self.assertIn(f"EnableTabHooks(top.{module}, false, top)", source)

    def test_background_diagnostics_use_existing_handlers(self):
        root = read("WndCompat/Root/RootSubclass.cpp")
        lv = read("GlassCore/src/Hooks/ListViewHooks.cpp")
        self.assertIn('[WndCompat][DelphiMemo] CTLCOLOR', root)
        self.assertIn('WndCompat::IsWindowClass(child, L"TMemo")', root)
        self.assertIn('budget = 16', root)
        self.assertIn('now - epoch >= 1000', root)
        self.assertIn('LogDelphiMemoCtlColor(hWnd, uMsg', root)
        self.assertIn('[GlassCore][DelphiListView] Begin', lv)
        self.assertIn('[GlassCore][DelphiListView] FillRect', lv)
        self.assertIn('AllowDelphiListViewLog(0)', lv)
        self.assertIn('AllowDelphiListViewLog(1)', lv)
        self.assertIn('AllowDelphiListViewLog(2)', lv)
        self.assertIn('[GlassCore][DelphiListView] PostBegin', lv)
        self.assertIn('now - epoch[channel] >= 1000', lv)

    def test_edit_getdc_preserves_active_paint_state(self):
        source = read("GlassCore/src/Hooks/EditHooks.cpp")
        getdc = source.split("static void OnGetDCEdit", 1)[1].split("static void OnReleaseDCEdit", 1)[0]
        self.assertRegex(getdc, r'(?s)if \(g_editSessionStack.empty\(\) && g_editDcStack.empty\(\)\)\s*\{\s*g_bypassEditBuffer = false;\s*g_sessionBlacknessRects.clear\(\);\s*\}')
        self.assertIn('[GlassCore][DelphiMemo] ExtTextOut', source)
        self.assertIn('[GlassCore][DelphiMemo] Flush', source)
        self.assertIn('_wcsicmp(cls, L"TMemo")', source)
        self.assertIn('const bool isDib = composeBits != nullptr', source)
        self.assertIn('buf->height, &composeBits)', source)

    def test_memo_buffer_uses_live_beginpaint_dc(self):
        source = read("GlassCore/src/Hooks/EditHooks.cpp")
        core = source.split("static BOOL Edit_WorkExtTextOutCore(", 1)[1].split("SyncActiveEditBufferDualPassFromGlobals();", 1)[0]
        self.assertIn('!g_editSessionStack.empty() && (IsDelphiMemo(sessionHwnd)', core)
        self.assertIn('GlassCore::DelphiInput::IsSpinEdit(sessionHwnd)', core)
        for guard in ('paint.hwnd != sessionHwnd', '!paint.paintDc', 'paint.paintDc != hdc',
                      'IsCurrentPaintDcForHwnd(sessionHwnd, hdc)',
                      'g_activeEditBuffer->screenDC != paint.paintDc'):
            self.assertIn(guard, core)
        self.assertIn('return origW(hdc, x, y, options, lprect, lpString, c, lpDx);', core)
        post = source.split('static bool OnEditAfterBeginPaint(', 1)[1].split('static void OnEndPaintEdit(', 1)[0]
        self.assertIn('!IsDelphiMemo(hWnd)', post)
        self.assertIn('g_editSessionStack.back().hwnd != hWnd', post)
        self.assertIn('g_editSessionStack.back().paintDc = hdc;', post)
        self.assertIn('return false;', post)
        self.assertIn('RegisterPostPaintProvider({ OnEditAfterBeginPaint })', source)
        dispatcher = read('GlassCore/src/Subsystems/PaintDispatcher.cpp')
        lookup = dispatcher.split('bool IsCurrentPaintDcForHwnd(', 1)[1].split('// Tier-1:', 1)[0]
        self.assertIn('g_dcStack.rbegin()', lookup)
        self.assertIn('if (it->hDC == hdc)', lookup)
        self.assertIn('it->hWnd == hWnd && it->type == DcEntryType::Paint', lookup)
        self.assertIn('[GlassCore][DelphiMemo] BufferTarget', source)
        self.assertIn('[GlassCore][DelphiMemo] ComposeFallback', source)

    def test_theme_detour_transaction_failure_rolls_back_ref(self):
        ux = read('GlassCore/src/Hooks/UxThemeHooks.cpp')
        body = ux.split('bool EnableThemeHooks(bool enable)', 1)[1].split('void BeginMenuBarUxThemeDraw', 1)[0]
        self.assertIn('const HRESULT detourHr', body)
        self.assertIn('if (FAILED(detourHr))', body)
        self.assertIn('g_themeRef += enable ? static_cast<size_t>(-1) : 1', body)
        self.assertLess(body.index('if (FAILED(detourHr))'), body.index('g_uxThemeDispatcher.EnableHook'))
        bit = read('GlassCore/src/Hooks/delphi/TBitBtnHooks.cpp')
        self.assertIn('if (!orig || g_delphiBitBtnInTheme) return false', bit)
        self.assertIn('if (session.themeDetourAcquired)', bit)
        self.assertIn('EnableThemeHooks(false)', bit)

    def test_toggle_alpha_hooks_are_session_and_buffer_scoped(self):
        code = read('GlassCore/src/Hooks/delphi/TToggleSwitchHooks.cpp')
        engine = read('GlassCore/src/Core/Engine.cpp')
        self.assertIn('GlassCore::DelphiToggle::Startup()', engine)
        self.assertIn('GlassCore::DelphiToggle::Shutdown()', engine)
        for guard in ('IsFullInjectionMode()', 'GlassIsBackdropAllowedForHwnd(hwnd)',
                      'caller == GlassCore::ModuleHandler::g_entryPointModule',
                      'IsTopWindowDrawingSession(s.hwnd)', 'GetStackOwnerForDc(dc) != s.hwnd',
                      'GetObjectType(dc) != OBJ_MEMDC', 'GetMapMode(dc) != MM_TEXT',
                      'info.bmWidth == client.right', 'info.bmHeight == client.bottom',
                      'bitmap != s.trackBitmap'):
            self.assertIn(guard, code)
        self.assertIn('"Rectangle", reinterpret_cast<void*>(ToggleRectangle)', code)
        self.assertIn('"FillRect", reinterpret_cast<void*>(ToggleFillRect)', code)
        self.assertIn('LayeredStrokePen::TryCreateSolid(lp, 255)', code)
        self.assertIn('CreateSolidColorBrushWithAlpha(lb.lbColor, 255)', code)
        self.assertIn('SelectObject(dc, prevPen)', code)
        self.assertIn('SelectObject(dc, prevBrush)', code)
        self.assertIn('return orig(dc, rect, alpha.get())', code)
        self.assertIn('session.eligible', code)
        self.assertIn('g_sessions.back().serial == sessionSerial', code)
        self.assertIn('RegisterGetDCProvider({BeginDcSession, EndDcSession})', code)
        root = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        self.assertNotIn('PreservePaintDcBkColor', root)
        self.assertIn('SetBkColor: entryPoint COLOR_WINDOW→0', root)

    def test_toggle_getdc_full_copy_requests_one_async_paint(self):
        code = read('GlassCore/src/Hooks/delphi/TToggleSwitchHooks.cpp')
        enum = code.split('enum : size_t { TrackRectangle', 1)[1].split('HookCount };', 1)[0]
        self.assertIn('CopyStretchBlt,', enum.split('#if defined(_DEBUG)', 1)[0])
        self.assertIn('CopyBitBlt,', enum.split('#if defined(_DEBUG)', 1)[1])
        self.assertIn('if (paint && i == CopyStretchBlt) continue;', code)
        body = code.rsplit('BOOL WINAPI ToggleStretchBlt(', 1)[1].split('\n        }', 1)[0]
        for guard in ('g_sessions.back().windowDC == dest', 'WindowFromDC(dest) == g_sessions.back().hwnd',
                      'IsTopWindowDrawingSession', 'HasOpenPaintSessionForHwnd(session.hwnd)',
                      'rop == SRCCOPY', 'GetObjectType(source) == OBJ_MEMDC',
                      'x == 0 && y == 0 && sx == 0 && sy == 0',
                      'width == client.right && height == client.bottom',
                      'bitmap.bmWidth == width && bitmap.bmHeight == height',
                      'g_sessions.back().serial == session.serial',
                      'g_sessions.back().copyInvalidated = true',
                      'InvalidateRect(session.hwnd, nullptr, FALSE)'):
            self.assertIn(guard, body)
        self.assertEqual(body.count('const BOOL result = orig('), 1)
        self.assertLess(body.index('const BOOL result = orig('), body.index('InvalidateRect('))
        self.assertIn('if (result && refresh', body)
        self.assertIn('SetLastError(entryError)', body)
        self.assertIn('SetLastError(error)', body)
        self.assertIn('return result;', body)
        self.assertNotIn('UpdateWindow(', body)
        self.assertNotIn('GdiAlphaBlend(', body)
        dispatcher = read('GlassCore/src/Subsystems/PaintDispatcher.cpp')
        lookup = dispatcher.split('bool HasOpenPaintSessionForHwnd(', 1)[1].split('bool IsCurrentPaintDcForHwnd(', 1)[0]
        self.assertIn('it->type == DcEntryType::Paint && it->hWnd == hWnd', lookup)

    def test_toggle_bitblt_probe_remains_debug_only_and_observational(self):
        code = read('GlassCore/src/Hooks/delphi/TToggleSwitchHooks.cpp')
        body = code.rsplit('BOOL WINAPI TraceBitBlt(', 1)[1].split('\n        }', 1)[0]
        self.assertIn('g_sessions.back().windowDC == dest', body)
        self.assertIn('WindowFromDC(dest) == g_sessions.back().hwnd', body)
        self.assertEqual(body.count('const BOOL result = orig('), 1)
        self.assertIn('SetLastError(entryError)', body)
        self.assertIn('SetLastError(error)', body)
        self.assertIn('return result;', body)
        self.assertIn('budget = 32', code)
        self.assertIn('LogCopy(L"StretchBlt"', code)

    def test_toggle_delete_trace_is_debug_bounded_and_observational(self):
        source = read('GlassCore/src/Subsystems/PaintDispatcher.cpp')
        trace = read('GlassCore/src/Hooks/delphi/TToggleSwitchHooks.cpp')
        for guard in ('L"TToggleSwitch"', 'OBJ_MEMDC', 'total >= 128', 'budget = 32',
                      'CaptureStackBackTrace', 'not DC ownership', 'GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT'):
            self.assertIn(guard, trace)
        self.assertIn('it->type == DcEntryType::Paint', source)
        self.assertIn('IsFullInjectionMode()', source)
        self.assertIn('#if defined(_DEBUG)', trace)
        body = source.split('static BOOL WINAPI HookDeleteDC(HDC hdc)', 1)[1].split('// ── Lifecycle', 1)[0]
        self.assertEqual(body.count('g_origDeleteDC(hdc)'), 1)
        self.assertLess(body.index('CaptureDelphiToggleDelete('), body.index('PopDC(hdc,'))
        self.assertLess(body.index('PopDC(hdc,'), body.index('g_origDeleteDC(hdc)'))
        self.assertIn('SetLastError(beforeTraceError)', body)
        self.assertIn('SetLastError(originalError)', body)
        self.assertIn('return result;', body)

    def test_richedit_and_memo_background_use_alpha_brush_for_every_non_window_color(self):
        source = read('GlassCore/src/Hooks/EditHooks.cpp')
        apply = source.split('static void ApplyEditBackgroundRect(', 1)[1].split('static COLORREF GetRichEdit20BackgroundFromEm', 1)[0]
        self.assertRegex(apply, r'const bool fill = IsRichEditKind\(GlassShared::GlassGetWindowKind\(sessionHwnd\)\) \|\| IsDelphiMemo\(sessionHwnd\)\s*'
                               r'\? rgb != sysWnd : rgb != 0 && rgb != sysBtn && rgb != sysWnd;')
        self.assertRegex(apply, r'CreateSolidColorBrushWithAlpha\(color,\s*rgb == \(GetSysColor\(COLOR_HIGHLIGHT\) & 0xFFFFFFu\) \? 255 : 100\)')
        self.assertRegex(apply, r'(?s)if \(!fill\)\s*\{[^}]*\}\s*else\s*\{\s*FillRect\(targetDC, &r, brush.get\(\)\);')
        self.assertNotIn('rgb == 0 || rgb == sysBtn || rgb == sysWnd', apply)

    def test_memo_background_is_composed_separately_from_text(self):
        source = read("GlassCore/src/Hooks/EditHooks.cpp")
        self.assertIn('std::vector<MemoBackgroundOp> memoBackgrounds;', source)
        apply = source.split('static void ApplyEditBackgroundRect(', 1)[1].split('static COLORREF GetRichEdit20BackgroundFromEm', 1)[0]
        self.assertRegex(apply, r'(?s)PatBlt\(targetDC.*?if \(useSession && g_activeEditBuffer && targetDC == g_activeEditBuffer->memDC\).*?if \(g_activeEditBuffer->delphiMemo\).*?memoBackgrounds.push_back\(\{ r, color, fill \}\);\s*return;')
        self.assertIn('PushSessionBlacknessRect(r,', apply)
        self.assertIn('FillRect(targetDC, &r, brush.get())', apply)
        replay = source.split('static void ReplayMemoBackgrounds(', 1)[1].split('// ── FlushEditBuffer', 1)[0]
        self.assertRegex(replay, r'(?s)for \(const auto& op : buf.memoBackgrounds\).*?PatBlt\(dc.*?if \(op.fill\).*?CreateSolidColorBrushWithAlpha\(op.color,\s*\(op.color & 0xFFFFFFu\) == \(GetSysColor\(COLOR_HIGHLIGHT\) & 0xFFFFFFu\) \? 255 : 100\).*?FillRect\(dc')
        flush = source.split('static void FlushEditBuffer()', 1)[1].split('static void ApplyEditBackgroundRect(', 1)[0]
        self.assertRegex(flush, r'(?s)if \(buf->delphiMemo\)\s*\{.*?ReplayMemoBackgrounds\(composeDC, \*buf\);.*?GdiAlphaBlend\(composeDC')
        self.assertRegex(flush, r'(?s)if \(buf->delphiMemo\)\s*\{.*?ReplayMemoBackgrounds\(buf->screenDC, \*buf\);.*?GdiAlphaBlend\(buf->screenDC')
        self.assertIn('memoBackgrounds=%zu', flush)

    def test_memo_solid_background_requires_current_color_confirmation(self):
        root = read('WndCompat/Root/RootSubclass.cpp')
        capture = root.split('const bool validBrush = GetObject(', 1)[1].split('if (validBrush)', 1)[0]
        for guard in ('WM_CTLCOLORSTATIC', 'L"TMemo"', 'validBrush', 'BS_SOLID', 'OPAQUE',
                      'GetBkColor(hdc)', 'COLOR_WINDOW'):
            self.assertIn(guard, capture)
        self.assertIn('SetDelphiMemoBackground(memo, rgb)', capture)
        self.assertIn('ClearDelphiMemoBackground(memo)', capture)
        self.assertNotIn('rgb != 0', capture)
        self.assertNotIn('COLOR_BTNFACE', capture)
        self.assertIn('if (!memoBackground) SetBkColor(hdc, 0)', root)
        self.assertIn('uMsg != WM_CTLCOLOREDIT || !WndCompat::IsWindowClass(reinterpret_cast<HWND>(lParam), L"TMemo")', root)
        edit_host = read('WndCompat/Controls/EditSubclass.cpp')
        self.assertEqual(edit_host.count('GlassShared::ClearDelphiMemoBackground(hWnd)'), 2)
        prop = read('GlassShared/include/GlassDelphiMemoBackground.h')
        self.assertIn('(color & 0xFFFFFFu) + 1u', prop)
        self.assertIn('value > 0x1000000u', prop)
        edit = read('GlassCore/src/Hooks/EditHooks.cpp')
        self.assertIn('if (buf->delphiMemo)\n            buf->memoSolidBackground = GlassShared::GetDelphiMemoBackground(hWnd);', edit)
        confirm = edit.split('static bool HasConfirmedMemoSolidBackground(', 1)[1].split('static bool PrimeMemoSolidBackground(', 1)[0]
        self.assertIn('buf.memoSolidBackground == CLR_INVALID', confirm)
        self.assertIn('op.fill && (op.color & 0xFFFFFFu) == buf.memoSolidBackground', confirm)
        prime = edit.split('static bool PrimeMemoSolidBackground(', 1)[1].split('static void ReplayMemoBackgrounds(', 1)[0]
        self.assertIn('!HasConfirmedMemoSolidBackground(buf)', prime)
        self.assertRegex(prime, r'CreateSolidColorBrushWithAlpha\(buf.memoSolidBackground,\s*\(buf.memoSolidBackground & 0xFFFFFFu\) == \(GetSysColor\(COLOR_HIGHLIGHT\) & 0xFFFFFFu\) \? 255 : 100\)')
        flush = edit.split('static void FlushEditBuffer()', 1)[1].split('static void ApplyEditBackgroundRect(', 1)[0]
        for dc in ('composeDC', 'buf->screenDC'):
            self.assertLess(flush.index(f'PrimeMemoSolidBackground({dc}, *buf)'), flush.index(f'ReplayMemoBackgrounds({dc}, *buf)'))
        self.assertIn('BitBlt(buf->screenDC', flush)
        self.assertIn('SolidBackground hwnd=', flush)

    def test_memo_dib_is_scoped_and_listview_black_brush_is_preserved(self):
        edit = read("GlassCore/src/Hooks/EditHooks.cpp")
        self.assertIn('buf->delphiMemo = IsDelphiMemo(hWnd)', edit)
        self.assertIn('bmi.bmiHeader.biBitCount = 32', edit)
        self.assertIn('? CreateMemoComposeBitmap(bltSrc, rc.right, rc.bottom) : nullptr', edit)
        self.assertRegex(edit, r'(?s)if \(buf->delphiMemo\)\s*composeBmp = CreateMemoComposeBitmap.*?if \(!composeBmp\)\s*composeBmp = CreateCompatibleBitmap')
        lv = read("GlassCore/src/Hooks/ListViewHooks.cpp")
        self.assertRegex(lv, r'(?s)if \(!lb.lbColor\)\s*return orig\(hdc, lprc, hbr\);')

    def test_vcl_application_window_not_subclassed(self):
        classifier = read("WndCompat/Tracker/WindowClassifier.cpp")
        root = read("WndCompat/Root/RootSubclass.cpp")
        self.assertRegex(classifier, r'(?s)if \(Equals\(cls, L"TApplication"\)\)\s*return \{\};')
        self.assertRegex(classifier, r'(?s)if \(Equals\(cls, L"TPUtilWindow"\)\)\s*return \{\};')
        attach = root.split("void Root_Attach(HWND hRoot)", 1)[1].split("void Root_Detach(HWND hRoot)", 1)[0]
        self.assertRegex(attach, r'(?s)if \(_wcsicmp\(rootClassName, L"TApplication"\) == 0\).*?return;.*?SetWindowSubclass')
        self.assertRegex(attach, r'(?s)if \(_wcsicmp\(rootClassName, L"TPUtilWindow"\) == 0\).*?return;.*?SetWindowSubclass')
        notify = root.split('void Root_AttachDropDownNotify(HWND hWnd)', 1)[1].split('bool Root_DropDownNotifyAttached', 1)[0]
        self.assertRegex(notify, r'(?s)L"TPUtilWindow".*?return;.*?SetWindowSubclass')

    def test_treeview_diagnostics_do_not_add_hooks(self):
        tree = read("GlassCore/src/Hooks/TreeViewHooks.cpp")
        self.assertIn('[GlassCore][DelphiTreeView] Begin', tree)
        self.assertIn('[GlassCore][DelphiTreeView] FillRect', tree)
        self.assertIn('AllowDelphiTreeLog(hWnd, 0)', tree)
        self.assertIn('AllowDelphiTreeLog(g_treeSessionStack.back().hWnd, 1)', tree)
        self.assertIn('now - epoch[channel] >= 1000', tree)

    def test_vcl_scroll_cleanup_uses_post_begin_paint_dc(self):
        lv = read("GlassCore/src/Hooks/ListViewHooks.cpp")
        post = lv.split('static bool OnListViewAfterBeginPaint', 1)[1].split('//  Lifecycle', 1)[0]
        self.assertIn('g_lvSessionStack.back().hWnd != hWnd', post)
        self.assertIn('session.needBlackout || session.vclListView', post)
        self.assertIn('PatBlt(hdc,', post)
        self.assertIn('ps->rcPaint.right - ps->rcPaint.left', post)
        self.assertIn('if (!cleared)', post)
        self.assertIn('const bool vclListView = IsDelphiListView(hWnd)', lv)
        tree = read("GlassCore/src/Hooks/TreeViewHooks.cpp")
        post = tree.split('static bool OnDelphiTreeAfterBeginPaint', 1)[1].split('//  Lifecycle', 1)[0]
        for guard in ('g_treeSessionStack.back().hWnd != hWnd', '!IsDelphiTreeView(hWnd)',
                      'IsRectEmpty(&ps->rcPaint)', 'return cleared != FALSE'):
            self.assertIn(guard, post)
        self.assertIn('PatBlt(hdc, ps->rcPaint.left, ps->rcPaint.top', post)
        self.assertIn('RegisterPostPaintProvider({ OnDelphiTreeAfterBeginPaint })', tree)

    def test_vcl_grid_hooks_are_narrow_and_balanced(self):
        root = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        block = root.split('StrEqI(cls, L"TShownValueList")', 1)[1].split('// Точка входа', 1)[0]
        for grid_cls in ('L"TStringGrid"', 'L"TDrawGrid"', 'L"TValueListEditor"'):
            self.assertIn(grid_cls, block + root.split('StrEqI(cls, L"TShownValueList")', 1)[0][-400:])
        for name in ('OHK_DELPHI_GRID_TEXT', 'OHK_DELPHI_GRID_FILL', 'OHK_DELPHI_GRID_THEME'):
            self.assertIn('PushPaint(instMod, ' + name, block)
            self.assertNotIn('PushDc(instMod, ' + name, block)
        # Стирание фона VCL-таблицы идёт PatBlt-ом.
        self.assertIn('PushPaint(instMod, OHK_PATBLT, cache)', block)
        gdi = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        grid = gdi.split('static bool IsDelphiGridClass', 1)[1].split('int WINAPI Root_MyDrawTextW(', 1)[0]
        for guard in ('WindowFromDC(dc)', 'GetPaintSessionHostHwnd()', 'IsForeignPaintBarrierActive()',
                      'L"TShownValueList"', 'L"TStringGrid"', 'L"TDrawGrid"',
                      'TA_UPDATECP', 'ETO_GLYPH_INDEX', 'ETO_PDY',
                      'lb.lbStyle == BS_SOLID',
                      'ApplyDelphiGridCellBackground(dc, rect, GetBkColor(dc))', 'L"Header"',
                      'ExtTextOutWithAlpha', 'return orig('):
            self.assertIn(guard, grid)
        self.assertIn('options == ETO_CLIPPED && rect && rect->top == 0', grid)
        self.assertIn('HeaderTextClear', grid)
        self.assertIn('[GlassCore][DelphiMemo] DirectGetDC', read("GlassCore/src/Hooks/EditHooks.cpp"))

    def test_vcl_combo_owner_draw_text_uses_only_supplied_dc(self):
        draw = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        begin = draw.split('bool BeginDraw(HWND parent', 1)[1].split('bool TryCheckListThemeBackground', 1)[0]
        for guard in ('ODT_COMBOBOX', 'L"TColorBox"', 'L"TDriveComboBox"',
                      'L"TFilterComboBox"', 'GlassControlKind::ComboBox',
                      'GetClientRect(dis->hwndItem, &client)', 's.bounds = client',
                      'ODS_COMBOBOXEDIT', 's.combo = combo'):
            self.assertIn(guard, begin)
        # TComboBox keeps its working field/Edit path and never enters this branch.
        self.assertNotIn('L"TComboBox"', begin)
        ext = draw.split('BOOL WINAPI MyExtText(HDC dc', 1)[1].split('int WINAPI MyDrawText(', 1)[0]
        for guard in ('IntersectClipRect(dc, s.bounds.left', 'ETO_OPAQUE',
                      'PatBlt(dc, rect->left', 'ExtTextOutWithAlpha(', 'if (!count) return TRUE;'):
            self.assertIn(guard, ext)
        drawtext = draw.split('int WINAPI MyDrawText(', 1)[1].split('void Startup()', 1)[0]
        self.assertIn('IntersectClipRect(dc, s.bounds.left', drawtext)
        fill = draw.split('int WINAPI MyFill(HDC dc', 1)[1].split('BOOL WINAPI MyExtText(', 1)[0]
        for guard in ('if (s.combo)', 'ComboFillBounds(s, *r)', 'SystemBackground(lb.lbColor)',
                      'return orig(dc, r, brush);'):
            self.assertIn(guard, fill)
        root = read('WndCompat/Root/RootSubclass.cpp')
        for guard in ('comboOwnerDraw', 'L"TColorBox"', 'L"TDriveComboBox"',
                      'L"TFilterComboBox"', 'listOwnerDraw && begin && end && begin(hWnd, dis)',
                      'if (listOwnerDraw || comboOwnerDraw)'):
            self.assertIn(guard, root)
        combo = read('GlassCore/src/Hooks/ComboBoxControlHooks.cpp')
        for marker in ('BeginPaint session skip', 'active-session', 'caller-not-comctl32',
                       'comctl-module-unresolved', 'g_activePaintMod',
                       'DTB(вне сессии)', 'wndClass=%s',
                       'IsNativeComboBoxClass', 'StrEq(cls, L"ComboBox")',
                       'appPaintedNative', 'L"app-iat"', 'source=%s',
                       'OnHotBeginGlobal'):
            self.assertIn(marker, combo)
        self.assertEqual(combo.count('GlassCore::DelphiListBox::IsInsideDraw()'), 3)
        self.assertIn('#include <Hooks/delphi/TListBoxDraw.h>', combo)
        native = combo.split('BeginPaint session skip', 1)[1].split('static void OnEndPaintGlobal', 1)[0]
        self.assertIn('callerMod == entryPoint', native)
        self.assertNotIn('IsNativeComboBoxClass(hWnd) ||', native)
        self.assertIn('current->combo', draw)
        self.assertIn('opaque-stripped', draw)
        self.assertIn('options & ~static_cast<UINT>(ETO_OPAQUE)', draw)
        combo_text = draw.split('if (current->combo)', 1)[1].split('if ((GetTextAlign(dc)', 1)[0]
        self.assertIn('ExtTextOutWithAlpha(', combo_text)
        self.assertIn('if (!count) return TRUE;', combo_text)
        begin_draw = draw.split('bool BeginDraw(HWND parent', 1)[1].split('bool TryCheckListThemeBackground', 1)[0]
        for guard in ('const bool comboField', 'if (!comboField && !comboRow) return false;',
                      'if (s.combo && i != FillSlot && i != ExtTextSlot && i != DrawTextSlot',
                      '&& i != BitBltSlot && i != StretchBltSlot) continue;', 's.field = comboField'):
            self.assertIn(guard, begin_draw)
        self.assertIn('opaque-brush', draw)
        self.assertIn('CreateSolidColorBrushWithAlpha(lb.lbColor, 255)', draw)

    def test_color_combo_popup_rows_are_fill_only_and_never_negative_id_fields(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        begin = code.split('bool BeginDraw(HWND parent', 1)[1].split('bool TryCheckListThemeBackground', 1)[0]
        field = begin.split('const bool comboField', 1)[1].split('const bool comboRow', 1)[0]
        self.assertIn('ODS_COMBOBOXEDIT', field)
        self.assertNotIn('itemID', field)
        row = begin.split('const bool comboRow', 1)[1].split('if (list)', 1)[0]
        for guard in ('combo && !comboField', 'L"TColorBox"', 'dis->itemID != static_cast<UINT>(-1)',
                      'ODA_DRAWENTIRE | ODA_SELECT'):
            self.assertIn(guard, row)
        for other in ('TComboBox', 'TDriveComboBox', 'TFilterComboBox'):
            self.assertNotIn(other, row)
        self.assertIn('if (comboRow && i != FillSlot) continue;', begin)
        self.assertIn('if (!comboRow && !IsInsideDraw())', begin)
        current = code.split('const Session* Current(', 1)[1].split('bool SystemBackground(', 1)[0]
        self.assertIn('bool comboRowFill = false', current)
        self.assertIn('s.combo && !s.field && !comboRowFill', current)
        self.assertEqual(code.count('Current(dc, true)'), 1)
        query = code.split('bool IsInsideDraw() noexcept', 1)[1].split('bool BeginDraw(', 1)[0]
        self.assertIn('for (const auto& s : g_stack)', query)
        self.assertIn('if (!s.combo || s.field) return true;', query)

    def test_color_combo_row_fills_keep_clip_rgb_native_errors_and_finite_extents(self):
        code = read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp')
        fill = code.split('int WINAPI MyFill(HDC dc', 1)[1].split('BOOL WINAPI MyExtText(HDC dc', 1)[0]
        row = fill.split('if (s.combo && !s.field)', 1)[1].split('if (s.combo)', 1)[0]
        for guard in ('!s.selected && EqualRect(r, &s.item) && SystemBackground(lb.lbColor)',
                      'const int result = orig(dc, r, brush)', 'const DWORD error = GetLastError()',
                      'const BOOL cleared = result && width > 0 && width <= INT_MAX',
                      'height > 0 && height <= INT_MAX', 'static_cast<long long>(r->right) - r->left',
                      'static_cast<long long>(r->bottom) - r->top', 'SetLastError(entryError)',
                      'CreateSolidColorBrushWithAlpha(lb.lbColor, 255)', 'opaque.reset()', 'SetLastError(error)'):
            self.assertIn(guard, row)
        for forbidden in ('GetClientRect', 'SaveDC', 'SelectClipRgn', 'IntersectClipRect', 'GetClipBox',
                          'ComboFillBounds', 'ExtTextOutWithAlpha', 'BitBlt', 'BackgroundAlpha', '0xFFFFFF'):
            self.assertNotIn(forbidden, row)
        self.assertIn('EqualRect(&clipped, r)', fill.split('if (s.combo && !s.field)', 1)[0])

    def test_tcombo_list_ctlcolor_is_native_first_own_attached_popup_only(self):
        code = read('WndCompat/Popup/ComboLBox/ComboBoxSubclass.cpp')
        case = code.split('case WM_CTLCOLORLISTBOX:', 1)[1].split('case WM_LBUTTONDOWN:', 1)[0]
        for guard in ('Shared::InjectionMode::Full', 'IsWindowClass(hWnd, L"TComboBox")',
                      'GlassIsBackdropAllowedForHwnd(hWnd)', 'GlassIsBackdropAllowedForHwnd(list)',
                      'IsWindowClass(list, L"ComboLBox")', 'ComboLBox_IsAttached(list)',
                      'GetComboBoxInfo(hWnd, &info)', 'info.hwndCombo == hWnd && info.hwndList == list',
                      'const LRESULT original = native()', 'const bool eligible = isOwnList()',
                      'eligible && solid && bk != CLR_INVALID', 'lb.lbStyle == BS_SOLID',
                      '(lb.lbColor & 0xFFFFFFu) == windowColor && (bk & 0xFFFFFFu) == windowColor',
                      'if (black && SetBkColor(dc, 0) != CLR_INVALID)', 'SetLastError(originalError)'):
            self.assertIn(guard, case)
        self.assertEqual(case.count('DefSubclassProc('), 1)
        self.assertLess(case.index('const LRESULT original = native()'), case.index('const COLORREF bk = GetBkColor(dc)'))
        self.assertLess(case.index('const LRESULT original = native()'), case.index('const bool eligible = isOwnList()'))
        for forbidden in ('SetTextColor', 'SetBkMode', 'PatBlt', 'FillRect', 'SelectClipRgn', 'DrawText'):
            self.assertNotIn(forbidden, case)

    def test_tcombo_list_debug_probe_is_local_and_bounded_without_global_hooks(self):
        code = read('WndCompat/Popup/ComboLBox/ComboBoxSubclass.cpp')
        case = code.split('case WM_CTLCOLORLISTBOX:', 1)[1].split('case WM_LBUTTONDOWN:', 1)[0]
        for marker in ('#if defined(_DEBUG)', 'now - epoch >= 1000', 'budget = 16', '--budget',
                       '[WndCompat][DelphiComboList] CTLCOLOR', 'list=0x%p dc=0x%p',
                       'bk=0x%08X brush=0x%p', 'eligible=%d replaced=%d'):
            self.assertIn(marker, case)
        self.assertNotIn('GLASS_DELPHI_DRAW_PROBE', case)
        for forbidden in ('DebugDetourHooks', 'SetEnabled', 'RegisterPaintProvider', 'RegisterCompatDCProvider'):
            self.assertNotIn(forbidden, case)
        harness = read('WndCompat/tests/combo_list_ctlcolor_test.py')
        self.assertIn('#include "WndCompat/Popup/ComboLBox/ComboBoxSubclass.cpp"', harness)
        self.assertIn('DebugProbe', harness)

    def test_vcl_combo_classification_and_combo_alpha_paths(self):
        classifier = read('WndCompat/Tracker/WindowClassifier.cpp')
        subclass = read('WndCompat/Popup/ComboLBox/ComboBoxSubclass.cpp')
        for cls in ('TComboBox', 'TColorBox', 'TDriveComboBox', 'TFilterComboBox'):
            self.assertRegex(classifier, r'(?s)if\s*\([^;{}]*L"' + cls +
                             r'"[^;{}]*\)\s*return ControlKind::ComboBox\s*;')
            self.assertIn('L"' + cls + '"', subclass)
        code = read('GlassCore/src/Hooks/ComboBoxControlHooks.cpp')
        draw = code.split('static HRESULT InvokeComboDrawThemeBackground(', 1)[1].split('int WINAPI Overall_MyDrawTextW', 1)[0]
        self.assertIn('GetEffectiveDrawThemeConstantAlphaForSession', draw)
        self.assertIn('DrawThemeBackgroundWithConstantAlpha(', draw)
        self.assertNotIn('if (dropPart)', draw)
        self.assertIn('if (!dropPart && g_comboDtbDedupField', draw)
        self.assertIn('if (!dropPart) g_comboDtbDedupField = dedupKey', draw)
        self.assertLess(draw.index('if (alphaOk)'), draw.index('g_comboDtbDedupField = dedupKey'))
        helper = read('GlassShared/src/WinApiUtils.cpp').split('bool DrawThemeBackgroundWithConstantAlpha(', 1)[1].split('bool DrawThemeBackgroundWithConstantAlphaAndChromaKey(', 1)[0]
        for guard in ('if (!IntersectRect(&blendRect, pRect, pClipRect))',
                      'blendRect.left - pRect->left', 'blendRect.right - blendRect.left',
                      'blendRect.bottom - blendRect.top', 'GdiAlphaBlend('):
            self.assertIn(guard, helper)
        self.assertIn('[WndCompat][DelphiCombo] WM_DRAWITEM', read('WndCompat/Root/RootSubclass.cpp'))

    def test_targeted_vcl_edit_probes_and_combo_ctlcolor(self):
        combo = read("WndCompat/Popup/ComboLBox/ComboBoxSubclass.cpp")
        ctl = combo.split('case WM_CTLCOLOREDIT:', 1)[1].split('case WM_CTLCOLORLISTBOX:', 1)[0]
        for guard in ('Shared::InjectionMode::Full', 'GlassIsBackdropAllowedForHwnd(hWnd)',
                      'IsDelphiComboBox(hWnd)', 'info.hwndItem != edit', 'L"Edit"',
                      'DefSubclassProc', 'lb.lbStyle == BS_SOLID', 'GetSysColor(COLOR_WINDOW)',
                      'SetBkColor(dc, 0)', 'BLACK_BRUSH', '#if defined(_DEBUG)'):
            self.assertIn(guard, ctl)
        self.assertIn('return result;', ctl)
        self.assertIn('InvalidateRect(info.hwndItem, nullptr, TRUE)', combo)
        paint = read("GlassCore/src/Subsystems/PaintDispatcher.cpp")
        for marker in ('LogVclEditDc(L"BeginPaint"', 'LogVclEditDc(L"EndPaint.before"',
                       'LogVclEditDc(L"GetDC"', 'LogVclEditDc(L"GetDCEx"',
                       'LogVclEditDc(L"ReleaseDC.before"', 'CreateCompatibleDC.context',
                       'DeleteDC.before.context', 'L"TMemo"', 'L"TComboBox"'):
            self.assertIn(marker, paint)
        edit = read("GlassCore/src/Hooks/EditHooks.cpp")
        for marker in ('DelphiMemo] TextGeometry', 'DelphiMemo] ApplyBackground api=PatBlt',
                       'DelphiMemo] ExtTextOutStack', 'DelphiComboEdit] TextResult'):
            self.assertIn(marker, edit)
        self.assertIn('DelphiMemo] BufferAPI api=EndBufferedPaint', read("GlassCore/src/Subsystems/RenderText.cpp"))
        self.assertNotIn('DebugDetourHooks::SetEnabled', edit)
        self.assertNotIn('DebugDetourHooks::SetEnabled', paint)

    def test_vcl_background_waits_for_proven_client_or_drawitem_dc(self):
        code = read('GlassCore/src/Hooks/delphi/TBackgroundHooks.cpp')
        for marker in ('IsFullInjectionMode()', 'GlassIsBackdropAllowedForHwnd',
                       'GlassControlKind::ListBox', 'GlassControlKind::Edit',
                       'HasDelphiInputDefaultBackground', 'IsInsideDraw()',
                       'EqualRect(r, &client)', 'GetMapMode(dc) == MM_TEXT',
                       'RegisterPaintProvider', 'RegisterPostPaintProvider', 'RegisterGetDCProvider',
                       's.paintDc == dc', 'IsCurrentPaintDcForHwnd',
                       's.observedDc != dc', 'observedBitmap', 'LB_GETTOPINDEX',
                       'LB_GETCOUNT', 'LB_GETITEMRECT', 'dis->itemID != static_cast<UINT>(top)',
                       'dis->hDC', 'serial == s.serial', 'if (s.armed)',
                       'g_dispatcher.DisableAllHooks()', 'BLACKNESS'):
            self.assertIn(marker, code)
        # Inside a session TSpinEdit belongs to the edit hooks alone. Its erase outside the sessions is the
        # rule of every VCL edit (see the erase-outside contract below).
        session_target = code.split('bool IsTarget(HWND hwnd, bool& isList) noexcept', 1)[1].split(
            'struct EraseClass', 1)[0]
        self.assertNotIn('TSpinEdit', session_target)
        self.assertEqual(code.count('TSpinEdit'), 1)
        self.assertIn('ClearObservedListTail(dis->hwndItem, dis)', read('GlassCore/src/Hooks/delphi/TListBoxDraw.cpp'))
        engine = read('GlassCore/src/Core/Engine.cpp')
        self.assertIn('DelphiBackground::Startup()', engine)
        self.assertIn('DelphiBackground::Shutdown()', engine)
        for file in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            self.assertIn('TBackgroundHooks.cpp', read(file))
            self.assertIn('TBackgroundHooks.h', read(file))

    def test_tab_brush_is_not_restored_to_another_class(self):
        source = read("WndCompat/Controls/TabControlSubclass.cpp")
        self.assertIn('_wcsicmp(className, L"TPageControl") != 0', source)
        self.assertGreaterEqual(source.count('_wcsicmp(g_brushClass, className) == 0'), 2)

    def test_delphi_calendar_header_linklabel_use_native_kinds(self):
        src = read("WndCompat/Tracker/WindowClassifier.cpp")
        def kind_for(cls_name, kind):
            marker = 'Equals(cls, L"' + cls_name + '")'
            self.assertIn(marker, src, cls_name)
            tail = src[src.index(marker):]
            stmt = tail[:tail.index(';')]
            self.assertIn('ControlKind::' + kind, stmt, f"{cls_name} -> {kind}")
        kind_for("TMonthCalendar", "MonthCal")
        kind_for("THeaderControl", "Header")
        kind_for("TLinkLabel", "Static")
        # Нативные аналоги остались в тех же ветках.
        kind_for("SysMonthCal32", "MonthCal")
        kind_for("SysHeader32", "Header")
        kind_for("SysLink", "Static")

    def test_delphi_grid_background_uses_low_alpha_for_non_window_colors(self):
        wc = read("WndCompat/Tracker/WindowClassifier.cpp")
        edit_block = wc.split('Equals(cls, L"TInplaceEditList")', 1)[0][-200:]
        self.assertIn('Equals(cls, L"TInplaceEdit")', edit_block)
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        brush = root.split('H BRUSH WINAPI Root_MyCreateSolidBrush'.replace('H B', 'HB'), 1)[1]
        brush = brush.split('// BWM: COMDLG32', 1)[0]
        for marker in ('IsDelphiGridClass(hostCls)',
                       'plainGrid == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)',
                       'return o(RGB(0, 0, 0))',
                       'kDelphiGridCellAlpha', 'return o(color)'):
            self.assertIn(marker, brush)
        # COLOR_WINDOW → 0; альфа для цветных ячеек делается на заливке
        # (pattern-кисть через FillRect альфу не переносит).
        self.assertIn('constexpr BYTE kDelphiGridCellAlpha = 100;', root)
        self.assertNotIn('CreateSolidColorBrushWithAlpha', brush)
        syscolor = root.split('HBRUSH WINAPI Root_MyGetSysColorBrush(int nIndex)', 1)[1].split('HBRUSH WINAPI Root_MyCreateSolidBrush', 1)[0]
        for marker in ('IsDelphiGridClass(hostCls)', 'nIndex == COLOR_WINDOW'):
            self.assertIn(marker, syscolor)
        self.assertNotIn('CreateSolidColorBrushWithAlpha', syscolor)
        # Очистка чёрным, затем AlphaBlend цветной подложки.
        for marker in ('static bool FillRectWithAlphaBlend(',
                       'GdiAlphaBlend(dc, rect->left, rect->top, w, h, mem, 0, 0, 1, 1, bf)',
                       'AC_SRC_ALPHA',
                       'static bool ApplyDelphiGridCellBackground(',
                       'rect->bottom - rect->top, BLACKNESS)',
                       'return FillRectWithAlphaBlend(dc, rect, color, kDelphiGridCellAlpha)',
                       'ApplyDelphiGridCellBackground(dc, rect, fillColor)',
                       'ApplyDelphiGridCellBackground(dc, rect, GetBkColor(dc))',
                       'AlphaFill hwnd=0x%p'):
            self.assertIn(marker, root)
        fill = root.split('int WINAPI Root_MyDelphiGridFill(HDC dc', 1)[1].split('HRESULT WINAPI Root_MyDelphiGridTheme', 1)[0]
        for marker in ('static bool IsDelphiGridFillDC(', 'IsDelphiGridFillDC(dc)',
                       '(rop == PATCOPY || rop == WHITENESS) && IsDelphiGridFillDC(hdc)'):
            self.assertIn(marker, root)
        for marker in ('[GlassCore][DelphiGrid] Fill', 'color=0x%08X', 'brush=%d'):
            self.assertIn(marker, fill)
        self.assertIn('count=%u rect=[', root)

    def test_delphi_grid_erase_is_blackened_in_subclass(self):
        root = read("WndCompat/Root/RootSubclass.cpp")
        erase = root.split('case WM_ERASEBKGND:', 1)[1].split('case WM_CTLCOLORSTATIC:', 1)[0]
        for marker in ('WndCompat::IsWindowClass(hWnd, L"TStringGrid")',
                       'WndCompat::IsWindowClass(hWnd, L"TDrawGrid")',
                       'WndCompat::IsWindowClass(hWnd, L"TShownValueList")',
                       'GetClipBox(hdc, &rc)', 'PatBlt(hdc, rc.left, rc.top', 'return 1;'):
            self.assertIn(marker, erase)
        # Черная очистка должна идти ДО проверки backdrop.
        self.assertLess(erase.index('L"TStringGrid"'),
                        erase.index('GlassIsBackdropAllowedForHwnd'))

    def test_delphi_grid_diagnostics_cover_surface_and_bitblt(self):
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        self.assertIn('static bool IsDelphiGridFillDC(', root)
        self.assertIn('[GlassCore][DelphiGrid] BitBlt hwnd=0x%p dst=(%d,%d) %dx%d', root)
        win = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        grid = win.split('L"TShownValueList"', 1)[1].split('// Точка входа процесса', 1)[0]
        self.assertIn('PushPaint(instMod, OHK_BITBLT, cache);', grid)
        self.assertIn('bmp=0x%p px=0x%08X', win)

    def test_delphi_grid_replays_fixed_area_text(self):
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        self.assertIn('struct GridTextRec', root)
        self.assertIn('struct GridPaintRec', root)
        replay = root.split('static void ReplayGridFixedArea(const GridPaintRec& rec)', 1)[1].split(
            'static void GridPaintBegin', 1)[0]
        for marker in ('rec.haveDataOrigin', 'rowStrip', 'colStrip', 'BLACKNESS',
                       'SetTextColor(dc, t.color)', 'ExtTextOutW(dc, t.x, t.y'):
            self.assertIn(marker, replay)
        self.assertIn('RegisterDelphiGridPaintProvider', root)
        header = read("GlassCore/Include/Hooks/RootGdiHooks.h")
        self.assertIn('void RegisterDelphiGridPaintProvider() noexcept;', header)
        win = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        self.assertIn('GlassCore::Root::RegisterDelphiGridPaintProvider();', win)

    def test_value_list_editor_is_handled_as_delphi_grid(self):
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        self.assertIn('|| StrEqI(cls, L"TValueListEditor")', root)
        self.assertIn('|| StrEqI(cls, L"TControlList");', root)
        # Тема Toolbar (фон ячейки значения у TValueListEditor) тоже гасится.
        theme = root.split('HRESULT WINAPI Root_MyDelphiGridTheme(HTHEME theme', 1)[1].split(
            'DWORD WINAPI Root_MyGetSysColor', 1)[0]
        self.assertIn('toolbarPart', theme)
        self.assertIn('StrEqI(cls, L"Toolbar")', theme)
        win = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        for marker in ('StrEqI(cls, L"TValueListEditor")', 'StrEqI(cn, L"TValueListEditor")'):
            self.assertIn(marker, win)
        sub = read("WndCompat/Root/RootSubclass.cpp")
        self.assertIn('WndCompat::IsWindowClass(hWnd, L"TValueListEditor")', sub)

    def test_control_list_is_handled_as_delphi_grid(self):
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        self.assertIn('|| StrEqI(cls, L"TControlList");', root)
        win = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        for marker in ('StrEqI(cls, L"TControlList")', 'StrEqI(cn, L"TControlList")'):
            self.assertIn(marker, win)
        sub = read("WndCompat/Root/RootSubclass.cpp")
        self.assertIn('WndCompat::IsWindowClass(hWnd, L"TControlList")', sub)

    def test_file_and_directory_list_boxes_use_owner_draw_session(self):
        cls = read("WndCompat/Tracker/WindowClassifier.cpp")
        block = cls.split('// ── ListBox ─', 1)[1].split('return ControlKind::ListBox;', 1)[0]
        for name in ("TFileListBox", "TDirectoryListBox"):
            self.assertIn('Equals(cls, L"%s")' % name, block)
        sub = read("WndCompat/Root/RootSubclass.cpp")
        for name in ("TFileListBox", "TDirectoryListBox"):
            self.assertIn('IsWindowClass(dis->hwndItem, L"%s")' % name, sub)
        draw = read("GlassCore/src/Hooks/delphi/TListBoxDraw.cpp")
        for name in ("TFileListBox", "TDirectoryListBox"):
            self.assertIn('_wcsicmp(cls, L"%s")' % name, draw)
        back = read("GlassCore/src/Hooks/delphi/TBackgroundHooks.cpp")
        for name in ("TFileListBox", "TDirectoryListBox"):
            self.assertIn('_wcsicmp(cls, L"%s")' % name, back)
        probe = read("GlassCore/src/Hooks/delphi/TListBoxProbe.cpp")
        for name in ("TFileListBox", "TDirectoryListBox"):
            self.assertIn('_wcsicmp(cls, L"%s")' % name, probe)

    def test_tcoolbar_uses_its_dedicated_glass_path(self):
        cls = read("WndCompat/Tracker/WindowClassifier.cpp")
        rebar_block = cls.split('// ── ReBar ─', 1)[1].split('return ControlKind::ReBar;', 1)[0]
        self.assertIn('Equals(cls, L"ReBarWindow32")', rebar_block)
        self.assertNotIn('CoolBar', rebar_block)

        # CoolBar — отдельный вид контрола и отдельный путь отрисовки.
        coolbar_block = cls.split('// ── CoolBar (Delphi TCoolBar) ─', 1)[1].split('// ── ToolBar', 1)[0]
        self.assertIn('WndCompat::CoolBar_IsClassName(cls.c_str())', coolbar_block)
        self.assertIn('return ControlKind::CoolBar;', coolbar_block)

        handler = read("WndCompat/Tracker/WinEventHandler.cpp")
        self.assertIn('case ControlKind::CoolBar:', handler)
        self.assertIn('WndCompat::CoolBar_Attach(hWnd);', handler)
        self.assertIn('shellRebar || topLevelHostRebar', handler)

        # Rebar-ветка не должна обрастать особенностями TCoolBar.
        rebar = read("GlassCore/src/Hooks/RebarHooks.cpp")
        self.assertNotIn('CoolBar', rebar)

    def test_tcoolbar_subclass_filename_matches_its_message_scope(self):
        source = read('WndCompat/Controls/TCoolBarSubclass.cpp')
        self.assertTrue((ROOT / 'WndCompat/Controls/TCoolBarSubclass.h').is_file())
        for extension in ('cpp', 'h'):
            self.assertFalse((ROOT / ('WndCompat/Controls/CoolBarSubclass.' + extension)).exists())
            for project in ('WndCompat/WndCompat.vcxproj', 'WndCompat/WndCompat.vcxproj.filters'):
                contents = read(project)
                self.assertIn('Controls\\TCoolBarSubclass.' + extension, contents)
                self.assertNotIn('Controls\\CoolBarSubclass.' + extension, contents)
        self.assertIn('SetWindowSubclass(hWnd, TCoolBarSubclassProc, 1, 0)', source)
        self.assertIn('RemoveWindowSubclass(hWnd, TCoolBarSubclassProc, 1)', source)
        self.assertIn('#include <Controls/TCoolBarSubclass.h>', source)
        for path in ('WndCompat/Tracker/WindowClassifier.cpp', 'WndCompat/Tracker/WinEventHandler.cpp',
                     'WndCompat/Tracker/BackdropPolicyRescan.cpp', 'WndCompat/Root/RootSubclass.cpp',
                     'WndCompat/Root/Win32ChromeRestore.cpp'):
            self.assertIn('#include <Controls/TCoolBarSubclass.h>', read(path))

    def test_coolbar_raw_class_gate_is_shared_by_classifier_attach_and_rescan(self):
        # The probe reports GetClassNameW == TCoolBar. It is not the caption
        # or the theme class Rebar, and NormalizeWindowClass keeps the T.
        code = read('WndCompat/Controls/TCoolBarSubclass.cpp')
        predicate = code.split('bool CoolBar_IsClassName(const wchar_t* className) noexcept', 1)[1].split(
            'void CoolBar_Attach(HWND hWnd)', 1)[0]
        self.assertIn('_wcsicmp(className, L"TCoolBar") == 0', predicate)
        self.assertIn('_wcsicmp(className, L"CoolBar") == 0', predicate)
        for forbidden in ('wcsstr', 'wcsncmp', 'ContainsSubstr', 'ReBarWindow32', 'SizeableRebar'):
            self.assertNotIn(forbidden, predicate)
        window_gate = code.split('bool IsCoolBarClass(HWND hWnd) noexcept', 1)[1].split(
            'LRESULT CALLBACK TCoolBarSubclassProc', 1)[0]
        self.assertIn('GetClassNameW', window_gate)
        self.assertIn('WndCompat::CoolBar_IsClassName(cls)', window_gate)
        self.assertIn('if (!IsCoolBarClass(hWnd))', code)
        self.assertIn('CoolBar_IsClassName(cls.c_str())', read('WndCompat/Tracker/WindowClassifier.cpp'))
        rescan = read('WndCompat/Tracker/BackdropPolicyRescan.cpp').split(
            'void TryRescanControlHwnd(HWND hChild) noexcept', 1)[1]
        self.assertLess(rescan.index('WndCompat::CoolBar_IsClassName(cls)'),
                        rescan.index('GlassGetWindowKind(hChild) != GK::None'))
        normalize = read('WndCompat/Tracker/WindowClassUtils.cpp').split(
            'std::wstring NormalizeWindowClass(HWND hWnd)', 1)[1].split('bool IsWindowClass', 1)[0]
        self.assertIn('return cls;', normalize)
        self.assertNotIn('TCoolBar', normalize)

    def test_coolbar_dedicated_path(self):
        # WndCompat: свой сабкласс и провайдер.
        subclass = read("WndCompat/Controls/TCoolBarSubclass.cpp")
        for marker in ('void CoolBar_Attach(HWND hWnd)',
                       'void CoolBar_Detach(HWND hWnd)',
                       'void CoolBar_DetachAll()',
                       'void CoolBar_CatchUp(HWND hRoot) noexcept',
                       'GlassMarkWindow(hWnd, GlassShared::GlassControlKind::CoolBar)',
                       'WndCompat::WM_GLASS_DETACH_COOLBAR',
                       '[WndCompat][CoolBar] CoolBar_Attach: субкласс установлен'):
            self.assertIn(marker, subclass)

        provider = read("WndCompat/Providers/CoolBarEventProvider.cpp")
        self.assertIn('SetCoolBarEventProvider', provider)

        # include'ы WinEventHook_GetMode/_SignalDetachComplete и InjectionMode
        # (без них WndCompat не собирается: C3861/C2677).
        self.assertIn('#include <Tracker/WinEventHandler.h>', subclass)
        self.assertIn('#include <InjectionMode.h>', subclass)

        # Delphi-контрол: код лежит в Hooks/delphi, как TListBoxDraw.
        self.assertTrue(os.path.exists("GlassCore/src/Hooks/delphi/TCoolBarDraw.cpp"))
        self.assertTrue(os.path.exists("GlassCore/Include/Hooks/delphi/TCoolBarDraw.h"))
        self.assertIn('#include <Hooks/delphi/TCoolBarDraw.h>',
                      read("GlassCore/src/Core/Engine.cpp"))

        # GlassCore: своя сессия и свои слоты.
        hooks = read("GlassCore/src/Hooks/delphi/TCoolBarDraw.cpp")
        for marker in ('ExtTextOutSlot', 'FillRectSlot', 'PatBltSlot', 'DrawTextSlot',
                       'DrawTextExSlot', 'DrawThemeTextSlot', 'bool IsPaintDc(HDC hdc) noexcept'):
            self.assertIn(marker, hooks)
        self.assertIn('return owner == sessionHwnd;', hooks)
        self.assertIn('session.suppliedDc && hdc == session.suppliedDc', hooks)
        self.assertIn('IsTopWindowDrawingSession(sessionHwnd)', hooks)
        self.assertIn('IsPaintStackDcForHwnd(sessionHwnd, hdc)', hooks)
        self.assertIn('ETO_OPAQUE', hooks)
        self.assertIn('GlassCore::Text::ExtTextOutWithAlpha', hooks)
        self.assertIn('GlassCore::Text::DrawTextWithAlpha', hooks)
        self.assertIn('[GlassCore][CoolBar] OnPaintBegin', hooks)
        self.assertIn('Diag(L"band-fill", hdc, rc);', hooks)
        self.assertNotIn('"DrawThemeBackground"', hooks)

        # Прокладка провайдера.
        exports = read("GlassCore/src/Core/Exports.cpp")
        self.assertIn('out->coolbar.OnPaintBegin = &GlassCore::CoolBarControl::OnPaintBegin;', exports)
        engine = read("GlassCore/src/Core/Engine.cpp")
        self.assertIn('GlassCore::CoolBarControl::Startup();', engine)
        self.assertIn('GlassCore::CoolBarControl::Shutdown();', engine)
        wnd = read("WndCompat/WndCompat.cpp")
        self.assertIn('WndCompat::CoolBar::SetCoolBarEventProvider(providers.coolbar);', wnd)
        kind = read("GlassShared/include/GlassControlKind.h")
        self.assertIn('CoolBar                   = 39', kind)

    def test_coolbar_covers_native_text_routes_and_preserves_dc_state(self):
        hooks = read('GlassCore/src/Hooks/delphi/TCoolBarDraw.cpp')
        for api in ('DrawTextW', 'DrawTextExW', 'DrawThemeText'):
            self.assertIn('"' + api + '", reinterpret_cast<PVOID>(My', hooks)
            self.assertIn('L"' + api + '"', hooks)
        self.assertIn('Dependencies<ComctlNewModule>()', hooks)
        self.assertIn('Dependencies<ComctlLegacyModule>()', hooks)
        self.assertIn('Dependencies<EntryPointModule>()', hooks)
        self.assertIn('Dependencies<UxThemeModule>()', hooks)
        self.assertNotIn('CallerModule(_ReturnAddress())', hooks)
        self.assertIn('held.acquired[i] = dispatcher.GetRef(i) > before', hooks)
        self.assertIn('if (held.acquired[i])', hooks)
        self.assertIn('ReleaseModule(session.modules[m - 1])', hooks)
        self.assertNotIn('if (!comctl)', hooks)
        self.assertIn('SetBkMode(hdc, TRANSPARENT)', hooks)
        self.assertIn('RestoreDC(hdc, saved)', hooks)
        self.assertIn('DT_CALCRECT | DT_MODIFYSTRING', hooks)
        self.assertIn('GetThemeFont(theme, hdc, part, state, TMT_FONT', hooks)
        self.assertIn('GetThemeColor(theme, part, state, TMT_TEXTCOLOR', hooks)
        self.assertIn('text, cch, dx, orig)', hooks)
        self.assertNotIn('GetProcAddress', hooks)
        self.assertIn('chrome-fill-preserved', hooks)
        self.assertIn('IsChromeStripe(&rc)', hooks)
        self.assertIn('!IsBackgroundBrush(static_cast<HBRUSH>(GetCurrentObject(hdc, OBJ_BRUSH)))', hooks)
        for diag in ('hooks module=', 'text api=', 'not-session-dc', 'alpha-failed', 'stackOwner='):
            self.assertIn(diag, hooks)

    def test_coolbar_printclient_is_native_paint_with_a_balanced_session(self):
        subclass = read('WndCompat/Controls/TCoolBarSubclass.cpp')
        paint = subclass.split('case WM_PAINT:', 1)[1].split('case WM_ERASEBKGND:', 1)[0]
        self.assertIn('case WM_PRINTCLIENT:', paint)
        self.assertIn('p->OnPaintBegin(hWnd, reinterpret_cast<HDC>(wParam))', paint)
        self.assertIn('if (began) end()', paint)
        self.assertIn('wil::scope_exit', paint)
        self.assertIn('return DefSubclassProc(hWnd, uMsg, wParam, lParam)', paint)
        self.assertNotIn('PatBlt(', paint)
        self.assertNotIn('case WM_PRINTCLIENT:', subclass.split('case WM_ERASEBKGND:', 1)[1])
        provider = read('GlassShared/include/Providers/CoolBarEventProvider.h')
        self.assertIn('bool (*OnPaintBegin)(HWND hWnd, HDC suppliedDc)', provider)

    def test_list_item_icons_are_alpha_composited(self):
        draw = read("GlassCore/src/Hooks/delphi/TListBoxDraw.cpp")
        # Слоты BitBlt/StretchBlt в сессии owner-draw.
        for marker in ('"BitBlt", reinterpret_cast<void*>(MyBitBlt)',
                       '"StretchBlt", reinterpret_cast<void*>(MyStretchBlt)',
                       'BOOL WINAPI MyStretchBlt(HDC dc, int x, int y, int cx, int cy,'):
            self.assertIn(marker, draw)
        # Пара «цвет + маска» заменяется одним alpha-композитом, маска гасится.
        self.assertIn('rop == 0x00E20746 || rop == 0x00220326', draw)
        self.assertIn('g_stack.back().iconAlphaDone = true;', draw)
        self.assertIn('g_stack.back().iconAlphaDone = false;', draw)
        # Вырез фона: настройка Root\Control\ListBox|ComboBox либо ключ по углам.
        self.assertIn('QueryIconBackgroundColorRemovalContext(', draw)
        self.assertIn('{ L"Root", L"Control", L"ListBox" }', draw)
        self.assertIn('{ L"Root", L"Control", L"ComboBox" }', draw)
        self.assertIn('PrepareItemIcon(converted.get(), branch, iconBg)', draw)
        # Фон значка вырезается заливкой от края: белые области внутри
        # (страница в папке) остаются непрозрачными.
        self.assertIn('CutIconBackground(bmp32, bg, 10)', draw)
        self.assertIn('bool CutIconBackground(HBITMAP bmp32, COLORREF bg, BYTE threshold)', draw)
        self.assertIn('RestoreGlyphAlpha(bmp32, bg, threshold);', draw)
        # Маска ImageList — только 1bpp: 4bpp/8bpp здесь цветной значок.
        self.assertIn('const bool maskSource = bm.bmBitsPixel == 1;', draw)
        self.assertNotIn('bm.bmBitsPixel < 8', draw)
        self.assertIn('icon-bypass', draw)
        self.assertIn('if (!PrepareItemIcon(converted.get(), branch, iconBg))', draw)
        self.assertIn('GdiAlphaBlend(dst, x, y, cx, cy, src, sx, sy, scx, scy, bf)', draw)

    def test_delphi_grid_replay_supports_row_only_fixed_area(self):
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        replay = root.split('static void ReplayGridFixedArea(const GridPaintRec& rec)', 1)[1].split(
            'static void GridPaintBegin', 1)[0]
        for marker in ('const bool haveRow', 'const bool haveCol',
                       'if (!haveRow && !haveCol)', 'if (haveRow)', 'if (haveCol)'):
            self.assertIn(marker, replay)

    def test_delphi_grid_remembers_data_origin_for_partial_paints(self):
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        self.assertIn('thread_local std::unordered_map<HWND, POINT> g_gridDataOrigins;', root)
        remember = root.split('static void RememberGridDataOrigin', 1)[1].split(
            'static GridPaintRec* FindGridPaint', 1)[0]
        for marker in ('try_emplace(wnd, POINT{ -1, -1 })', 'x < p.x', 'y < p.y',
                       'x < 0 || y < 0'):
            self.assertIn(marker, remember)
        replay = root.split('static void ReplayGridFixedArea(const GridPaintRec& rec)', 1)[1].split(
            'static void GridPaintBegin', 1)[0]
        self.assertIn('g_gridDataOrigins.find(rec.hwnd)', replay)
        fill = root.split('int WINAPI Root_MyDelphiGridFill(HDC dc', 1)[1].split(
            'HRESULT WINAPI Root_MyDelphiGridTheme(', 1)[0]
        self.assertIn('RememberGridDataOrigin(fillWnd, rect->left, rect->top)', fill)

    def test_delphi_grid_specs_enabled_on_beginpaint_caller(self):
        win = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        block = win.split('kGridSessionSpecs[] = {', 1)[1].split('g_rootPaintStack.push_back', 1)[0]
        for marker in ('OHK_DELPHI_GRID_FILL', 'OHK_GETSYSCOLOR', 'OHK_PATBLT',
                       'EnableRootHook(callerMod, idx, true)',
                       'session.specs.push_back(HookSpec{ static_cast<void*>(callerMod), idx })'):
            self.assertIn(marker, block)
        # Парность: спеки добавляются в сессию, которую снимает Root_OnEndPaint.
        self.assertIn('EnableSpecs(session.specs, false)', win)

    def test_delphi_grid_kills_window_color_for_client_fill(self):
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        body = root.split('DWORD WINAPI Root_MyGetSysColor(int nIndex)', 1)[1].split(
            'int WINAPI Root_MyDrawTextW(', 1)[0]
        for marker in ('nIndex == COLOR_WINDOW', 'IsDelphiGridPaintHost()', 'return 0;'):
            self.assertIn(marker, body)
        self.assertIn('bool IsDelphiGridPaintHost()', root)
        header = read("GlassCore/Include/Hooks/RootGdiHooks.h")
        self.assertIn('OHK_GETSYSCOLOR,', header)
        self.assertIn('"GetSysColor", reinterpret_cast<PVOID>(Root_MyGetSysColor) }', root)
        win = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        grid = win.split('L"TShownValueList"', 1)[1].split('// Точка входа процесса', 1)[0]
        self.assertIn('PushPaint(instMod, OHK_GETSYSCOLOR, cache);', grid)

    def test_delphi_grid_logs_fill_entry_and_general_fillrect(self):
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        self.assertIn('[GlassCore][DelphiGrid] FillEnter dc=0x%p dcWnd=0x%p', root)
        self.assertIn('orig=%d caller=%s', root)
        # Обработка не должна зависеть от наличия трамплина: без orig детур
        # раньше молча уходил в FillRect.
        fill = root.split('int WINAPI Root_MyDelphiGridFill(HDC dc', 1)[1].split(
            'HRESULT WINAPI Root_MyDelphiGridTheme(', 1)[0]
        self.assertLess(fill.index('const auto passthrough'), fill.index('IsDelphiGridFillDC(dc))'))
        self.assertIn('orig ? orig(dc, rect, brush) : ::FillRect(dc, rect, brush)', fill)
        self.assertIn('ApplyDelphiGridCellBackground(dc, rect, fillColor)', fill)
        self.assertIn('[GlassCore][DelphiGrid] GenFill hwnd=0x%p rect=[%ld,%ld,%ld,%ld]', root)
        # Общий FillRect не должен разыменовывать rect/brush до проверки.
        gen = root.split('[GlassCore][DelphiGrid] GenFill', 1)[0]
        self.assertIn('if (lprc && hbr && (StrEqI(cls, L"TStringGrid")', gen)

    def test_delphi_grid_logs_theme_and_any_patblt(self):
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        self.assertIn('[GlassCore][DelphiGrid] Theme hwnd=0x%p part=%d state=%d', root)
        self.assertIn('[GlassCore][DelphiGrid] PatBltAny hwnd=0x%p rop=0x%08X', root)

    def test_panel_classes_and_native_message_bridge_share_exact_gate(self):
        classes = read('GlassShared/include/GlassDelphiPanel.h')
        for name in ('TPanel', 'TFlowPanel', 'TGridPanel', 'TStackPanel', 'TRelativePanel',
                     'TCategoryPanel', 'TCategoryPanelGroup', 'TScrollBox',
                     'TFrame', 'TSampleFrame', 'TEmbeddedForm'):
            self.assertIn('_wcsicmp(name, L"' + name + '") == 0', classes)
        for forbidden in ('wcsstr', 'wcsncmp', 'find(', 'TCoolBar'):
            self.assertNotIn(forbidden, classes)
        self.assertIn('GlassShared::IsDelphiPanelClassName(cls.c_str())',
                      read('WndCompat/Tracker/WindowClassifier.cpp'))
        bridge = read('WndCompat/Root/DelphiPanelPaint.h')
        for guard in ('WM_PAINT', 'WM_ERASEBKGND', 'WM_PRINTCLIENT', 'IsShuttingDown()',
                      'InjectionMode::Full', 'GlassIsBackdropAllowedForHwnd',
                      'GlassShared::IsDelphiPanelWindow', '!begin || !end || !begin(',
                      'wil::scope_exit', 'DefSubclassProc(hwnd, message, wParam, lParam)'):
            self.assertIn(guard, bridge)
        root = read('WndCompat/Root/RootSubclass.cpp').split('LRESULT CALLBACK RootSubclassProcShared(', 1)[1]
        self.assertLess(root.index('WndCompat::TryHandleDelphiPanelPaint('), root.index('case WM_ERASEBKGND:'))

    def test_panel_background_uses_real_alpha_and_both_system_defaults_for_exact_tpanel(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        self.assertIn('kCustomBackgroundAlpha = 100', code)
        for api in ('"FillRect"', '"PatBlt"', '"ExtTextOutW"', 'GdiAlphaBlend',
                    'CreateDIBSection', 'AC_SRC_ALPHA', 'Blacken(dc, r)', 'GetObjectW', 'BS_SOLID'):
            self.assertIn(api, code)
        self.assertNotIn('CreateSolidColorBrushWithAlpha', code)
        policy = code.split('bool ApplyBackground(', 1)[1].split('int WINAPI PanelFill(', 1)[0]
        self.assertEqual(policy.count('GetSysColor('), 2)
        self.assertIn('GetSysColor(COLOR_WINDOW)', policy)
        self.assertIn('GetSysColor(COLOR_BTNFACE)', policy)
        self.assertNotIn('windowOnlyDefault', policy)
        self.assertNotIn('GetClassNameW', policy)
        self.assertIn('|| rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu)', policy)
        self.assertIn('pixel[3] = kCustomBackgroundAlpha', policy)
        cache = read('GlassCore/src/Hooks/RootWindowHooks.cpp')
        self.assertIn('if (!GlassShared::IsDelphiPanelClassName(cls)\n'
                      '                    && !GlassShared::IsDelphiButtonGroupsClassName(cls)\n'
                      '                    && !cache.bomeMemo)', cache)
        self.assertIn('&& !GlassShared::IsDelphiPanelWindow(hWnd)', cache)

    def test_panel_sessions_release_only_acquired_refs_and_isolate_dcs(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        for guard in ('dc == s.suppliedDc', 'owner == s.hwnd', 'IsTopWindowDrawingSession(s.hwnd)',
                      'IsPaintStackDcForHwnd(s.hwnd, dc)', 'if (g_inHook', 'g_dispatcher.GetRef(i) > before',
                      'if (s.acquired[i-1])', 'g_dispatcher.DisableAllHooks()', 'if (!mask) return false'):
            self.assertIn(guard, code)
        self.assertNotIn('_ReturnAddress', code)

    def test_panel_ansi_export_detour_is_private_exact_tpanel_and_paint_print_only(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        gate = code.split('bool IsAnsiTextPanel(', 1)[1].split('bool IsAnsiTextRect(', 1)[0]
        for marker in ('message == WM_PAINT || message == WM_PRINTCLIENT',
                       'GetClassNameW(hwnd, name', '_wcsicmp(name, L"TPanel") == 0'):
            self.assertIn(marker, gate)
        for forbidden in ('WM_ERASEBKGND', 'IsDelphiPanelClassName', 'wcsstr', 'TFrame', 'TPage'):
            self.assertNotIn(forbidden, gate)
        table = code.split('g_dep{{', 1)[1].split('}};', 1)[0]
        self.assertNotIn('DrawTextA', table)
        self.assertNotIn('TextSlot', code)
        self.assertIn('const bool panelText = IsAnsiTextPanel(hwnd, message)', code)
        self.assertIn('s.ansiTextAcquired = panelText && AcquireAnsiText()', code)
        self.assertIn('GetProcAddress(user32, "DrawTextA")', code)
        self.assertIn('Detours::Attach(reinterpret_cast<void**>(&g_actualDrawTextA)', code)
        self.assertIn('Detours::Detach(reinterpret_cast<void**>(&g_actualDrawTextA)', code)
        self.assertIn('g_dispatcher.moduleAddress = GlassCore::ModuleHandler::g_entryPointModule', code)
        handler = code.split('int DrawAnsiText(AnsiTextRoute route,', 1)[1].split(
            'int WINAPI PanelDrawTextA(HDC dc,', 1)[0]
        wrappers = code.split('int WINAPI PanelDrawTextA(HDC dc,', 1)[1].split(
            'HRESULT DrawContainerThemeTextEx(', 1)[0]
        self.assertIn('return DrawAnsiText(AnsiTextRoute::PanelExport, dc, text, count, rect, flags)', wrappers)
        self.assertIn('return DrawAnsiText(AnsiTextRoute::PageIat, dc, text, count, rect, flags)', wrappers)
        for marker in ('const auto original = pageIat ? PageAnsiTextOriginal() : AnsiTextOriginal()',
                       'if (!original && !pageIat) return 0', '!IsPanelDc(dc)',
                       '!g_stack.back().ansiTextPaint'):
            self.assertIn(marker, handler)
        for forbidden in ('IsWindowUnicode', 'GetACP(', 'DetourAttach', '_ReturnAddress'):
            self.assertNotIn(forbidden, code)
        self.assertNotIn('DrawTextA', read('GlassCore/Include/Hooks/delphi/TPanelDraw.h'))

    def test_panel_export_lease_lifetime_is_independent_of_exe_iat_and_native_uses_trampoline(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        for marker in ('size_t g_ansiTextRefs{}', 'bool g_ansiTextAttached = false',
                       'bool ansiTextAcquired = false', 'constexpr unsigned kAnsiTextMask = 1u << Count',
                       '!g_ansiTextRefs && !g_ansiTextAttached', 'if (FAILED(hr)) return false',
                       '++g_ansiTextRefs', 's.ansiTextAcquired && g_ansiTextRefs && --g_ansiTextRefs == 0',
                       'if (s.ansiTextAcquired) mask |= kAnsiTextMask', 'return g_actualDrawTextA',
                       'catch (...) { ReleaseSession(s); return false; }', 'text-resolve api=DrawTextA',
                       'text-detour api=DrawTextA enable=1', 'text-detour api=DrawTextA enable=0'):
            self.assertIn(marker, code)
        begin = code.split('bool BeginPaint(HWND hwnd,', 1)[1].split('void EndPaint()', 1)[0]
        self.assertNotIn('!g_dispatcher.moduleAddress', begin)
        self.assertLess(begin.index('AcquireAnsiText()'), begin.index('if (!mask) return false'))
        self.assertIn('g_dispatcher.moduleAddress && i < Count', begin)
        shutdown = code.split('void Shutdown() noexcept', 1)[1]
        self.assertIn('g_ansiTextRefs = 0', shutdown)
        self.assertIn('DetachAnsiText()', shutdown)
        self.assertNotIn('DebugDetourHooks', code)

    def test_actual_debug_detour_new_public_bits_preserve_old_slots_and_all_mask(self):
        header = read('GlassCore/Include/Core/Intercept/DebugDetourHooks.h')
        code = read('GlassCore/src/Core/Intercept/DebugDetourHooks.cpp')
        for name, slot in (('DrawTextA', 34), ('DrawTextExA', 35), ('ExtTextOutA', 36),
                           ('TextOutA', 37), ('TabbedTextOutA', 38), ('TabbedTextOutW', 39), ('TextOutW', 8)):
            self.assertIn(f'DDH_{name} = 1ull << {slot};', header)
            self.assertIn(f'decltype(&::{name}) g_true{name} = ::{name};', code)
            for switch in (code.split('void AttachSlot(size_t slot)', 1)[1].split('void DetachSlot', 1)[0],
                           code.split('void DetachSlot(size_t slot)', 1)[1].split('HRESULT WINAPI HookDrawThemeBackground', 1)[0]):
                case = switch.split(f'case {slot}:', 1)[1].split('break;', 1)[0]
                self.assertIn(f'&g_true{name}', case)
                self.assertIn(f'&Hook{name}', case)
        self.assertIn('constexpr size_t kSlotCount = 40;', code)
        all_mask = header.split('inline constexpr uint64_t DDH_All =', 1)[1].split(';', 1)[0]
        for name in ('DrawTextA', 'DrawTextExA', 'ExtTextOutA', 'TextOutA', 'TabbedTextOutA', 'TabbedTextOutW'):
            self.assertIn('DDH_' + name, all_mask)
        self.assertIn('const auto before = std::to_array(g_ref)', code)
        self.assertIn('if (FAILED(hr)) std::copy(before.begin(), before.end(), g_ref)', code)

    def test_actual_debug_text_metadata_never_reads_strings_or_arrays_and_stays_passthrough(self):
        code = read('GlassCore/src/Core/Intercept/DebugDetourHooks.cpp')
        new = code.split('thread_local bool g_inTextMetadata = false;', 1)[1].split(
            'int WINAPI HookDrawTextW(HDC hdc, LPCWSTR lpchText', 1)[0]
        for marker in ('GetOuterCallerModuleBaseName', 'FormatHdcWindowSuffix', 'GetTextColor(hdc)',
                       'GetBkColor(hdc)', 'GetBkMode(hdc)', 'SetLastError(error)',
                       'g_inTextMetadata || CallerModuleIsGlassCore(caller)', 'g_inTextMetadata = false',
                       'count=%lld flags=0x%X', 'tabCount=%d tabOrigin=%d', 'catch (...)'):
            self.assertIn(marker, new)
        for forbidden in ('FormatTextPreview', 'MultiByteToWideChar', 'strlen', 'wcslen', 'text[', 'rect->',
                          '*text', '*rect', '*params', '*dx', '*tabs', 'DrawTextWithAlpha', 'SetBkMode(',
                          'SetTextColor(', 'SetBkColor('):
            self.assertNotIn(forbidden, new)
        for name in ('DrawTextA', 'DrawTextExA', 'ExtTextOutA', 'TextOutA', 'TabbedTextOutA', 'TabbedTextOutW'):
            self.assertEqual(new.count('return g_true' + name + '('), 1)
            self.assertIn(f'LogTextMetadata(L"{name}", _ReturnAddress()', new)

    def test_actual_debug_detour_harness_compiles_whole_production_cpp_not_optional_iat_probe(self):
        test = read('WndCompat/tests/debug_detour_hooks_tests.cpp')
        self.assertIn('#include "../../GlassCore/src/Core/Intercept/DebugDetourHooks.cpp"', test)
        self.assertIn('#include <Core/Intercept/DebugDetourHooks.h>', test)
        self.assertNotIn('RootGdiHooks', test)
        for marker in ('FailedTransactions', 'MissingOriginal', 'NoTextOrArrayReads', 'Metadata',
                       'NativeMutations', 'ReentrantLogger', 'FailedLogger', 'ReleaseNoop'):
            self.assertIn(marker, test)
        runner = read('WndCompat/tests/debug_detour_hooks_test.py')
        for marker in ("'Debug'", "'OptimizedDebug'", "'Release'", "'-Werror'", "'-D_DEBUG'"):
            self.assertIn(marker, runner)
        self.assertNotIn("'GLASS_DELPHI_DRAW_PROBE'", runner)
        project = read('GlassCore/GlassCore.vcxproj').split(
            '<ClCompile Include="src\\Core\\Intercept\\DebugDetourHooks.cpp">', 1)[1].split('</ClCompile>', 1)[0]
        self.assertIn("Release|Win32", project)
        self.assertIn("Release|x64", project)

    def test_panel_ansi_decoder_preserves_byte_bounds_and_uses_real_utf16_length(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        decode = code.split('bool DecodeAnsiPanelText(', 1)[1].split('int DrawAnsiText(', 1)[0]
        for marker in ('GetTextCharset(dc)', 'charset < 0 || charset == SYMBOL_CHARSET', 'cp = CP_ACP',
                       'charset != DEFAULT_CHARSET', 'TranslateCharsetInfo(', 'TCI_SRCCHARSET', 'cp = info.ciACP',
                       'MultiByteToWideChar(cp, 0, text, count, nullptr, 0)',
                       'MultiByteToWideChar(cp, 0, text, count, wide.data(), size) != size',
                       'count == -1', 'wide.pop_back()'):
            self.assertIn(marker, decode)
        self.assertNotIn('strlen', decode)
        handler = code.split('int DrawAnsiText(AnsiTextRoute route,', 1)[1].split(
            'int WINAPI PanelDrawTextA(HDC dc,', 1)[0]
        for marker in ('count == 0 || count < -1', 'IsAnsiTextRect(rect)',
                       'DT_CALCRECT | DT_MODIFYSTRING | DT_NOCLIP', 'GetMapMode(dc) != MM_TEXT',
                       'RECT drawRect = *rect', 'DrawTextWithAlpha(dc, wide.data()',
                       'static_cast<int>(wide.size()), &drawRect, flags, result, true)',
                       'const auto target = original ? original : &::DrawTextA',
                       'target(dc, text, count, rect, flags)'):
            self.assertIn(marker, handler)
        for forbidden in ('ApplyBackground(', 'Blacken(', 'FillRect(', 'PatBlt(', 'DrawTextW(',
                          'BomeMemo::', 'DrawTextAttach(', 'SetTextColor(', '::DrawTextA('):
            self.assertNotIn(forbidden, handler)

    def test_panel_ansi_state_error_cleanup_and_production_harness_stay_scoped(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        handler = code.split('int DrawAnsiText(AnsiTextRoute route,', 1)[1].split(
            'int WINAPI PanelDrawTextA(HDC dc,', 1)[0]
        for marker in ('DWORD exitError = entryError', 'SetLastError(exitError)', 'SetLastError(entryError)',
                       'exitError = GetLastError()', 'SaveDC(dc)', 'RestoreDC(dc, saved)',
                       'SetBkMode(dc, TRANSPARENT)', 'g_inHook = true', 'g_inHook = false',
                       'catch (...)', 'TakeDiag(4)', 'text api=DrawTextA', 'ansiCount=%d wideCount=%d cp=%u'):
            self.assertIn(marker, handler)
        self.assertLess(handler.index('GetLastError()'), handler.index('AnsiTextOriginal()'))
        self.assertLess(handler.index('SetLastError(exitError)'), handler.index('AnsiTextOriginal()'))
        self.assertLess(handler.index('RestoreDC(dc, saved)'), handler.index('return SUCCEEDED(hr) ? result : native()'))
        self.assertNotIn('lpchText=%s', handler)
        self.assertNotIn('wide.c_str()', handler)
        runner = read('WndCompat/tests/panel_rendering_test.py')
        for marker in ("'Release'", "'Debug'", "'DebugProbe'", "'-Werror'", "'-pthread'"):
            self.assertIn(marker, runner)
        tests = read('WndCompat/tests/panel_rendering_tests.cpp')
        self.assertIn('../../GlassCore/src/Hooks/delphi/TPanelDraw.cpp', tests)
        for marker in ('AnsiCountsAndUtf16Length', 'AnsiConversionFailures', 'AnsiNestedRefDeltas',
                       'AnsiExportWithoutImportRoute', 'AnsiExportAndIndependentIatOccupant',
                       'AnsiDetourAcquisitionFailures', 'AnsiDetachFailureAndShutdownLifetime',
                       'AnsiPreservesDcClippingAndBackground',
                       'AnsiThreadLocalContext', 'AnsiLastError', 'AnsiRendererFailures'):
            self.assertIn(marker, tests)

    def test_page_ansi_gate_is_separate_exact_class_and_paint_print_only(self):
        shared = read('GlassShared/include/GlassDelphiPanel.h')
        gate = shared.split('inline bool IsDelphiPageAnsiPaintWindow(', 1)[1]
        for marker in ('message != WM_PAINT && message != WM_PRINTCLIENT',
                       'hwnd && GetClassNameW(hwnd, name', '_wcsicmp(name, L"TPage") == 0'):
            self.assertIn(marker, gate)
        for forbidden in ('TPanel', 'TPageControl', 'TTabSheet', 'WM_ERASEBKGND',
                          'wcsstr', 'wcsncmp', 'GetDC', 'IsDelphiPanelClassName'):
            self.assertNotIn(forbidden, gate)
        bridge = read('WndCompat/Root/DelphiPanelPaint.h')
        self.assertIn('(!GlassShared::IsDelphiPanelWindow(hwnd)\n'
                      '                && !GlassShared::IsDelphiPageAnsiPaintWindow(hwnd, message))', bridge)
        self.assertIn('!begin || !end || !begin(hwnd, message, reinterpret_cast<HDC>(wParam))', bridge)
        self.assertIn('DefSubclassProc(hwnd, message, wParam, lParam)', bridge)

    def test_page_ansi_iat_has_one_private_exe_slot_and_does_not_replace_panel_export(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        table = code.split('g_pageTextDep{{', 1)[1].split('}};', 1)[0]
        self.assertEqual(re.findall(r'"([A-Za-z0-9]+)", reinterpret_cast<PVOID>', table), ['DrawTextA'])
        for marker in ('"user32.dll"sv', '"ext-ms-win-ntuser-draw-l1-1-0.dll"sv',
                       '"ext-ms-win-ntuser-misc-l1-1-0.dll"sv', 'reinterpret_cast<PVOID>(PageDrawTextA)'):
            self.assertIn(marker, table)
        self.assertIn('HookDispatcher<1, 3> g_pageTextDispatcher{g_pageTextDep, nullptr}', code)
        self.assertIn('g_pageTextDispatcher.moduleAddress = GlassCore::ModuleHandler::g_entryPointModule', code)
        self.assertIn('g_pageTextDispatcher.CacheHookData()', code)
        original = code.split('decltype(&::DrawTextA) PageAnsiTextOriginal()', 1)[1].split(
            'bool IsNestedImagePanel(', 1)[0]
        self.assertIn('g_lock.lock_shared()', original)
        self.assertIn('g_pageTextDispatcher.GetOrg<0, decltype(&::DrawTextA)>()', original)
        self.assertNotIn('g_actualDrawTextA', original)
        for forbidden in ('IATRegistry::Instance()', 'WriteMemory(', 'GetIAT(', 'VirtualProtect(',
                          'SetEnabled(', 'DDH_All', 'DrawTextExAttach'):
            self.assertNotIn(forbidden, code)

    def test_page_ansi_iat_ref_delta_mask_release_and_shutdown_are_balanced(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        acquire = code.split('bool AcquirePageAnsiText(HWND hwnd)', 1)[1].split('void ReleaseSession(', 1)[0]
        for marker in ('const int before = g_pageTextDispatcher.GetRef(0)',
                       'g_pageTextDispatcher.EnableHook(0, true)',
                       'const int refs = g_pageTextDispatcher.GetRef(0)', 'const bool acquired = refs > before',
                       'return acquired', 'text-iat api=DrawTextA class=TPage enable=1', 'acquired=%d refs=%d'):
            self.assertIn(marker, acquire)
        release = code.split('void ReleaseSession(', 1)[1].split('decltype(&::DrawTextA) AnsiTextOriginal()', 1)[0]
        self.assertIn('if (s.pageAnsiTextAcquired)', release)
        self.assertIn('g_pageTextDispatcher.EnableHook(0, false)', release)
        self.assertIn('text-iat api=DrawTextA class=TPage enable=0', release)
        self.assertIn('g_pageTextDispatcher.DisableAllHooks()', code.split('void Shutdown() noexcept', 1)[1])
        begin = code.split('bool BeginPaint(HWND hwnd,', 1)[1].split('void EndPaint()', 1)[0]
        for marker in ('const bool pageText = GlassShared::IsDelphiPageAnsiPaintWindow(hwnd, message)',
                       'if (!background && !pageText) return false', 'g_lock.lock_exclusive()',
                       's.pageAnsiTextAcquired = pageText && AcquirePageAnsiText(hwnd)',
                       'if (s.pageAnsiTextAcquired) mask |= kPageAnsiTextMask',
                       'catch (...) { ReleaseSession(s); return false; }'):
            self.assertIn(marker, begin)
        self.assertIn('constexpr unsigned kPageAnsiTextMask = 1u << (Count + 1)', code)
        self.assertLess(begin.index('AcquirePageAnsiText(hwnd)'), begin.index('if (!mask) return false'))

    def test_page_text_session_cannot_authorize_inherited_background_or_image_hooks(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        begin = code.split('bool BeginPaint(HWND hwnd,', 1)[1].split('void EndPaint()', 1)[0]
        self.assertIn('const bool background = GlassShared::IsDelphiPanelWindow(hwnd)', begin)
        self.assertIn('s.panelBackground = background', begin)
        self.assertIn('for (size_t i = 0; background && g_dispatcher.moduleAddress && i < Count;', begin)
        rect = code.split('bool IsBackgroundRect(', 1)[1].split('bool SolidBrushColor(', 1)[0]
        self.assertIn('!g_stack.back().panelBackground', rect)
        self.assertLess(rect.index('!IsPanelDc(dc)'), rect.index('!g_stack.back().panelBackground'))
        image = code.split('BOOL WINAPI PanelStretch(HDC dc,', 1)[1].split('BOOL WINAPI PanelExt(HDC dc,', 1)[0]
        self.assertIn('!g_stack.back().acquired[StretchSlot]', image)
        for source in ('GlassCore/src/Hooks/RootWindowHooks.cpp', 'GlassCore/src/Hooks/RootGdiHooks.cpp',
                       'WndCompat/Root/RootSubclass.cpp', 'WndCompat/Tracker/WindowClassifier.cpp',
                       'GlassCore/src/Subsystems/PaintDispatcher.cpp'):
            self.assertNotIn('IsDelphiPageAnsiPaintWindow', read(source))

    def test_page_iat_and_panel_export_use_one_cached_text_context_and_native_fallback(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        handler = code.split('int DrawAnsiText(AnsiTextRoute route,', 1)[1].split(
            'int WINAPI PanelDrawTextA(HDC dc,', 1)[0]
        for marker in ('route == AnsiTextRoute::PageIat',
                       'const auto original = pageIat ? PageAnsiTextOriginal() : AnsiTextOriginal()',
                       'if (!original || g_inHook || !IsPanelDc(dc) || !g_stack.back().ansiTextPaint) return native()',
                       'const bool previous = g_inHook', 'g_inHook = previous',
                       'const auto target = original ? original : &::DrawTextA',
                       'target(dc, text, count, rect, flags)', 'L"page-iat" : L"panel-export"', 'route=%s'):
            self.assertIn(marker, handler)
        for forbidden in ('pageAnsiTextAcquired', 'ansiTextAcquired', 'IsDelphiPageAnsiPaintWindow',
                          'IsAnsiTextPanel', 'GetClassNameW', 'eligible'):
            self.assertNotIn(forbidden, handler)
        native = handler.split('const auto native = [&] {', 1)[1].split('if (!original || g_inHook', 1)[0]
        self.assertIn('g_inHook = true', native)
        self.assertNotIn('pageIat', native)
        self.assertNotRegex(native, r'\bif\s*\(')
        self.assertEqual(handler.count('target(dc, text, count, rect, flags)'), 1)
        self.assertNotIn('return PanelDrawTextA(', handler)
        self.assertNotIn('return PageDrawTextA(', handler)

    def test_ansi_paint_context_is_checked_once_not_derived_from_hook_acquisition(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        self.assertIn('bool ansiTextPaint = false', code)
        begin = code.split('bool BeginPaint(HWND hwnd,', 1)[1].split('void EndPaint()', 1)[0]
        for marker in ('const bool panelText = IsAnsiTextPanel(hwnd, message)',
                       'const bool pageText = GlassShared::IsDelphiPageAnsiPaintWindow(hwnd, message)',
                       's.ansiTextPaint = panelText || pageText',
                       's.ansiTextAcquired = panelText && AcquireAnsiText()',
                       's.pageAnsiTextAcquired = pageText && AcquirePageAnsiText(hwnd)'):
            self.assertIn(marker, begin)
        self.assertEqual(begin.count('IsAnsiTextPanel(hwnd, message)'), 1)
        self.assertEqual(begin.count('IsDelphiPageAnsiPaintWindow(hwnd, message)'), 1)
        self.assertLess(begin.index('s.ansiTextPaint = panelText || pageText'), begin.index('g_lock.lock_exclusive()'))
        self.assertLess(begin.index('if (!mask) return false'), begin.index('g_stack.push_back(s)'))
        tests = read('WndCompat/tests/panel_rendering_tests.cpp')
        for marker in ('iatForwardCalls == 0', 'SharedAnsiContextDoesNotDependOnTheMatchingHookLease',
                       'SharedAnsiFallbackIsNativeOnceForBothOwnersAndEntries',
                       'PanelTest::classQueries == classQueries',
                       'SharedAnsiMissingOriginalAndStaleIatStayNativeUnderExport'):
            self.assertIn(marker, tests)

    def test_page_iat_harness_executes_production_bridge_and_hybrid_renderer_paths(self):
        test = read('WndCompat/tests/panel_rendering_tests.cpp')
        self.assertIn('#include "../../GlassCore/src/Hooks/delphi/TPanelDraw.cpp"', test)
        self.assertIn('#include "../Root/DelphiPanelPaint.h"', test)
        for marker in ('PageIatMessageRoute', 'PageIatExactClassesAndMessages',
                       'PageIatMissingImportAndRegistration', 'PageIatIndependentOfResolvedExport',
                       'PageIatBackgroundIsolationUnderPanel', 'PageIatDrawsNestedPanelDirectly',
                       'PageIatHybridFallbackNativeOnce', 'PageIatFirstOccupantAndPeerLifetime',
                       'PageIatOriginalAndStaleHandler', 'SharedAnsiNativeFallbackOriginalReentry',
                       'PageIatThreadLocalContext',
                       'PageIatFailureFallbacks', 'PageIatStateAndLastError'):
            self.assertIn(marker, test)
        runner = read('WndCompat/tests/panel_rendering_test.py')
        self.assertIn("self.run_case('page-route')", runner)
        self.assertIn("self.run_case('page-nested-panel')", runner)
        shim = read('WndCompat/tests/panel_test_shim.h')
        self.assertIn('PanelCaptureLogString(args)', shim)
        self.assertIn('first-active-occupant model', shim)

    def test_panel_hook_bridge_and_exact_class_header_are_registered(self):
        for project in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            self.assertIn(r'src\Hooks\delphi\TPanelDraw.cpp', read(project))
            self.assertIn(r'Include\Hooks\delphi\TPanelDraw.h', read(project))
        for project in ('GlassShared/GlassShared.vcxproj', 'GlassShared/GlassShared.vcxproj.filters'):
            self.assertIn(r'include\GlassDelphiPanel.h', read(project))
        for project in ('WndCompat/WndCompat.vcxproj', 'WndCompat/WndCompat.vcxproj.filters'):
            self.assertIn(r'Root\DelphiPanelPaint.h', read(project))
        for export in ('GlassCore_BeginDelphiPanelPaint', 'GlassCore_EndDelphiPanelPaint'):
            self.assertIn(export, read('GlassCore/GlassCore.def'))
            self.assertIn(export, read('GlassCore/src/Core/Exports.cpp'))
            self.assertIn(export, read('WndCompat/Root/DelphiPanelPaint.h'))
        engine = read('GlassCore/src/Core/Engine.cpp')
        self.assertIn('GlassCore::DelphiPanel::Startup();', engine)
        self.assertIn('GlassCore::DelphiPanel::Shutdown();', engine)

    def test_opaque_bitmap_source_is_a_scoped_gdi_copy_not_a_pixel_color_key(self):
        code = read('GlassCore/src/Subsystems/OpaqueBitmapSource.cpp')
        header = read('GlassCore/Include/Subsystems/OpaqueBitmapSource.h')
        for marker in ('GetObjectType(source) != OBJ_MEMDC', 'info.bmPlanes != 1',
                       'info.bmBitsPixel < 8', 'info.bmBitsPixel > 32',
                       'info.bmWidth <= 0 || info.bmHeight <= 0',
                       'std::numeric_limits<size_t>::max', 'std::numeric_limits<LONG>::max',
                       'GdiFlush()', 'CreateBitmap(1, 1, 1, 1, nullptr)',
                       'SelectObject(source, placeholder.get())', 'SelectObject(source, original)',
                       'CreateDIBSection(source, &dib', 'dib.bmiHeader.biBitCount = 32',
                       'dib.bmiHeader.biHeight = -info.bmHeight',
                       'data, &dib, DIB_RGB_COLORS) != info.bmHeight',
                       'pixels[i] |= 0xFF000000u', 'SelectObject(source, copy.get())',
                       'SelectObject(m_dc, m_original)', 'm_bitmap = std::move(copy)'):
            self.assertIn(marker, code)
        self.assertLess(code.index('SelectObject(source, placeholder.get())'), code.index('GetDIBits(source,'))
        self.assertIn('wil::unique_hbitmap m_bitmap', header)
        self.assertIn('OpaqueBitmapSource(const OpaqueBitmapSource&) = delete', header)
        for forbidden in ('ApplyBlackKeyAlpha', 'SpriteEffect', 'HasAlphaChannel(', 'GdiAlphaBlend(',
                          'Gdip', 'BufferedPaintSetAlpha', 'SetBkColor(', 'SetMapMode('):
            self.assertNotIn(forbidden, code)

    def test_ttoolbar_vcl_exe_route_and_native_barrier_do_not_reuse_generic_alpha_renderer(self):
        code = read('GlassCore/src/Hooks/ToolbarHooks.cpp')
        gate = code.split('static bool IsVclToolbarWindow(', 1)[1].split('static bool TakeVclImageDiag(', 1)[0]
        for marker in ('StrEqI(name, L"TToolBar")', 'GWLP_HINSTANCE', 'caller == exe',
                       'IsFullInjectionMode()', 'GlassIsBackdropAllowedForHwnd(hwnd)',
                       'g_shutdown.load', 'g_inOpaqueIcon', 'GetMapMode(dc) != MM_TEXT',
                       's.iconModules[0]', 's.iconModules[1]', 'IsTopWindowDrawingSession(s.hwnd)',
                       'IsPaintStackDcForHwnd(s.hwnd, dc)', 'GetStackOwnerForDc(dc) == s.hwnd'):
            self.assertIn(marker, gate)
        self.assertNotIn('GetPaintSessionHostHwnd', gate)
        begin = code.split('static void OnBeginGlobal(', 1)[1].split('static void OnEndGlobal(', 1)[0]
        for marker in ('session.vclIcons = IsVclToolbarWindow(hWnd)',
                       'session.vclIcons && IsVclToolbarIconSession(hWnd, callerMod)',
                       'g_comctl32New', 'g_comctl32Legacy', 'modules[i] != modules[0]',
                       'EnableHooksOnModule(modules[i], OHK_MASKBLT, true)', 'g_modStack.push_back(session)'):
            self.assertIn(marker, begin)
        self.assertNotIn('OHK_BITBLT', begin)
        self.assertNotIn('OHK_MASKBLT, true)', begin.split('if (session.vclIcons', 1)[0])
        enable = code.split('static bool EnableHooksOnModule(', 1)[1].split('// ── DisableAllOverallHooks', 1)[0]
        self.assertIn('const int before = it->second.GetRef(index)', enable)
        self.assertIn('it->second.GetRef(index) > before', enable)
        end = code.split('static void OnEndGlobal(', 1)[1].split('//  Lifecycle', 1)[0]
        self.assertIn('session.iconModules[i-1]', end)
        self.assertNotIn('g_comctl32', end)

    def test_ttoolbar_opaque_path_preserves_mask_rop_and_does_not_suppress_native_bitblt(self):
        code = read('GlassCore/src/Hooks/ToolbarHooks.cpp')
        mask = code.split('BOOL WINAPI Overall_MyMaskBlt(\n', 1)[1].split('// ── BitBlt', 1)[0]
        vcl = mask.split('if (g_modStack.back().vclIcons)', 1)[1].split('if (!IsToolbarGlassSessionAllowedForHdc(hdcDest))', 1)[0]
        for marker in ('OpaqueBitmapSource image{hdcSrc}', 'IsVclToolbarIconDc(hdcDest, callerMod)',
                       'hbmMask, xMask, yMask, rop)', 'result = native()',
                       'error = GetLastError()', 'SetLastError(error)', 'TakeVclImageDiag()'):
            self.assertIn(marker, vcl)
        self.assertEqual(vcl.count('result = native()'), 1)
        for forbidden in ('GdiAlphaBlend', 'HasAlphaChannel', 'ApplyIconBackgroundAlpha',
                          'g_toolbarSuppressMaskBitBlt'):
            self.assertNotIn(forbidden, vcl)
        bit = code.split('BOOL WINAPI Overall_MyBitBlt(\n', 1)[1].split('// ── CreateSolidBrush', 1)[0]
        self.assertIn('if (g_modStack.back().vclIcons)\n            return orig(', bit)
        self.assertIn('GdiAlphaBlend', bit)  # existing common-controls renderer retained

    def test_panel_image_slot_is_paint_only_exact_nested_exe_tpanel_and_preserves_native_stretch(self):
        code = read('GlassCore/src/Hooks/delphi/TPanelDraw.cpp')
        enum = code.split('enum : size_t {', 1)[1].split('Count };', 1)[0]
        table = code.split('g_dep{{', 1)[1].split('}};', 1)[0]
        self.assertEqual(len(re.findall(r'\b\w+Slot\b', enum)), 4)
        self.assertEqual(re.findall(r'"([A-Za-z0-9]+)", reinterpret_cast<PVOID>', table),
                         ['FillRect', 'PatBlt', 'ExtTextOutW', 'StretchBlt'])
        gate = code.split('bool IsNestedImagePanel(', 1)[1].split('bool IsPanelDc(', 1)[0]
        for marker in ('g_entryPointModule', 'GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)',
                       '_wcsicmp(name, L"TPanel") == 0', 'GetParent(hwnd)',
                       '_wcsicmp(parentName, L"TPanel") == 0'):
            self.assertIn(marker, gate)
        self.assertNotIn('wcsstr', gate)
        self.assertIn('i == StretchSlot && (message != WM_PAINT || !IsNestedImagePanel(hwnd))', code)
        image = code.split('BOOL WINAPI PanelStretch(HDC dc,', 1)[1].split('BOOL WINAPI PanelExt(HDC dc,', 1)[0]
        for marker in ('Original<StretchSlot, decltype(&::StretchBlt)>', 'IsPanelDc(dc)',
                       'g_stack.back().acquired[StretchSlot]', 'IsNestedImagePanel(g_stack.back().hwnd)',
                       'IsTopWindowDrawingSession', 'IsPaintStackDcForHwnd', 'GetMapMode(source) != MM_TEXT',
                       'bitmap.bmWidth == client.right-client.left', 'bitmap.bmHeight == client.bottom-client.top',
                       'OpaqueBitmapSource image{source}', 'source, sx, sy, sw, sh, rop)',
                       'SetLastError(error)', 'TakeDiag(3)'):
            self.assertIn(marker, image)
        for forbidden in ('GdiAlphaBlend', 'ApplyBlackKeyAlpha', 'HasAlphaChannel', '_ReturnAddress'):
            self.assertNotIn(forbidden, image)
        self.assertEqual(image.count('result = native()'), 1)

    def test_vcl_images_reuse_one_helper_and_compile_full_production_with_strict_wil_double(self):
        for project in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            for item in (r'src\Subsystems\OpaqueBitmapSource.cpp', r'Include\Subsystems\OpaqueBitmapSource.h'):
                self.assertIn(item, read(project))
        for item in ('GlassCore/src/Hooks/ToolbarHooks.cpp', 'GlassCore/src/Hooks/delphi/TPanelDraw.cpp'):
            self.assertIn('GlassCore::GdiUtils::OpaqueBitmapSource image{', read(item))
        test = read('WndCompat/tests/vcl_images_rendering_tests.cpp')
        for item in ('GlassShared/src/BitmapUtils.cpp', 'GlassCore/src/Subsystems/OpaqueBitmapSource.cpp',
                     'GlassCore/src/Hooks/ToolbarHooks.cpp', 'GlassCore/src/Hooks/delphi/TPanelDraw.cpp'):
            self.assertIn(item, test)
        runner = read('WndCompat/tests/vcl_images_rendering_test.py')
        for marker in ("'Release'", "'Debug'", "'-D_DEBUG'", "'-DGLASS_DELPHI_DRAW_PROBE'", "'-Werror'"):
            self.assertIn(marker, runner)
        shim = read('WndCompat/tests/vcl_images_test_shim.h')
        self.assertIn('#include "panel_test_shim.h"', shim)
        self.assertIn('assert(selected != bmp)', shim)
        self.assertIn('registrationFailures', shim)
        # The shared forwarding double already rejects WIL-invalid lvalue callbacks.
        self.assertIn('auto scope_exit(F&& fn)', read('WndCompat/tests/coolbar_test_shim.h'))

    def test_vcl_listview_mask_route_uses_exact_classes_and_only_the_actual_comctl_caller(self):
        code = read('GlassCore/src/Hooks/ListViewHooks.cpp')
        classes = code.split('static bool IsDelphiListView(', 1)[1].split('// BWM RestoratorPaintHandler', 1)[0]
        for name in ('TListView', 'TSystemListView'):
            self.assertIn('_wcsicmp(cls, L"' + name + '") == 0', classes)
        for forbidden in ('wcsstr', 'wcsncmp', 'find('):
            self.assertNotIn(forbidden, classes)
        gate = code.split('static bool IsVclListViewIconSession(', 1)[1].split('static bool IsVclListViewIconDc(', 1)[0]
        for marker in ('caller &&', 'IsFullInjectionMode()', 'IsWindow(hwnd)', 'IsDelphiListView(hwnd)',
                       'GlassControlKind::ListView', 'GlassIsBackdropAllowedForHwnd(hwnd)',
                       'caller == GlassCore::ModuleHandler::g_comctl32New',
                       'caller == GlassCore::ModuleHandler::g_comctl32Legacy'):
            self.assertIn(marker, gate)
        self.assertNotIn('g_entryPointModule', gate)
        begin = code.split('static void OnBeginPaintGlobal(', 1)[1].split('static void OnEndPaintGlobal(', 1)[0]
        lease = begin.split('ListViewPaintSession session{', 1)[1].split('g_lvSessionStack.push_back(session)', 1)[0]
        for marker in ('session.vclListView && IsVclListViewIconSession(hWnd, callerMod)',
                       'EnableHooksOnModule(callerMod, OHK_MASKBLT, true)', 'session.vclMaskModule = callerMod'):
            self.assertIn(marker, lease)
        for forbidden in ('g_comctl32New', 'g_comctl32Legacy', 'g_entryPointModule', 'OHK_BITBLT', 'OHK_STRETCHBLT'):
            self.assertNotIn(forbidden, lease)
        self.assertIn('EnableListViewHooksForModule(hWnd, callerMod, kind, true, session.vclListView)', begin)

    def test_vcl_listview_mask_ref_is_acquired_by_delta_and_released_from_session_snapshot(self):
        code = read('GlassCore/src/Hooks/ListViewHooks.cpp')
        enable = code.split('static bool EnableHooksOnModule(', 1)[1].split('// ── Предпрогрев', 1)[0]
        for marker in ('if (!enable) return false', 'const int before = it->second.GetRef(index)',
                       'it->second.GetRef(index) > before'):
            self.assertIn(marker, enable)
        end = code.split('static void OnEndPaintGlobal(', 1)[1].split('// Post-BeginPaint:', 1)[0]
        for marker in ('g_lvSessionStack.back().hWnd != hWnd', 'const ListViewPaintSession s',
                       'false, s.vclListView)', 'if (s.vclMaskModule)',
                       'EnableHooksOnModule(s.vclMaskModule, OHK_MASKBLT, false)'):
            self.assertIn(marker, end)
        for forbidden in ('GlassGetWindowKind', 'IsDelphiListView', 'g_comctl32'):
            self.assertNotIn(forbidden, end)
        generic = code.split('static void EnableListViewHooksForModule(', 1)[1].split('//  PaintDispatcher provider callbacks', 1)[0]
        self.assertRegex(generic, r'if \(!vclListView\)\s*EnableHooksOnModule\(g_new, OHK_MASKBLT,\s*enable\)')
        self.assertEqual(generic.count('OHK_MASKBLT'), 1)  # no duplicate VCL ref; legacy leases separately

    def test_vcl_listview_icon_dc_requires_own_tls_ref_and_registered_top_owner_not_ambient_host(self):
        code = read('GlassCore/src/Hooks/ListViewHooks.cpp')
        dc = code.split('static bool IsVclListViewIconDc(', 1)[1].split('static bool AllowDelphiListViewLog(', 1)[0]
        for marker in ('g_shutdown.load', 'g_inVclMaskBlt', 'g_lvSessionStack.empty()',
                       'GetMapMode(dc) != MM_TEXT', 'IsInsideScrollBarDrawThemeOrig()',
                       '!s.vclMaskModule', 'caller != s.vclMaskModule',
                       'IsVclListViewIconSession(s.hWnd, s.mod)', 'IsTopWindowDrawingSession(s.hWnd)',
                       'IsPaintStackDcForHwnd(s.hWnd, dc)', 'owner == s.hWnd',
                       'GetObjectType(dc) == OBJ_MEMDC', 'GetStackOwnerForDc(dc) == s.hWnd'):
            self.assertIn(marker, dc)
        self.assertNotIn('GetPaintSessionHostHwnd', dc)

    def test_vcl_listview_mask_is_opaque_source_plus_one_native_mask_rop_draw_with_error_restored(self):
        code = read('GlassCore/src/Hooks/ListViewHooks.cpp')
        mask = code.split('BOOL WINAPI Overall_MyMaskBlt(\n', 1)[1].split('// ── SetBkColor', 1)[0]
        for marker in ('const DWORD entryError = GetLastError()', 'SetLastError(entryError)',
                       'IsVclListViewIconDc(hdcDest, callerMod)', 'width <= 0 || height <= 0',
                       'hdcSrc == hdcDest', 'GetMapMode(hdcSrc) != MM_TEXT',
                       'info.bmWidth == client.right-client.left', 'info.bmHeight == client.bottom-client.top',
                       'reason=client-sized-source', 'const HWND hwnd = g_lvSessionStack.back().hWnd',
                       'OpaqueBitmapSource image{hdcSrc}', 'hbmMask, xMask, yMask, rop)',
                       'error = GetLastError()', 'SetLastError(error)', 'AllowDelphiListViewLog(3)'):
            self.assertIn(marker, mask)
        self.assertEqual(mask.count('result = native()'), 1)
        for forbidden in ('GdiAlphaBlend', 'HasAlphaChannel', 'Promise32BPP', 'ApplyIconBackgroundAlpha',
                          'ApplyBlackKeyAlpha', 'BufferedPaintSetAlpha', 'FindTrueComctlCaller', 'GetPaintSessionHostHwnd'):
            self.assertNotIn(forbidden, mask)
        self.assertIn('epoch[4]', code)
        self.assertIn('budget[4]', code)

    def test_listview_existing_alpha_blits_and_post_background_policy_are_separate_from_vcl_mask(self):
        code = read('GlassCore/src/Hooks/ListViewHooks.cpp')
        icons = code.split('BOOL WINAPI Overall_MyBitBlt(\n', 1)[1].split('// ── MaskBlt', 1)[0]
        for marker in ('FindTrueComctlCaller()', 'ForceOpaqueAlpha(hConverted.get())',
                       'Promise32BPP', 'ApplyIconBackgroundAlpha', 'GdiAlphaBlend',
                       'IsInsideScrollBarDrawThemeOrig()', 'rop != SRCCOPY'):
            self.assertIn(marker, icons)
        for forbidden in ('vclMaskModule', 'OpaqueBitmapSource', 'g_inVclMaskBlt'):
            self.assertNotIn(forbidden, icons)
        post = code.split('static bool OnListViewAfterBeginPaint(', 1)[1].split('//  Lifecycle', 1)[0]
        self.assertIn('session.needBlackout || session.vclListView', post)
        self.assertNotIn('vclMaskModule', post)
        self.assertNotIn('OpaqueBitmapSource', post)
        self.assertIn('RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal })', code)
        self.assertNotIn('RegisterGetDCProvider', code)
        self.assertNotIn('RegisterCompatDCProvider', code)

    def test_listview_image_suite_compiles_complete_production_and_reuses_strict_bitmap_ref_doubles(self):
        code = read('GlassCore/src/Hooks/ListViewHooks.cpp')
        self.assertIn('#include <Subsystems/OpaqueBitmapSource.h>', code)
        for forbidden in ('ToolbarControl::', 'DelphiPanel::', 'IATRegistry::Instance().Register'):
            self.assertNotIn(forbidden, code)
        test = read('WndCompat/tests/listview_images_rendering_tests.cpp')
        for item in ('GlassShared/src/BitmapUtils.cpp', 'GlassCore/src/Subsystems/OpaqueBitmapSource.cpp',
                     'GlassCore/src/Hooks/ListViewHooks.cpp'):
            self.assertIn('#include "../../' + item + '"', test)
        self.assertIn('#include "vcl_images_test_shim.h"', read('WndCompat/tests/listview_images_test_shim.h'))
        runner = read('WndCompat/tests/listview_images_rendering_test.py')
        for marker in ("'Release'", "'Debug'", "'-D_DEBUG'", "'-DGLASS_DELPHI_DRAW_PROBE'", "'-Werror'"):
            self.assertIn(marker, runner)
        for forbidden in ('read_text', 'source.split(', 'code.split(', '-Wno-error'):
            self.assertNotIn(forbidden, runner)

    def test_delphi_grid_clears_repaint_area_after_beginpaint(self):
        root = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        body = root.split('Root_OnDelphiGridAfterBeginPaint(', 1)[1].split('static void EnableSpecs', 1)[0]
        for marker in ('IsRectEmpty(&ps->rcPaint)', 'L"TStringGrid"', 'L"TDrawGrid"',
                       'L"TShownValueList"', 'PatBlt(hdc, rc.left, rc.top', 'return ok != FALSE;'):
            self.assertIn(marker, body)
        self.assertIn('RegisterPostPaintProvider({ Root_OnDelphiGridAfterBeginPaint })', root)

    def test_restorator_images_gate_is_exact_class_exe_caller_instance_and_empty_tree(self):
        code = read('GlassCore/src/Hooks/delphi/RestoratorImages.cpp')
        host = code.split('Host HostOf(HWND hwnd) noexcept', 1)[1].split('void OnBegin(', 1)[0]
        for cls in ('TResTreeView', 'TImgViewBox'):
            self.assertIn('_wcsicmp(name, L"' + cls + '") == 0', host)
        self.assertEqual(code.count('L"TResTreeView"'), 2)  # the gate and its diagnostic label
        self.assertEqual(code.count('L"TImgViewBox"'), 2)
        for forbidden in ('wcsstr', 'wcsncmp', 'find(', 'Restorator.exe', 'GetModuleFileNameW',
                          'L"TScrollBox"', 'L"TTreeView"', 'L"TPanel"', 'GlassGetWindowKind'):
            self.assertNotIn(forbidden, code)
        begin = code.split('void OnBegin(HWND hwnd, HMODULE caller) noexcept', 1)[1].split('void OnEnd(', 1)[0]
        for marker in ('g_shutdown.load', 'host == Host::None', 'exe && caller == exe',
                       'GlassCore::Root::IsFullInjectionMode()',
                       'GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)',
                       'GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) == exe',
                       'eligible && host == Host::Tree ? static_cast<unsigned>(TreeView_GetCount(hwnd)) : 0',
                       'eligible && !items && Acquire(session.generation)',
                       'g_sessions.push_back(session)',
                       'if (session.leased) Release(session.generation); return;'):
            self.assertIn(marker, begin)
        # Every exact-class BeginPaint is recorded before the lease decides anything.
        self.assertLess(begin.index('host == Host::None'), begin.index('Acquire(session.generation)'))
        end = code.split('void OnEnd(HWND hwnd, HMODULE) noexcept', 1)[1].split('void Startup() noexcept', 1)[0]
        for marker in ('g_sessions.rbegin()', 'if (it->hwnd != hwnd) continue;',
                       'g_sessions.erase((it + 1).base())', 'if (session.leased) Release(session.generation);'):
            self.assertIn(marker, end)
        self.assertEqual(code.count('RegisterPaintProvider({OnBegin, OnEnd})'), 1)
        for forbidden in ('RegisterGetDCProvider', 'RegisterCompatDCProvider', 'RegisterPostPaintProvider'):
            self.assertNotIn(forbidden, code)

    def test_restorator_images_lease_two_private_gdi32_export_detours_in_one_transaction(self):
        code = read('GlassCore/src/Hooks/delphi/RestoratorImages.cpp')
        for marker in ('GetModuleHandleW(L"gdi32.dll")', 'GetProcAddress(gdi32, "BitBlt")',
                       'GetProcAddress(gdi32, "StretchBlt")', 'decltype(&::BitBlt) g_actualBitBlt{}',
                       'decltype(&::StretchBlt) g_actualStretchBlt{}'):
            self.assertIn(marker, code)
        self.assertEqual(code.count('Detours::Write('), 2)
        self.assertEqual(code.count('Detours::Attach('), 2)
        self.assertEqual(code.count('Detours::Detach('), 2)
        for forbidden in ('HookDispatcher', 'IATRegistry', 'EnableHook', 'DetourFindFunction',
                          'gdi32full', 'ext-ms-win-gdi'):
            self.assertNotIn(forbidden, code)
        acquire = code.split('bool Acquire(unsigned& generation) noexcept', 1)[1].split('void Release(', 1)[0]
        for marker in ('g_lock.lock_exclusive()', '!g_actualBitBlt || !g_actualStretchBlt',
                       'if (!g_attached)', 'if (SUCCEEDED(hr)) g_attached = true;',
                       'if (FAILED(hr)) return false;', '++g_refs; generation = g_generation;'):
            self.assertIn(marker, acquire)
        self.assertLess(acquire.index('if (FAILED(hr)) return false;'), acquire.index('++g_refs'))
        self.assertLess(acquire.index('&PictureBitBlt'), acquire.index('&PictureStretchBlt'))
        release = code.split('void Release(unsigned generation) noexcept', 1)[1].split('enum class Host', 1)[0]
        for marker in ('g_lock.lock_exclusive()', 'generation != g_generation) return;',
                       'if (g_refs && --g_refs == 0) Detach();'):
            self.assertIn(marker, release)
        detach = code.split('bool Detach() noexcept', 1)[1].split('bool Acquire(', 1)[0]
        for marker in ('if (!g_attached) return true;', 'if (SUCCEEDED(hr)) g_attached = false;',
                       '&PictureBitBlt', '&PictureStretchBlt'):
            self.assertIn(marker, detach)
        startup = code.split('void Startup() noexcept', 1)[1].split('void Shutdown() noexcept', 1)[0]
        for marker in ('if (!g_shutdown.load(std::memory_order_acquire)) return;', 'if (!g_attached)',
                       'g_shutdown.store(false, std::memory_order_release);'):
            self.assertIn(marker, startup)
        self.assertLess(startup.index('if (!g_attached)'), startup.index('GetProcAddress(gdi32, "BitBlt")'))
        self.assertLess(startup.index('RegisterPaintProvider'), startup.index('g_shutdown.store(false'))
        shutdown = code.split('void Shutdown() noexcept', 1)[1]
        for marker in ('g_shutdown.store(true, std::memory_order_release);', 'g_refs = 0; ++g_generation;',
                       'Detach();', 'g_sessions.clear();'):
            self.assertIn(marker, shutdown)
        self.assertLess(shutdown.index('g_shutdown.store(true'), shutdown.index('g_lock.lock_exclusive()'))

    def test_restorator_image_blit_needs_srccopy_into_its_own_paint_dc_and_stays_one_native_call(self):
        code = read('GlassCore/src/Hooks/delphi/RestoratorImages.cpp')
        session = code.split('HWND SessionWindow(void* returnAddress) noexcept', 1)[1].split('template <typename Native>', 1)[0]
        for marker in ('g_inside', 'g_sessions.empty()', '!g_sessions.back().leased', 'g_shutdown.load',
                       'exe && DetourGetContainingModule(returnAddress) == exe ? g_sessions.back().hwnd : nullptr'):
            self.assertIn(marker, session)
        reject = code.split('LPCWSTR Reject(const Blit& blit, BITMAP& info) noexcept', 1)[1].split('bool CutSelectedCopy(', 1)[0]
        for marker in ('if (blit.rop != SRCCOPY) return L"rop";', 'GlassCore::Root::IsFullInjectionMode()',
                       '!IsWindow(blit.hwnd)', 'GlassShared::GlassIsBackdropAllowedForHwnd(blit.hwnd)',
                       'WindowFromDC(blit.dc) != blit.hwnd',
                       '!GlassCore::PaintDispatcher::IsTopWindowDrawingSession(blit.hwnd)',
                       '!GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(blit.hwnd, blit.dc)',
                       '!blit.source || blit.source == blit.dc', 'return RejectBitmap(blit.source, info);'):
            self.assertIn(marker, reject)
        # A memory DC is never a destination here, tracked for the window or not:
        # the owner test precedes the stack test and no ambient host is consulted.
        self.assertLess(reject.index('WindowFromDC(blit.dc) != blit.hwnd'), reject.index('IsPaintStackDcForHwnd'))
        for forbidden in ('OBJ_MEMDC', 'GetStackOwnerForDc', 'GetPaintSessionHostHwnd', 'IsCurrentPaintDcForHwnd'):
            self.assertNotIn(forbidden, code)
        draw = code.split('BOOL DrawPicture(const Blit& blit, bool extents, const Native& native)', 1)[1].split('template <typename Fn> Fn Actual(', 1)[0]
        for marker in ('g_inside = true;', 'wil::scope_exit([]() noexcept { g_inside = false; })',
                       'LPCWSTR reason = extents ? Reject(blit, info) : L"extents";',
                       'GlassCore::GdiUtils::OpaqueBitmapSource picture{blit.source};',
                       'if (!picture) reason = L"copy";',
                       'else if (!CutSelectedCopy(blit.source, info)) reason = L"background";',
                       'else { result = native(); error = GetLastError(); }',
                       'if (reason) { result = native(); error = GetLastError(); }',
                       'TakeDiag(1)', 'SetLastError(error);'):
            self.assertIn(marker, draw)
        self.assertEqual(draw.count('native()'), 2)
        self.assertLess(draw.index('TakeDiag(1)'), draw.index('SetLastError(error);'))
        # The return address is the detour's own caller: taken in each detour body, nowhere else.
        self.assertEqual(code.count('_ReturnAddress()'), 2)
        bit = code.split('BOOL WINAPI PictureBitBlt(HDC dc, int x, int y, int w, int h, HDC source, int sx, int sy, DWORD rop)\n', 1)[1].split('BOOL WINAPI PictureStretchBlt(HDC dc,', 1)[0]
        stretch = code.split('HDC source, int sx, int sy, int sw, int sh, DWORD rop)\n        {', 1)[1].split('// Caller holds g_lock exclusively.', 1)[0]
        for body, call, extents in (
                (bit, 'return actual(dc, x, y, w, h, source, sx, sy, rop);', 'w && h, native);'),
                (stretch, 'return actual(dc, x, y, w, h, source, sx, sy, sw, sh, rop);', 'w && h && sw && sh, native);')):
            for marker in ('const DWORD entryError = GetLastError();', 'void* const returnAddress = _ReturnAddress();',
                           'if (!actual) return FALSE;', 'SetLastError(entryError);', call,
                           'const HWND hwnd = SessionWindow(returnAddress);', 'if (!hwnd) return native();', extents):
                self.assertIn(marker, body)
            self.assertLess(body.index('_ReturnAddress()'), body.index('const auto native = [&]'))
        self.assertIn('Actual(g_actualBitBlt)', bit)
        self.assertIn('Actual(g_actualStretchBlt)', stretch)
        for forbidden in ('::BitBlt(', '::StretchBlt(', 'GdiAlphaBlend', 'AlphaBlend(', 'PatBlt(', 'BLACKNESS',
                          'SetStretchBltMode', 'SetMapMode', 'SelectObject(', 'CreateDIBSection(', 'GetDIBits(',
                          'DeleteObject(', 'CreateCompatibleDC('):
            self.assertNotIn(forbidden, code)

    def test_restorator_image_policy_is_a_module_local_brightness_mask_over_the_shared_opaque_copy(self):
        code = read('GlassCore/src/Hooks/delphi/RestoratorImages.cpp')
        for marker in ('constexpr unsigned kBackground = 16;', 'constexpr std::uint64_t kMaxPixels = 1u << 22;',
                       'return (std::max)({pixel & 0xFFu, (pixel >> 8) & 0xFFu, (pixel >> 16) & 0xFFu});'):
            self.assertIn(marker, code)
        policy = code.split('bool CutBlackBackground(DWORD* pixels, size_t width, size_t height) noexcept', 1)[1].split('struct Blit', 1)[0]
        for marker in ('if (Level(pixels[y * width + x]) <= kBackground) ++dark;',
                       'if (dark * 4 < outline * 3) return false;',
                       'const DWORD mask = Level(pixels[i]);',
                       'pixels[i] = (pixels[i] & 0x00FFFFFFu) | (mask << 24);'):
            self.assertIn(marker, policy)
        # The outline test comes before the only pixel write.
        self.assertLess(policy.index('return false;'), policy.index('pixels[i] ='))
        self.assertEqual(policy.count('pixels[i] ='), 1)
        # The mask is per pixel: no thresholds on content, no neighbourhoods, no regions,
        # nothing forced opaque or keyed out, and therefore nothing to allocate.
        for forbidden in ('std::vector', 'pending', '0xFF000000', ': 0u', 'x + 1', 'i - 1', 'i + width'):
            self.assertNotIn(forbidden, policy)
        self.assertEqual(policy.count('kBackground'), 1)
        for forbidden in ('kInk', 'kSolidSide', 'Solid', 'Weak', 'Corner', 'std::uint32_t'):
            self.assertNotIn(forbidden, code)
        bitmap = code.split('LPCWSTR RejectBitmap(HDC source, BITMAP& info) noexcept', 1)[1].split('LPCWSTR Reject(const Blit& blit', 1)[0]
        for marker in ('GetObjectW(bitmap, sizeof(dib), &dib)', 'size != sizeof(dib) && size != sizeof(BITMAP)',
                       'info.bmWidth <= 0 || info.bmHeight <= 0', '> kMaxPixels', 'return L"size";',
                       'size == sizeof(dib) && info.bmBitsPixel == 32 && info.bmBits && GdiFlush()',
                       'if (bytes[i * 4 + 3]) return L"alpha";'):
            self.assertIn(marker, bitmap)
        self.assertLess(bitmap.index('return L"size";'), bitmap.index('GdiFlush()'))
        copy = code.split('bool CutSelectedCopy(HDC source, const BITMAP& original) noexcept', 1)[1].split('HWND SessionWindow(', 1)[0]
        for marker in ('GetObjectW(copy, sizeof(dib), &dib) != sizeof(dib)', 'info.bmBitsPixel != 32',
                       'info.bmWidth != original.bmWidth || info.bmHeight != original.bmHeight',
                       'return CutBlackBackground(static_cast<DWORD*>(info.bmBits),'):
            self.assertIn(marker, copy)
        self.assertNotIn('catch', copy)
        for forbidden in ('ApplyBlackKeyAlpha', 'ApplyIconBackgroundAlpha', 'SpriteEffect', 'Promise32BPP',
                          'HasAlphaChannel(', 'PrepareBitmapForAlphaBlend', 'QueryIconBackgroundColorRemoval',
                          'Gdip', 'TreeViewControl::', 'DelphiPanel::', 'ToolbarControl::', 'ListViewControl::'):
            self.assertNotIn(forbidden, code)
        # The policy lives only here: shared helpers and the tree hooks do not learn about it.
        for path in ('GlassCore/src/Subsystems/OpaqueBitmapSource.cpp', 'GlassCore/src/Subsystems/GdiHookUtils.cpp',
                     'GlassShared/src/BitmapUtils.cpp', 'GlassCore/src/Hooks/TreeViewHooks.cpp',
                     'GlassCore/src/Hooks/delphi/TPanelDraw.cpp', 'GlassCore/src/Hooks/RootGdiHooks.cpp'):
            for forbidden in ('RestoratorImages', 'CutBlackBackground', 'TImgViewBox'):
                self.assertNotIn(forbidden, read(path))
        self.assertNotIn('OHK_BITBLT', read('GlassCore/src/Hooks/TreeViewHooks.cpp'))

    def test_restorator_images_are_registered_and_bracketed_by_the_paint_dispatcher_lifetime(self):
        for project in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            text = read(project)
            self.assertEqual(text.count(r'src\Hooks\delphi\RestoratorImages.cpp'), 1)
            self.assertEqual(text.count(r'Include\Hooks\delphi\RestoratorImages.h'), 1)
            ET.fromstring(text.encode('utf-8'))
        engine = read('GlassCore/src/Core/Engine.cpp')
        self.assertIn('#include <Hooks/delphi/RestoratorImages.h>', engine)
        self.assertEqual(engine.count('GlassCore::RestoratorImages::Startup();'), 1)
        self.assertEqual(engine.count('GlassCore::RestoratorImages::Shutdown();'), 1)
        self.assertLess(engine.index('GlassCore::RestoratorImages::Startup();'),
                        engine.index('GlassCore::PaintDispatcher::Startup();'))
        self.assertLess(engine.index('GlassCore::RestoratorImages::Shutdown();'),
                        engine.index('GlassCore::PaintDispatcher::Shutdown();'))
        header = read('GlassCore/Include/Hooks/delphi/RestoratorImages.h')
        self.assertIn('namespace GlassCore::RestoratorImages', header)
        self.assertEqual(re.findall(r'\bvoid (\w+)\(\) noexcept;', header), ['Startup', 'Shutdown'])

    def test_restorator_image_suite_compiles_complete_production_over_the_strict_bitmap_doubles(self):
        test = read('WndCompat/tests/restorator_images_rendering_tests.cpp')
        for item in ('GlassShared/src/BitmapUtils.cpp', 'GlassCore/src/Subsystems/OpaqueBitmapSource.cpp',
                     'GlassCore/src/Hooks/delphi/RestoratorImages.cpp'):
            self.assertEqual(test.count('#include "../../' + item + '"'), 1)
        # Only the Detours wrapper is renamed for the production unit; nothing else is redefined around it.
        unit = test.split('#include "../../GlassCore/src/Subsystems/OpaqueBitmapSource.cpp"\n', 1)[1].split('#include <thread>', 1)[0]
        self.assertEqual(unit, '#define Detours RestoratorDetours\n'
                               '#include "../../GlassCore/src/Hooks/delphi/RestoratorImages.cpp"\n'
                               '#undef Detours\n')
        shim = read('WndCompat/tests/restorator_images_test_shim.h')
        self.assertIn('#include "vcl_images_test_shim.h"', shim)
        for marker in ('namespace GlassCore::Hooks::RestoratorDetours', 'if (failCommit) return E_FAIL;',
                       'assert(!installed);', 'assert(installed == op.hook);'):
            self.assertIn(marker, shim)
        # The expected bytes are stated by the tests; the algorithm is not copied into them.
        for text in (test, shim):
            for forbidden in ('bool CutBlackBackground', 'kBackground', 'Ri::Level', 'Ri::CutBlackBackground',
                              'std::max', 'operator new'):
                self.assertNotIn(forbidden, text)
        # Every painted pixel states its alpha, and nothing dark may come out opaque.
        self.assertIn('void Put(int x, int y, BYTE r, BYTE g, BYTE b, BYTE expected)', test)
        self.assertIn('const bool bright=out[3]!=255 || out[0]==255 || out[1]==255 || out[2]==255;', test)
        runner = read('WndCompat/tests/restorator_images_rendering_test.py')
        for marker in ("'Release'", "'Debug'", "'-D_DEBUG'", "'-DGLASS_DELPHI_DRAW_PROBE'", "'-Werror'",
                       "'-std=c++20'"):
            self.assertIn(marker, runner)
        for forbidden in ('read_text', 'source.split(', 'code.split(', '-Wno-error'):
            self.assertNotIn(forbidden, runner)
        cases = re.findall(r'\{"([a-z-]+)",\w+\}', test)
        self.assertEqual(len(cases), 12)
        self.assertEqual(sorted(cases), sorted(re.findall(r"self\.run_case\('([a-z-]+)'\)", runner)))

    def test_hex_dump_is_registered_and_bracketed_by_the_paint_dispatcher_lifetime(self):
        for project in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            text = read(project)
            self.assertEqual(text.count(r'src\Hooks\delphi\THexDumpDraw.cpp'), 1)
            self.assertEqual(text.count(r'Include\Hooks\delphi\THexDumpDraw.h'), 1)
            ET.fromstring(text.encode('utf-8'))
        engine = read('GlassCore/src/Core/Engine.cpp')
        self.assertIn('#include <Hooks/delphi/THexDumpDraw.h>', engine)
        self.assertEqual(engine.count('GlassCore::DelphiHexDump::Startup();'), 1)
        self.assertEqual(engine.count('GlassCore::DelphiHexDump::Shutdown();'), 1)
        # The text renderer is up before the first row can be drawn; providers register before the dispatcher.
        self.assertLess(engine.index('GlassCore::Text::Startup();'),
                        engine.index('GlassCore::DelphiHexDump::Startup();'))
        self.assertLess(engine.index('GlassCore::DelphiHexDump::Startup();'),
                        engine.index('GlassCore::PaintDispatcher::Startup();'))
        self.assertLess(engine.index('GlassCore::DelphiHexDump::Shutdown();'),
                        engine.index('GlassCore::PaintDispatcher::Shutdown();'))
        header = read('GlassCore/Include/Hooks/delphi/THexDumpDraw.h')
        self.assertIn('namespace GlassCore::DelphiHexDump', header)
        self.assertEqual(re.findall(r'\bvoid (\w+)\(\) noexcept;', header), ['Startup', 'Shutdown'])
        # A separate module: the accepted neighbours do not know about it and it does not reach into them.
        for neighbour in ('GlassCore/src/Hooks/delphi/RestoratorImages.cpp', 'GlassCore/src/Hooks/delphi/TPanelDraw.cpp',
                          'GlassCore/src/Hooks/delphi/BomeMemoDraw.cpp', 'GlassCore/src/Hooks/RootGdiHooks.cpp',
                          'GlassCore/src/Hooks/RootWindowHooks.cpp', 'GlassCore/src/Subsystems/RenderText.cpp',
                          'GlassCore/src/Subsystems/PaintDispatcher.cpp'):
            self.assertNotIn('HexDump', read(neighbour))
        code = read('GlassCore/src/Hooks/delphi/THexDumpDraw.cpp')
        for foreign in ('RestoratorImages', 'DelphiPanel', 'BomeMemo', 'OpaqueBitmapSource', 'HookDispatcher',
                        'IATRegistry', 'GetRootOrg'):
            self.assertNotIn(foreign, code)

    def test_hex_dump_gate_is_exact_class_exe_caller_and_instance(self):
        code = read('GlassCore/src/Hooks/delphi/THexDumpDraw.cpp')
        self.assertIn('&& _wcsicmp(name, L"THexDump") == 0;', code)
        self.assertEqual(code.count('L"THexDump"'), 1)
        for forbidden in ('wcsstr', '_wcsnicmp', 'StrStrI', 'L"TBomeMemo"', 'L"TImgViewBox"', 'L"TResVersionEditor"'):
            self.assertNotIn(forbidden, code)
        begin = code.split('void OnBegin(HWND hwnd, HMODULE caller) noexcept', 1)[1].split('void OnEnd(', 1)[0]
        for marker in ('if (g_shutdown.load(std::memory_order_acquire) || !IsHexDump(hwnd)) return;',
                       'const bool eligible = exe && caller == exe && GlassCore::Root::IsFullInjectionMode()',
                       '&& GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)',
                       '&& reinterpret_cast<HMODULE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) == exe;',
                       'session.leased = eligible && Acquire(session.generation);',
                       'try { g_sessions.push_back(session); }',
                       'catch (...) { if (session.leased) Release(session.generation); return; }'):
            self.assertIn(marker, begin)
        # BWM enables the hooks for the paint session only (RestoratorPaintHandler::Paint), not for GetDC.
        self.assertEqual(code.count('GlassCore::PaintDispatcher::RegisterPaintProvider({OnBegin, OnEnd});'), 1)
        for forbidden in ('RegisterGetDCProvider', 'RegisterCompatDCProvider', 'RegisterPostPaintProvider',
                          'SetWindowSubclass', 'WM_ERASEBKGND'):
            self.assertNotIn(forbidden, code)
        session = code.split('HWND SessionWindow(void* returnAddress) noexcept', 1)[1].split('template <typename Native>', 1)[0]
        for marker in ('if (g_inside || g_sessions.empty() || !g_sessions.back().leased',
                       '|| g_shutdown.load(std::memory_order_acquire)) return nullptr;',
                       'return exe && DetourGetContainingModule(returnAddress) == exe ? g_sessions.back().hwnd : nullptr;'):
            self.assertIn(marker, session)

    def test_hex_dump_leases_two_private_export_detours_in_one_transaction(self):
        code = read('GlassCore/src/Hooks/delphi/THexDumpDraw.cpp')
        for marker in ('const HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");',
                       'const HMODULE user32 = GetModuleHandleW(L"user32.dll");',
                       'GetProcAddress(gdi32, "ExtTextOutA")', 'GetProcAddress(user32, "TabbedTextOutA")',
                       'decltype(&::ExtTextOutA) g_actualExtTextOut{};',
                       'decltype(&::TabbedTextOutA) g_actualTabbedTextOut{};'):
            self.assertEqual(code.count(marker), 1, marker)
        self.assertEqual(code.count('GetProcAddress('), 2)
        acquire = code.split('bool Acquire(unsigned& generation) noexcept', 1)[1].split('void Release(', 1)[0]
        self.assertEqual(acquire.count('Detours::Write('), 1)
        self.assertEqual(acquire.count('Detours::Attach('), 2)
        for marker in ('|| !g_actualExtTextOut || !g_actualTabbedTextOut) return false;',
                       'if (FAILED(hr)) return false; // No phantom lease after an aborted transaction.',
                       '++g_refs; generation = g_generation;'):
            self.assertIn(marker, acquire)
        detach = code.split('bool Detach() noexcept', 1)[1].split('bool Acquire(', 1)[0]
        self.assertEqual(detach.count('Detours::Write('), 1)
        self.assertEqual(detach.count('Detours::Detach('), 2)
        self.assertIn('if (SUCCEEDED(hr)) g_attached = false;', detach)
        release = code.split('void Release(unsigned generation) noexcept', 1)[1].split('bool IsHexDump(', 1)[0]
        self.assertIn('if (generation != g_generation) return;', release)
        self.assertIn('if (g_refs && --g_refs == 0) Detach();', release)
        shutdown = code.split('void Shutdown() noexcept', 1)[1]
        for marker in ('g_refs = 0; ++g_generation;', 'Detach(); // Also retries a failed last-session Detach.'):
            self.assertIn(marker, shutdown)
        # A detour never calls its own export: only the resolved original, exactly as the caller passed it.
        ext = code.split('BOOL WINAPI RowExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* rect,', 1)[1]
        ext = ext.split('LONG WINAPI RowTabbedTextOutA(HDC dc,', 1)[0]
        for marker in ('const DWORD entryError = GetLastError();', 'void* const returnAddress = _ReturnAddress();',
                       'if (!actual) return FALSE; // Never attached without it; the export would re-enter.',
                       'return actual(dc, x, y, options, rect, text, count, dx);', 'if (!hwnd) return native();'):
            self.assertIn(marker, ext)
        tabbed = code.split('LONG WINAPI RowTabbedTextOutA(HDC dc, int x, int y, LPCSTR text, int count,', 1)[1]
        tabbed = tabbed.split('bool Detach() noexcept', 1)[0]
        for marker in ('if (!actual) return 0;', 'return actual(dc, x, y, text, count, tabCount, tabs, tabOrigin);',
                       'if (!hwnd) return native();'):
            self.assertIn(marker, tabbed)
        for forbidden in ('::ExtTextOutA(', '::TabbedTextOutA(', 'TextOutA(dc', 'DrawTextA'):
            self.assertNotIn(forbidden, code)

    def test_hex_dump_text_needs_its_own_dc_and_keeps_the_callers_position_and_clipping(self):
        code = read('GlassCore/src/Hooks/delphi/THexDumpDraw.cpp')
        target = code.split('LPCWSTR RejectTarget(HWND hwnd, HDC dc) noexcept', 1)[1].split('bool Decode(', 1)[0]
        for marker in ('|| !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)) return L"mode";',
                       'const HWND owner = dc ? WindowFromDC(dc) : nullptr;',
                       'const bool own = dc && (owner == hwnd || (!owner && GetObjectType(dc) == OBJ_MEMDC',
                       '&& GlassCore::PaintDispatcher::GetStackOwnerForDc(dc) == hwnd));',
                       'if (!own || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(hwnd)',
                       '|| !GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(hwnd, dc)) return L"dc";',
                       'if (GetMapMode(dc) != MM_TEXT) return L"map";'):
            self.assertIn(marker, target)
        draw = code.split('BOOL DrawExtText(HWND hwnd, HDC dc, int x, int y, UINT options, const RECT* rect,', 1)[1]
        draw = draw.split('LONG DrawTabbedText(', 1)[0]
        order = ('LPCWSTR reason = RejectTarget(hwnd, dc);',
                 'if (!reason && (options & (ETO_GLYPH_INDEX | ETO_PDY))) reason = L"options";',
                 'if (!reason && (options & ETO_OPAQUE) && !rect) reason = L"rect";',
                 'if (!reason && !count && !(options & ETO_OPAQUE)) reason = L"empty";',
                 'if (!reason && count && GetTextAlign(dc) != (TA_LEFT | TA_TOP | TA_NOUPDATECP)) reason = L"align";',
                 'if (!reason && count && !dx && GetTextCharacterExtra(dc) != 0) reason = L"spacing";',
                 'if (!decoded) reason = L"conversion";',
                 'else if (dx && wide.size() != count) reason = L"dx";',
                 'try { filled = Background(dc, *rect, GetBkColor(dc)); }',
                 'hr = GlassCore::Text::ExtTextOutWithAlpha(dc, x, y, options & ~static_cast<UINT>(ETO_OPAQUE),',
                 'rect, wide.data(), static_cast<UINT>(wide.size()), dx, &::ExtTextOutW);',
                 'if (reason) { result = native(); error = GetLastError(); }',
                 'SetLastError(error);')
        positions = [draw.index(marker) for marker in order]
        self.assertEqual(positions, sorted(positions)) # every gate precedes the first pixel, the fill precedes the glyphs
        self.assertEqual(draw.count('native()'), 1)
        self.assertEqual(code.count('GlassCore::Text::ExtTextOutWithAlpha('), 2)
        # The donor's rectangle arithmetic (x + 80, DrawText inside lprect) is not reproduced, and the shared
        # renderer is the only thing that draws glyphs: no DrawText path, GDI+, AlphaBlend or colour key here.
        for forbidden in ('x + 80', '+ 80', 'DrawTextWithAlpha', 'DrawThemeContent', 'AlphaBlend', 'Gdip', 'gdiplus',
                          'ApplyBlackKeyAlpha', 'SetBkMode(', 'SetTextColor(', 'SetTextAlign(', 'SaveDC(', 'SelectObject(',
                          'InvalidateRect', 'BeginBufferedPaint'):
            self.assertNotIn(forbidden, code)
        background = code.split('bool Background(HDC dc, const RECT& r, COLORREF color)', 1)[1].split('bool Layout(', 1)[0]
        for marker in ('if (width <= 0 || height <= 0) return true; // nothing to fill',
                       'if (width > INT_MAX || height > INT_MAX) return false;',
                       'const COLORREF rgb = color & 0xFFFFFFu;',
                       'if (!rgb || rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)',
                       '|| rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu))',
                       'return PatBlt(dc, r.left, r.top, static_cast<int>(width), static_cast<int>(height), BLACKNESS) != FALSE;',
                       'Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(rgb, 255)',
                       'return brush && FillRect(dc, &r, brush.get()) != 0;'):
            self.assertIn(marker, background)
        self.assertEqual(code.count('PatBlt('), 1)
        self.assertEqual(code.count('FillRect('), 1)
        # Diagnostics carry metadata only: a row is file content.
        report = code.split('void Report(const Row& row, LPCWSTR reason, long result) noexcept', 1)[1]
        report = report.split('HWND SessionWindow(', 1)[0]
        self.assertIn('if (!TakeDiag(1)) return;', report)
        self.assertIn('L"[GlassCore][DelphiHexDump] text api=%s hwnd=0x%p dc=0x%p dcWnd=0x%p dcType=%lu pos=[%d,%d] '
                      'options=0x%X rect=[%ld,%ld,%ld,%ld] ansiCount=%d wideCount=%zu cp=%u align=0x%X bkMode=%d '
                      'bk=0x%06X color=0x%06X tabs=%d step=%d origin=%d drawn=%d reason=%s result=%ld",', report)
        row = code.split('struct Row', 1)[1].split('};', 1)[0]
        for forbidden in ('LPCSTR', 'std::wstring', 'wchar_t*', 'text'):
            self.assertNotIn(forbidden, row)
        for forbidden in ('%hs', '%S', '%ls', 'wide.c_str()', 'glyphs.c_str()'):
            self.assertNotIn(forbidden, code)

    def test_hex_dump_tabs_are_one_positive_distance_turned_into_glyph_advances(self):
        code = read('GlassCore/src/Hooks/delphi/THexDumpDraw.cpp')
        draw = code.split('LONG DrawTabbedText(HWND hwnd, HDC dc, int x, int y, LPCSTR text, int count,', 1)[1]
        draw = draw.split('template <typename Fn> Fn Actual(', 1)[0]
        order = ('LPCWSTR reason = RejectTarget(hwnd, dc);',
                 'if (!reason && (tabCount != 1 || !tabs || tabs[0] <= 0 || x < tabOrigin)) reason = L"tabs";',
                 'if (!reason && GetTextAlign(dc) != (TA_LEFT | TA_TOP | TA_NOUPDATECP)) reason = L"align";',
                 'try { decoded = text && count > 0 && Decode(dc, text, count, wide, codepage); }',
                 'if (!reason && (!GetTextMetricsW(dc, &metrics) || metrics.tmHeight <= 0',
                 '|| metrics.tmUnderlined || metrics.tmStruckOut)) reason = L"font";',
                 'try { placed = Layout(dc, wide, x, tabs[0], tabOrigin, glyphs, advance, first, width); }',
                 'if (!placed || bottom > INT_MAX) reason = L"layout";',
                 'else extent = RECT{x, y, x + width, static_cast<LONG>(bottom)};',
                 'if (!reason && GetBkMode(dc) == OPAQUE)',
                 'try { filled = Background(dc, extent, GetBkColor(dc)); }',
                 'if (!reason && !glyphs.empty())',
                 'hr = GlassCore::Text::ExtTextOutWithAlpha(dc, first, y, 0, &extent, glyphs.data(),',
                 'static_cast<UINT>(glyphs.size()), advance.data(), &::ExtTextOutW);',
                 'LONG result = MAKELONG(width, metrics.tmHeight);',
                 'if (reason) { result = native(); error = GetLastError(); }',
                 'SetLastError(error);')
        positions = [draw.index(marker) for marker in order]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(draw.count('native()'), 1)
        layout = code.split('bool Layout(HDC dc, const std::wstring& text, int x, int step, int origin,', 1)[1]
        layout = layout.split('struct Row', 1)[0]
        for marker in ("while (i < text.size() && text[i] == L'\\t') { ++tabs; ++i; }",
                       "while (i < text.size() && text[i] != L'\\t') ++i;",
                       'if (cursor < origin || length > INT_MAX) return false;',
                       'const long long start = tabs ? origin + ((cursor - origin) / step + tabs) * step : cursor;',
                       'if (start > INT_MAX) return false;',
                       'if (!GetTextExtentExPointW(dc, text.data() + begin, static_cast<int>(length), 0, nullptr,',
                       'if (advance.empty()) first = static_cast<int>(start);',
                       'const long long reach = advance.back() + (start - cursor);',
                       'const long long after = k + 1 < length ? partial[k] : size.cx;',
                       'if (after < run) return false;',
                       'advance.push_back(static_cast<INT>(after - run));',
                       'cursor = start + run;', 'if (cursor > INT_MAX) return false;',
                       'width = static_cast<int>(cursor - x);'):
            self.assertIn(marker, layout)
        # Tab characters never reach GDI or the renderer, and the layout draws nothing by itself.
        for forbidden in ('ExtTextOut', 'PatBlt', 'FillRect', 'TabbedTextOut', 'GetTabbedTextExtent', 'DT_EXPANDTABS'):
            self.assertNotIn(forbidden, layout)
        for forbidden in ('DT_TABSTOP', 'DT_EXPANDTABS', 'GetTabbedTextExtent', 'MAKELONG(343'):
            self.assertNotIn(forbidden, code)

    def test_hex_dump_suite_compiles_complete_production_over_recording_doubles(self):
        test = read('WndCompat/tests/hexdump_rendering_tests.cpp')
        # The production unit is included as it is: nothing is renamed or redefined around it.
        self.assertTrue(test.startswith('#include "hexdump_test_shim.h"\n'
                                        '#include "../../GlassCore/src/Hooks/delphi/THexDumpDraw.cpp"\n\n'
                                        '#define CHECK(expr)'))
        self.assertEqual(test.count('#include "'), 2)
        shim = read('WndCompat/tests/hexdump_test_shim.h')
        self.assertIn('#include "coolbar_test_shim.h"', shim)
        self.assertEqual(shim.count('#include "'), 1)
        for marker in ('namespace GlassCore::Hooks::Detours', 'if (failCommit) return E_FAIL;', 'assert(!installed);',
                       'assert(installed == op.hook);', "for (int i = 0; i < count; ++i) assert(text[i] != L'\\t');",
                       'assert(dc && w > 0 && h > 0 && rop == BLACKNESS);'):
            self.assertIn(marker, shim)
        # The doubles record requests; neither they nor the tests lay a row out with the production algorithm.
        for text in (test, shim):
            for forbidden in ('Hd::Layout', 'Hd::Background', 'Hd::Decode', 'Hd::RejectTarget', 'bool Layout(',
                              '/ step + tabs', '(cursor - origin)'):
                self.assertNotIn(forbidden, text)
        self.assertIn('struct Case { const char* text; int x, origin, step, first; const wchar_t* glyphs; '
                      'std::vector<INT> dx; int width; };', test)
        runner = read('WndCompat/tests/hexdump_rendering_test.py')
        for marker in ("'Release'", "'Debug'", "'-D_DEBUG'", "'-DGLASS_DELPHI_DRAW_PROBE'", "'-Werror'",
                       "'-std=c++20'", "'Subsystems/RenderText.h'"):
            self.assertIn(marker, runner)
        for forbidden in ('read_text', 'source.split(', 'code.split(', '-Wno-error'):
            self.assertNotIn(forbidden, runner)
        cases = re.findall(r'\{"([a-z-]+)", \w+\}', test)
        self.assertEqual(len(cases), 11)
        self.assertEqual(sorted(cases), sorted(re.findall(r"self\.run_case\('([a-z-]+)'\)", runner)))

    def test_hex_dump_module_is_named_by_its_class_and_reads_no_application_name(self):
        # THexDump is the control of Borland's HexDump.pas, compiled into every application that takes the unit:
        # the rule belongs to the window class. Neither the module nor its harness names an application.
        for path in ('GlassCore/src/Hooks/delphi/THexDumpDraw.cpp', 'GlassCore/Include/Hooks/delphi/THexDumpDraw.h',
                     'WndCompat/tests/hexdump_test_shim.h', 'WndCompat/tests/hexdump_rendering_tests.cpp',
                     'WndCompat/tests/hexdump_rendering_test.py'):
            self.assertNotIn('restorator', read(path).lower(), path)
        code = read('GlassCore/src/Hooks/delphi/THexDumpDraw.cpp')
        self.assertIn('\nnamespace GlassCore::DelphiHexDump\n', code)
        # The gate has no process identity in it: no image name is read or compared.
        for forbidden in ('GetModuleFileName', 'GetModuleBaseName', 'QueryFullProcessImageName',
                          'GetProcessImageFileName', 'PathFindFileName', 'GetCommandLine', '.exe'):
            self.assertNotIn(forbidden, code)
        self.assertEqual(code.count('L"[GlassCore][DelphiHexDump] '), 5)
        self.assertEqual(code.count('[GlassCore]['), 5)
        # The application-named files were renamed, not kept beside the new ones, and nothing wires them in.
        for stale in ('GlassCore/src/Hooks/delphi/RestoratorHexDump.cpp',
                      'GlassCore/Include/Hooks/delphi/RestoratorHexDump.h',
                      'WndCompat/tests/restorator_hexdump_test_shim.h',
                      'WndCompat/tests/restorator_hexdump_rendering_tests.cpp',
                      'WndCompat/tests/restorator_hexdump_rendering_test.py'):
            self.assertFalse((ROOT / stale).exists(), stale)
        for wiring in ('GlassCore/src/Core/Engine.cpp', 'GlassCore/GlassCore.vcxproj',
                       'GlassCore/GlassCore.vcxproj.filters'):
            self.assertNotIn('RestoratorHexDump', read(wiring))

    def test_tcombo_disabled_edit_asks_with_ctlcolorstatic_and_shares_the_field_rule(self):
        combo = read('WndCompat/Popup/ComboLBox/ComboBoxSubclass.cpp')
        # A disabled or read-only Edit sends WM_CTLCOLORSTATIC instead of WM_CTLCOLOREDIT and VCL answers
        # both from one handler with one brush: one block here, the static label falling into the edit one.
        self.assertEqual(combo.count('        case WM_CTLCOLORSTATIC:\n        case WM_CTLCOLOREDIT:\n        {\n'), 1)
        self.assertEqual(combo.count('case WM_CTLCOLORSTATIC:'), 1)
        self.assertEqual(combo.count('case WM_CTLCOLOREDIT:'), 1)
        ctl = combo.split('case WM_CTLCOLOREDIT:', 1)[1].split('case WM_CTLCOLORLISTBOX:', 1)[0]
        # Still only this combo's own Edit child, one native call, the same brush proof.
        for marker in ('IsDelphiComboBox(hWnd)', 'info.hwndItem != edit', 'WndCompat::IsWindowClass(edit, L"Edit")',
                       'const bool replace = solid && (lb.lbColor & 0xFFFFFFu) == windowColor',
                       '&& (bk & 0xFFFFFFu) == windowColor;', 'msg=0x%X'):
            self.assertIn(marker, ctl)
        self.assertEqual(ctl.count('DefSubclassProc('), 1)
        # Whoever is not this field gets the message exactly as it came, last error included.
        self.assertEqual(ctl.count('const DWORD entryError = GetLastError();'), 1)
        self.assertEqual(ctl.count('SetLastError(entryError);'), 3)
        self.assertEqual(ctl.count('{ SetLastError(entryError); break; }'), 2)
        self.assertLess(ctl.index('const DWORD entryError = GetLastError();'), ctl.index('WinEventHook_GetMode()'))
        self.assertLess(ctl.index('SetLastError(entryError);\n            if (!dc) break;'),
                        ctl.index('const LRESULT original = DefSubclassProc('))
        # The state of the edit is not read and no colour of the text is set: the control keeps doing that.
        for forbidden in ('SetTextColor', 'SetBkMode', 'GRAYTEXT', 'IsWindowEnabled', 'WS_DISABLED', 'ES_READONLY',
                          'GetWindowLong', 'PatBlt', 'FillRect'):
            self.assertNotIn(forbidden, ctl)
        harness = read('WndCompat/tests/combo_list_ctlcolor_test.py')
        self.assertIn('else if (name=="disabled-edit") DisabledEdit();', harness)
        self.assertIn("self.run_case('disabled-edit')", harness)
        for marker in ('for (UINT msg:{WM_CTLCOLORSTATIC,WM_CTLCOLOREDIT})', 'L"msg=0x138"', 'L"msg=0x133"'):
            self.assertIn(marker, harness)

    def test_vcl_list_erase_sent_before_beginpaint_is_asked_again_on_the_returned_dc(self):
        code = read('GlassCore/src/Hooks/delphi/TBackgroundHooks.cpp')
        after = code.split('bool AfterPaint(HWND hwnd, HDC dc, LPPAINTSTRUCT ps, HMODULE) noexcept', 1)[1].split(
            '    void Startup() noexcept', 1)[0]
        # Only a list, only when BeginPaint brought no client erase, only with something to repaint.
        self.assertIn('if (s.list && !s.observedDc && dc && ps && !IsRectEmpty(&ps->rcPaint))\n'
                      '                return EraseAgain(g_sessions.size() - 1, hwnd, dc, *ps);', after)
        self.assertLess(after.index('if (!s.armed || !s.paint || s.hwnd != hwnd) return false;'),
                        after.index('s.afterBegin = true; s.paintDc = dc;'))
        self.assertLess(after.index('s.afterBegin = true; s.paintDc = dc;'), after.index('return EraseAgain('))
        self.assertLess(after.index('return EraseAgain('), after.index('if (!s.defaultBrush'))
        again = code.split('bool EraseAgain(size_t slot, HWND hwnd, HDC dc, const PAINTSTRUCT& ps) noexcept', 1)[1].split(
            'bool AfterPaint(', 1)[0]
        # The message Windows itself sends: the DC and nothing in lParam. The handler is the application's,
        # so the session is found again by position and serial, never through a kept reference.
        for marker in ('const unsigned long long serial = g_sessions[slot].serial;',
                       'g_sessions[slot].erasing = true;',
                       'SendMessageW(hwnd, WM_ERASEBKGND, reinterpret_cast<WPARAM>(dc), 0);',
                       'if (slot >= g_sessions.size() || g_sessions[slot].serial != serial) return false;',
                       'g_sessions[slot].erasing = false;',
                       'const bool cleared = g_sessions[slot].erasedAgain;', 'return cleared;',
                       'L"[GlassCore][DelphiBackground] erase-again hwnd=0x%p dc=0x%p paint=[%ld,%ld,%ld,%ld] cleared=%d"',
                       'budget = 12'):
            self.assertIn(marker, again)
        self.assertLess(again.index('g_sessions[slot].erasing = true;'), again.index('SendMessageW('))
        self.assertLess(again.index('SendMessageW('), again.index('g_sessions[slot].serial != serial'))
        self.assertLess(again.index('g_sessions[slot].serial != serial'), again.index('g_sessions[slot].erasing = false;'))
        for forbidden in ('auto& s', 'g_sessions.back()', 'PatBlt', 'FillRect'):
            self.assertNotIn(forbidden, again)
        self.assertEqual(code.count('SendMessageW(hwnd, WM_ERASEBKGND'), 1)
        fill = code.split('int WINAPI ObserveFill(HDC dc, const RECT* r, HBRUSH brush)\n        {', 1)[1].split(
            'bool EraseAgain(', 1)[0]
        branch = fill.split('if (s.erasing)', 1)[1].split('// Once BeginPaint has returned', 1)[0]
        # Replaced, not followed: the returned DC, a default brush, the whole client; anything else is native.
        for marker in ('if (s.paintDc != dc || !sys',
                       '|| !PatBlt(dc, r->left, r->top, r->right-r->left, r->bottom-r->top, BLACKNESS))',
                       'return orig(dc, r, brush);', 'g_sessions[slot].erasedAgain = true;', 'return 1;'):
            self.assertIn(marker, branch)
        self.assertEqual(branch.count('orig(dc, r, brush)'), 1)
        self.assertLess(fill.index('|| !IsFullClient(s.hwnd, dc, r))'), fill.index('if (s.erasing)'))
        self.assertLess(fill.index('const bool sys = IsSystemBrush(brush)'), fill.index('if (s.erasing)'))
        self.assertLess(fill.index('if (s.erasing)'),
                        fill.index('if (s.paint && s.afterBegin && s.paintDc == dc && sys'))
        # No new hook, provider or subclass; no repaint is forced and no pixel is read.
        for forbidden in ('SetWindowSubclass', 'GetPixel', 'RedrawWindow', 'InvalidateRect', 'UpdateWindow',
                          'Detours::', 'GetSysColorBrush', 'CreateSolidBrush', 'PostMessage', 'SendMessageTimeout'):
            self.assertNotIn(forbidden, code)
        for registration in ('RegisterPaintProvider(', 'RegisterPostPaintProvider(', 'RegisterGetDCProvider('):
            self.assertEqual(code.count(registration), 1)
        self.assertIn('HookDispatcher<1, 3> g_dispatcher{g_dep, nullptr};', code)
        self.assertEqual(read('GlassCore/Include/Hooks/delphi/TBackgroundHooks.h').count('noexcept;'), 3)

    def test_vcl_background_suite_compiles_complete_production_over_recording_doubles(self):
        test = read('WndCompat/tests/vcl_background_rendering_tests.cpp')
        self.assertTrue(test.startswith('#include "vcl_background_test_shim.h"\n'
                                        '#include "../../GlassCore/src/Hooks/delphi/TBackgroundHooks.cpp"\n\n'
                                        '#define CHECK(expr)'))
        self.assertEqual(test.count('#include "'), 2)
        shim = read('WndCompat/tests/vcl_background_test_shim.h')
        self.assertEqual(shim.count('#include "'), 0)
        # The window double erases the way TWinControl.WMEraseBkgnd does, through the EXE's import, and the
        # dispatcher double records the paint DC only after the post providers ran.
        for marker in ('if (w->erases && (!w->doubleBuffered || wParam == static_cast<WPARAM>(lParam)))',
                       'ExeFillRect(*GlassCore::ModuleHandler::g_entryPointModule, dc, &w->client, w->brush);',
                       'assert(dc && rop == BLACKNESS);'):
            self.assertIn(marker, shim)
        begin = test.split('bool Begin(TestWindow& w, TestDC& dc, bool erasePending, PAINTSTRUCT* ps = nullptr)', 1)[1].split(
            'void End(TestWindow& w, TestDC& dc)', 1)[0]
        self.assertLess(begin.index('paintProviders.front().onBegin(&w, &exe);'),
                        begin.index('if (erasePending) SendMessageW(&w, WM_ERASEBKGND'))
        self.assertLess(begin.index('if (erasePending) SendMessageW(&w, WM_ERASEBKGND'),
                        begin.index('postProviders.front().onAfterBeginPaint('))
        self.assertLess(begin.index('postProviders.front().onAfterBeginPaint('),
                        begin.index('BgTest::paintDcs[&dc] = &w;'))
        runner = read('WndCompat/tests/vcl_background_rendering_test.py')
        for marker in ("'Release'", "'Debug'", "'-D_DEBUG'", "'-DGLASS_DELPHI_DRAW_PROBE'", "'-Werror'", "'-std=c++20'"):
            self.assertIn(marker, runner)
        for forbidden in ('read_text', 'source.split(', 'code.split(', '-Wno-error'):
            self.assertNotIn(forbidden, runner)
        cases = re.findall(r'name == "([a-z-]+)"', test)
        self.assertEqual(len(cases), 13)
        self.assertEqual(sorted(cases), sorted(re.findall(r"self\.run_case\('([a-z-]+)'\)", runner)))

    def test_res_version_editor_is_registered_and_bracketed_by_the_paint_dispatcher_lifetime(self):
        for project in ('GlassCore/GlassCore.vcxproj', 'GlassCore/GlassCore.vcxproj.filters'):
            text = read(project)
            self.assertEqual(text.count(r'src\Hooks\delphi\TResVersionEditorDraw.cpp'), 1)
            self.assertEqual(text.count(r'Include\Hooks\delphi\TResVersionEditorDraw.h'), 1)
            ET.fromstring(text.encode('utf-8'))
        engine = read('GlassCore/src/Core/Engine.cpp')
        self.assertIn('#include <Hooks/delphi/TResVersionEditorDraw.h>', engine)
        self.assertEqual(engine.count('GlassCore::DelphiResVersionEditor::Startup();'), 1)
        self.assertEqual(engine.count('GlassCore::DelphiResVersionEditor::Shutdown();'), 1)
        # The text renderer is up before the first cell can be drawn; providers register before the dispatcher.
        self.assertLess(engine.index('GlassCore::Text::Startup();'),
                        engine.index('GlassCore::DelphiResVersionEditor::Startup();'))
        self.assertLess(engine.index('GlassCore::DelphiResVersionEditor::Startup();'),
                        engine.index('GlassCore::PaintDispatcher::Startup();'))
        self.assertLess(engine.index('GlassCore::DelphiResVersionEditor::Shutdown();'),
                        engine.index('GlassCore::PaintDispatcher::Shutdown();'))
        header = read('GlassCore/Include/Hooks/delphi/TResVersionEditorDraw.h')
        self.assertIn('namespace GlassCore::DelphiResVersionEditor', header)
        self.assertEqual(re.findall(r'\bvoid (\w+)\(\) noexcept;', header), ['Startup', 'Shutdown'])
        # A separate module: the accepted neighbours do not know about it and it does not reach into them.
        # THexDump keeps its two export detours; this class takes imports of the EXE, so that no second detour
        # is stacked on the same export.
        for neighbour in ('GlassCore/src/Hooks/delphi/RestoratorImages.cpp', 'GlassCore/src/Hooks/delphi/TPanelDraw.cpp',
                          'GlassCore/src/Hooks/delphi/BomeMemoDraw.cpp', 'GlassCore/src/Hooks/delphi/THexDumpDraw.cpp',
                          'GlassCore/src/Hooks/delphi/TBackgroundHooks.cpp', 'GlassCore/src/Hooks/RootWindowHooks.cpp',
                          'GlassCore/src/Subsystems/RenderText.cpp', 'GlassCore/src/Subsystems/PaintDispatcher.cpp'):
            self.assertNotIn('ResVersionEditor', read(neighbour))
        code = read('GlassCore/src/Hooks/delphi/TResVersionEditorDraw.cpp')
        for foreign in ('RestoratorImages', 'DelphiPanel', 'BomeMemo', 'DelphiHexDump', 'OpaqueBitmapSource',
                        'Detours::', 'DetourGetContainingModule', 'GetProcAddress', 'IATRegistry', 'GetRootOrg'):
            self.assertNotIn(foreign, code)
        # The opt-in draw probe knows the class, so that a log can show what the control really calls.
        gdi = read('GlassCore/src/Hooks/RootGdiHooks.cpp')
        self.assertEqual(gdi.count('TResVersionEditor'), 1)
        begin = gdi.split('void BeginVclDrawProbe(HWND hwnd, bool paint)', 1)[1].split('void EndVclDrawProbe(', 1)[0]
        self.assertIn('&& _wcsicmp(cls, L"TPage") != 0 && _wcsicmp(cls, L"TResVersionEditor") != 0))', begin)
        self.assertIn('L"TResVersionEditor", L"tresversioneditor"}', read('WndCompat/tests/vcl_draw_probe_tests.cpp'))

    def test_res_version_editor_gate_is_exact_class_exe_caller_and_instance(self):
        code = read('GlassCore/src/Hooks/delphi/TResVersionEditorDraw.cpp')
        self.assertIn('&& _wcsicmp(name, L"TResVersionEditor") == 0;', code)
        self.assertEqual(code.count('L"TResVersionEditor"'), 1)
        for forbidden in ('wcsstr', '_wcsnicmp', 'StrStrI', 'L"THexDump"', 'L"TStringGrid"', 'L"TBomeMemo"'):
            self.assertNotIn(forbidden, code)
        begin = code.split('void OnBegin(HWND hwnd, HMODULE caller) noexcept', 1)[1].split('void OnEnd(', 1)[0]
        for marker in ('if (g_shutdown.load(std::memory_order_acquire) || !IsVersionEditor(hwnd)) return;',
                       'const bool eligible = exe && caller == exe && GlassCore::Root::IsFullInjectionMode()',
                       '&& GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)',
                       '&& reinterpret_cast<HMODULE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) == exe;',
                       'session.leased = eligible && Acquire(session.generation);',
                       'try { g_sessions.push_back(session); }',
                       'catch (...) { if (session.leased) Release(session.generation); return; }'):
            self.assertIn(marker, begin)
        # BWM enables its hook for the paint session of this class only (RestoratorPaintHandler::Paint).
        self.assertEqual(code.count('GlassCore::PaintDispatcher::RegisterPaintProvider({OnBegin, OnEnd});'), 1)
        for forbidden in ('RegisterGetDCProvider', 'RegisterCompatDCProvider', 'RegisterPostPaintProvider',
                          'SetWindowSubclass', 'WM_ERASEBKGND'):
            self.assertNotIn(forbidden, code)
        session = code.split('HWND SessionWindow() noexcept', 1)[1].split('template <typename Native>', 1)[0]
        for marker in ('if (g_inside || g_sessions.empty() || g_shutdown.load(std::memory_order_acquire)) return nullptr;',
                       'const Session& top = g_sessions.back();',
                       'return top.leased && top.generation == g_generation.load(std::memory_order_acquire) ? top.hwnd : nullptr;'):
            self.assertIn(marker, session)
        # The gate has no process identity in it and names no application.
        for path in ('GlassCore/src/Hooks/delphi/TResVersionEditorDraw.cpp',
                     'GlassCore/Include/Hooks/delphi/TResVersionEditorDraw.h',
                     'WndCompat/tests/resversion_test_shim.h', 'WndCompat/tests/resversion_rendering_tests.cpp',
                     'WndCompat/tests/resversion_rendering_test.py'):
            self.assertNotIn('restorator', read(path).lower(), path)
        for forbidden in ('GetModuleFileName', 'GetModuleBaseName', 'QueryFullProcessImageName',
                          'GetProcessImageFileName', 'PathFindFileName', 'GetCommandLine', '.exe'):
            self.assertNotIn(forbidden, code)
        self.assertEqual(code.count('L"[GlassCore][DelphiResVersionEditor] '), 3)
        self.assertEqual(code.count('[GlassCore]['), 3)

    def test_res_version_editor_leases_two_private_exe_imports_together(self):
        code = read('GlassCore/src/Hooks/delphi/TResVersionEditorDraw.cpp')
        table = code.split('g_dep{{', 1)[1].split('}};', 1)[0]
        self.assertEqual(re.findall(r'"([A-Za-z0-9]+)", reinterpret_cast<void\*>', table), ['ExtTextOutA', 'FillRect'])
        for marker in ('"gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv',
                       '"user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv',
                       'reinterpret_cast<void*>(CellExtTextOutA)', 'reinterpret_cast<void*>(CellFillRect)'):
            self.assertIn(marker, table)
        self.assertIn('enum : size_t { TextSlot, FillSlot, Count };', code)
        self.assertIn('HookDispatcher<Count, 3> g_dispatcher{g_dep, nullptr};', code)
        acquire = code.split('bool Acquire(unsigned& generation) noexcept', 1)[1].split('void Release(', 1)[0]
        # Both or neither, and a reference only when the dispatcher really took it.
        for marker in ('auto lock = g_lock.lock_exclusive();',
                       'if (g_shutdown.load(std::memory_order_acquire) || !exe) return false;',
                       'g_dispatcher.moduleAddress = exe;', 'for (; taken < Count; ++taken)',
                       'const int before = g_dispatcher.GetRef(taken);', 'g_dispatcher.EnableHook(taken, true);',
                       'if (g_dispatcher.GetRef(taken) <= before) break;',
                       'if (taken == Count) { generation = g_generation.load(std::memory_order_acquire); return true; }',
                       'while (taken--) g_dispatcher.EnableHook(taken, false);'):
            self.assertIn(marker, acquire)
        self.assertLess(acquire.index('const int before = g_dispatcher.GetRef(taken);'),
                        acquire.index('g_dispatcher.EnableHook(taken, true);'))
        release = code.split('void Release(unsigned generation) noexcept', 1)[1].split('bool IsVersionEditor(', 1)[0]
        for marker in ('auto lock = g_lock.lock_exclusive();',
                       'if (generation != g_generation.load(std::memory_order_acquire)) return;',
                       'for (size_t i = 0; i < Count; ++i) g_dispatcher.EnableHook(i, false);'):
            self.assertIn(marker, release)
        shutdown = code.split('void Shutdown() noexcept', 1)[1]
        for marker in ('g_shutdown.store(true, std::memory_order_release);',
                       'g_generation.fetch_add(1, std::memory_order_acq_rel);',
                       'g_dispatcher.DisableAllHooks();', 'g_sessions.clear();'):
            self.assertIn(marker, shutdown)
        startup = code.split('void Startup() noexcept', 1)[1].split('void Shutdown() noexcept', 1)[0]
        for marker in ('if (!g_shutdown.load(std::memory_order_acquire)) return;', 'g_dispatcher.CacheHookData();',
                       'g_shutdown.store(false, std::memory_order_release);'):
            self.assertIn(marker, startup)
        # The hooks reach the original through the dispatcher under the shared lock, and by name without one.
        for marker in ('original = g_dispatcher.GetOrg<TextSlot, decltype(&::ExtTextOutA)>(); }',
                       'original = g_dispatcher.GetOrg<FillSlot, decltype(&::FillRect)>(); }',
                       ': ::ExtTextOutA(dc, x, y, options, rect, text, count, dx);',
                       'return original ? original(dc, rect, brush) : ::FillRect(dc, rect, brush);'):
            self.assertIn(marker, code)
        self.assertEqual(code.count('const HWND hwnd = SessionWindow();\n            if (!hwnd) return native();'), 2)
        self.assertEqual(code.count('auto lock = g_lock.lock_shared();'), 2)

    def test_res_version_editor_text_and_fill_need_the_windows_own_dc(self):
        code = read('GlassCore/src/Hooks/delphi/TResVersionEditorDraw.cpp')
        target = code.split('LPCWSTR RejectTarget(HWND hwnd, HDC dc) noexcept', 1)[1].split('bool Decode(', 1)[0]
        # A window DC is served while an open session of its window holds it, whichever session opened last;
        # a memory DC only when the dispatcher recorded it for this window. Never the ambient host.
        for marker in ('if (!GlassCore::Root::IsFullInjectionMode() || !IsWindow(hwnd)',
                       '|| !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)) return L"mode";',
                       'const HWND owner = dc ? WindowFromDC(dc) : nullptr;',
                       '? owner == hwnd && GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(hwnd, dc)',
                       ': GetObjectType(dc) == OBJ_MEMDC && GlassCore::PaintDispatcher::GetStackOwnerForDc(dc) == hwnd;',
                       'if (!own) return L"dc";', 'if (GetMapMode(dc) != MM_TEXT) return L"map";'):
            self.assertIn(marker, target)
        for forbidden in ('IsTopWindowDrawingSession', 'GetPaintSessionHostHwnd', 'IsCurrentPaintDcForHwnd'):
            self.assertNotIn(forbidden, code)
        text = code.split('BOOL DrawCellText(HWND hwnd, HDC dc, int x, int y, UINT options, const RECT* rect,', 1)[1].split(
            'int ClearOwnFill(', 1)[0]
        gates = ('LPCWSTR reason = RejectTarget(hwnd, dc);',
                 'if (!reason && (options & (ETO_GLYPH_INDEX | ETO_PDY))) reason = L"options";',
                 'if (!reason && (options & (ETO_OPAQUE | ETO_CLIPPED)) && !rect) reason = L"rect";',
                 'if (!reason && !count && !(options & ETO_OPAQUE)) reason = L"empty";',
                 'if (!reason && count && GetTextAlign(dc) != (TA_LEFT | TA_TOP | TA_NOUPDATECP)) reason = L"align";',
                 'if (!reason && count && !dx && GetTextCharacterExtra(dc) != 0) reason = L"spacing";',
                 'if (!decoded) reason = L"conversion";',
                 'else if (dx && wide.size() != count) reason = L"dx";',
                 'try { filled = Background(dc, *rect, GetBkColor(dc)); }',
                 'if (!filled) reason = L"background";',
                 'hr = GlassCore::Text::ExtTextOutWithAlpha(dc, x, y, options & ~static_cast<UINT>(ETO_OPAQUE),',
                 'rect, wide.data(), static_cast<UINT>(wide.size()), dx, &::ExtTextOutW);',
                 'if (FAILED(hr)) reason = L"renderer";',
                 'if (reason) { result = native(); error = GetLastError(); }',
                 'SetLastError(error);')
        positions = [text.index(marker) for marker in gates]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(text.count('native()'), 1)
        self.assertEqual(text.count('ExtTextOutWithAlpha('), 1)
        # The caller's text is drawn where the caller put it: nothing is measured, moved or re-laid out here.
        for forbidden in ('DrawTextWithAlpha', 'GetTextExtent', 'SetTextAlign', 'SetBkMode', 'SetBkColor',
                          'SaveDC', 'SelectObject', 'x +', 'y +'):
            self.assertNotIn(forbidden, text)
        background = code.split('bool Background(HDC dc, const RECT& r, COLORREF color)', 1)[1].split(
            'void ReportText(', 1)[0]
        for marker in ('if (!rgb || SystemBackground(rgb))', 'return PatBlt(dc, r.left, r.top, width, height, BLACKNESS) != FALSE;',
                       'Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(rgb, 255)',
                       'return brush && FillRect(dc, &r, brush.get()) != 0;'):
            self.assertIn(marker, background)
        fill = code.split('int ClearOwnFill(HWND hwnd, HDC dc, const RECT* rect, HBRUSH brush, DWORD entryError, const Native& native)', 1)[1].split(
            'BOOL WINAPI CellExtTextOutA(HDC dc,', 1)[0]
        # Only a solid brush of the default window or button-face colour; black, any other colour, patterns and
        # brushes without metadata stay the application's. This is the fill of one call, not a colour key.
        fill_gates = ('LPCWSTR reason = RejectTarget(hwnd, dc);',
                      'if (!reason && (!rect || !Extent(*rect, width, height))) reason = L"rect";',
                      'if (brush && GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb) && lb.lbStyle == BS_SOLID) color = lb.lbColor;',
                      'else reason = L"brush";',
                      'if (!reason && !SystemBackground(color)) reason = L"colour";',
                      'if (!reason && !PatBlt(dc, rect->left, rect->top, width, height, BLACKNESS)) reason = L"clear";',
                      'if (reason) { result = native(); error = GetLastError(); }', 'SetLastError(error);')
        positions = [fill.index(marker) for marker in fill_gates]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(fill.count('native()'), 1)
        self.assertIn('return rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu) || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);', code)
        # Nothing is remapped for the process or kept after the call: no colour, brush or pixel policy.
        for forbidden in ('GetPixel', 'SetPixel', 'GetDIBits', 'CreateBrushIndirect', 'CreateSolidBrush(',
                          'GetSysColorBrush', 'GetStockObject', 'COLOR_HIGHLIGHT', 'SetPropW', 'GetPropW'):
            self.assertNotIn(forbidden, code)

    def test_res_version_editor_suite_compiles_complete_production_over_recording_doubles(self):
        test = read('WndCompat/tests/resversion_rendering_tests.cpp')
        # The production unit is included as it is: nothing is renamed or redefined around it.
        self.assertTrue(test.startswith('#include "resversion_test_shim.h"\n'
                                        '#include "../../GlassCore/src/Hooks/delphi/TResVersionEditorDraw.cpp"\n\n'
                                        '#define CHECK(expr)'))
        self.assertEqual(test.count('#include "'), 2)
        shim = read('WndCompat/tests/resversion_test_shim.h')
        self.assertIn('#include "coolbar_test_shim.h"', shim)
        self.assertEqual(shim.count('#include "'), 1)
        # The EXE reaches the hooks through its import slots; the true functions record what got through.
        for marker in ('const auto it = exe.hooks.find("ExtTextOutA");', 'const auto it = exe.hooks.find("FillRect");',
                       'assert(dc && w > 0 && h > 0 && rop == BLACKNESS);',
                       'e.kind = brush->lbHatch ? ResTest::Kind::Fill : ResTest::Kind::DirectFill;'):
            self.assertIn(marker, shim)
        for text in (test, shim):
            for forbidden in ('Rv::Background', 'Rv::Decode', 'Rv::RejectTarget', 'Rv::SystemBackground',
                              'Rv::DrawCellText', 'Rv::ClearOwnFill'):
                self.assertNotIn(forbidden, text)
        runner = read('WndCompat/tests/resversion_rendering_test.py')
        for marker in ("'Release'", "'Debug'", "'-D_DEBUG'", "'-DGLASS_DELPHI_DRAW_PROBE'", "'-Werror'",
                       "'-std=c++20'", "'Subsystems/RenderText.h'", "'Core/Intercept/HookDispatcher.h'"):
            self.assertIn(marker, runner)
        for forbidden in ('read_text', 'source.split(', 'code.split(', '-Wno-error'):
            self.assertNotIn(forbidden, runner)
        cases = re.findall(r'name == "([a-z-]+)"', test)
        self.assertEqual(len(cases), 8)
        self.assertEqual(sorted(cases), sorted(re.findall(r"self\.run_case\('([a-z-]+)'\)", runner)))

    def test_vcl_erase_sent_outside_sessions_is_replaced_through_the_kept_import(self):
        code = read('GlassCore/src/Hooks/delphi/TBackgroundHooks.cpp')
        outside = code.split('bool EraseOutside(HDC dc, const RECT* r, HBRUSH brush) noexcept', 1)[1].split(
            'int WINAPI ObserveFill(HDC dc, const RECT* r, HBRUSH brush)\n        {', 1)[0]
        # Cheapest first: a memory DC is not asked for its window, and a window with a session of its own
        # is not classified. Then the class, the thread, the whole client inside the clip, the brush - and one fill.
        gates = ('if (GetObjectType(dc) != OBJ_DC) return false;',
                 'const HWND hwnd = WindowFromDC(dc);',
                 'for (const Session& s : g_sessions) if (s.hwnd == hwnd) return false;',
                 'if (!IsEraseTarget(hwnd, target) || GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return false;',
                 'const int clipKind = GetClipBox(dc, &clip);',
                 'const bool whole = IsFullClient(hwnd, dc, r) && clipKind > NULLREGION',
                 '&& clip.left >= r->left && clip.top >= r->top && clip.right <= r->right && clip.bottom <= r->bottom;',
                 'const bool cleared = whole && IsSystemBrush(brush, target.windowOnly)',
                 '&& (!target.number || GlassShared::HasDelphiInputDefaultBackground(hwnd))',
                 '&& PatBlt(dc, r->left, r->top, r->right-r->left, r->bottom-r->top, BLACKNESS);',
                 'return cleared;')
        positions = [outside.index(marker) for marker in gates]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(outside.count('PatBlt('), 1)
        # It neither forwards the call nor touches a session, sends nothing and asks nothing of the window.
        for forbidden in ('orig(', 'FillRect(', 'SendMessage', 'g_sessions[', 'g_sessions.back()', '.push_back(',
                          '.erase(', 'Acquire', 'EnableHook', 'IsCurrentPaintDcForHwnd', 'IsTopWindowDrawingSession',
                          'GetDCOrgEx', 'ClientToScreen', 'GetWindowRect', 'IsInsideDraw'):
            self.assertNotIn(forbidden, outside)
        # The line is written for every fill that came as far as the class and the thread, replaced or not:
        # it is what tells, without a debugger, why a control stayed white.
        self.assertIn('L"[GlassCore][DelphiBackground] erase-outside hwnd=0x%p class=%s dc=0x%p rect=[%ld,%ld,%ld,%ld] '
                      'client=[%ld,%ld] clip=%d[%ld,%ld,%ld,%ld] whole=%d style=%d rgb=0x%06X cleared=%d"', outside)
        self.assertIn('budget = 32', outside)
        self.assertEqual(outside.count('#if defined(_DEBUG)'), 1)
        self.assertEqual(outside.count('#endif'), 1)
        debug = outside.split('#if defined(_DEBUG)', 1)[1].split('#endif', 1)[0]
        self.assertLess(outside.index('const bool cleared = whole && IsSystemBrush('), outside.index('#if defined(_DEBUG)'))
        # Nothing the release build decides on is computed there, and nothing is drawn or returned.
        for forbidden in ('PatBlt', 'return', 'whole =', 'cleared =', 'SetLastError'):
            self.assertNotIn(forbidden, debug)
        self.assertEqual(outside.count('return false;'), 3)

        fill = code.split('int WINAPI ObserveFill(HDC dc, const RECT* r, HBRUSH brush)\n        {', 1)[1].split(
            'bool EraseAgain(', 1)[0]
        # Before every rule of a session, and also while sessions of other windows are open. The checks are
        # this unit's: the caller's last error is put back whatever they did to it.
        order = ('if (!orig) return FillRect(dc, r, brush);',
                 'if (g_shutdown.load(std::memory_order_acquire) || g_inside) return orig(dc, r, brush);',
                 'const DWORD entryError = GetLastError();',
                 'const bool erased = EraseOutside(dc, r, brush);',
                 'SetLastError(entryError);',
                 'if (erased) return 1;',
                 'if (g_sessions.empty()) return orig(dc, r, brush);',
                 'const size_t slot = g_sessions.size() - 1;')
        positions = [fill.index(marker) for marker in order]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(code.count('EraseOutside(dc, r, brush)'), 1)

        # The classes are the classifier's VCL names of the five kinds whose client Glass paints, each under
        # its own kind: a class added there has to be decided here.
        table = code.split('constexpr EraseClass kEraseClasses[] = {', 1)[1].split('        };', 1)[0]
        rows = re.findall(r'\{ L"(\w+)", GlassShared::GlassControlKind::(\w+), (true|false) \},', table)
        self.assertEqual(len(rows), 21)
        self.assertEqual(len({name for name, _, _ in rows}), 21)
        self.assertEqual(table.count('{ L"'), 21)
        classifier = read('WndCompat/Tracker/WindowClassifier.cpp')
        blocks = {
            'Edit': classifier.split('// ── Edit ─', 1)[1].split('return ControlKind::Edit;', 1)[0],
            'RichEdit50W': classifier.split('// ── RichEdit ─', 1)[1].split('return ControlKind::RichEdit50W;', 1)[0].split(
                'return ControlKind::RichEdit20W;', 1)[1],
            'ListBox': classifier.split('// ── ListBox ─', 1)[1].split('return ControlKind::ListBox;', 1)[0],
            'ListView': classifier.split('// ── ListView ─', 1)[1].split('return ControlKind::ListView;', 1)[0],
            'TreeView': classifier.split('// ── TreeView ─', 1)[1].split('return ControlKind::TreeView;', 1)[0],
        }
        for kind, block in blocks.items():
            with self.subTest(kind=kind):
                vcl = sorted(name for name in re.findall(r'Equals\(cls, L"(\w+)"\)', block) if re.match(r'T[A-Z]', name))
                self.assertTrue(vcl)
                self.assertEqual(sorted(name for name, k, _ in rows if k == kind), vcl)
        self.assertEqual({k for _, k, _ in rows}, set(blocks))
        # COLOR_BTNFACE is a colour of its own exactly where the edit hooks keep it.
        self.assertEqual(sorted(name for name, _, only in rows if only == 'true'), ['TMemo', 'TRichEdit'])
        self.assertIn('const bool fill = IsRichEditKind(GlassShared::GlassGetWindowKind(sessionHwnd)) || IsDelphiMemo(sessionHwnd)\n'
                      '            ? rgb != sysWnd : rgb != 0 && rgb != sysBtn && rgb != sysWnd;',
                      read('GlassCore/src/Hooks/EditHooks.cpp'))
        target = code.split('bool IsEraseTarget(HWND hwnd, EraseTarget& target) noexcept', 1)[1].split(
            'bool IsSystemBrush(', 1)[0]
        for marker in ('if (!hwnd || !GlassCore::Root::IsFullInjectionMode()) return false;',
                       'if (c.kind != kind || _wcsicmp(target.cls, c.name) != 0) continue;',
                       'target.windowOnly = c.windowOnly;',
                       'target.number = _wcsicmp(target.cls, L"TNumberBox") == 0;',
                       'return GlassShared::GlassIsBackdropAllowedForHwnd(hwnd);'):
            self.assertIn(marker, target)
        for forbidden in ('wcsstr', 'wcsncmp', '_wcsnicmp', 'L"T"', 'GWLP_HINSTANCE'):
            self.assertNotIn(forbidden, target)
        self.assertIn('|| (!windowOnly && rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu));', code)

        # One reference outlives the sessions: taken when such a control is first seen in a paint or a GetDC,
        # released by Shutdown only. A session's end knows nothing about it.
        keep = code.split('void Keep(HWND hwnd) noexcept', 1)[1].split('void Begin(HWND hwnd, bool paint) noexcept', 1)[0]
        for marker in ('if (g_kept.load(std::memory_order_acquire) || !IsEraseTarget(hwnd, target)) return;',
                       'auto lock = g_lock.lock_exclusive();',
                       'if (!g_kept.load(std::memory_order_acquire) && AcquireLocked())',
                       'g_kept.store(true, std::memory_order_release);'):
            self.assertIn(marker, keep)
        self.assertLess(keep.index('auto lock = g_lock.lock_exclusive();'), keep.index('AcquireLocked()'))
        self.assertEqual(code.count('Keep(hwnd);'), 1)
        begin = code.split('void Begin(HWND hwnd, bool paint) noexcept', 1)[1].split('void End(HWND hwnd, bool paint) noexcept', 1)[0]
        self.assertLess(begin.index('if (g_shutdown.load(std::memory_order_acquire)) return;'), begin.index('Keep(hwnd);'))
        self.assertLess(begin.index('Keep(hwnd);'), begin.index('g_sessions.push_back(s);'))
        end = code.split('void End(HWND hwnd, bool paint) noexcept', 1)[1].split('void PaintBegin(HWND h, HMODULE)', 1)[0]
        self.assertNotIn('g_kept', end)
        self.assertEqual(code.count('g_kept.store('), 2)
        shutdown = code.split('    void Shutdown() noexcept', 1)[1].split('    void ClearObservedListTail(', 1)[0]
        self.assertLess(shutdown.index('g_dispatcher.DisableAllHooks();'),
                        shutdown.index('g_kept.store(false, std::memory_order_release);'))
        # The lock is not recursive: the locked part takes none, and the unlocked entry takes it once.
        locked = code.split('bool AcquireLocked() noexcept', 1)[1].split('bool Acquire() noexcept', 1)[0]
        self.assertNotIn('g_lock', locked.split('\n', 1)[1])
        self.assertEqual(code.count('AcquireLocked()'), 3)
        # Still one dispatcher with one slot of the EXE; nothing is subclassed, detoured or remapped.
        self.assertIn('HookDispatcher<1, 3> g_dispatcher{g_dep, nullptr};', code)
        self.assertEqual(code.count('"FillRect"'), 1)
        for forbidden in ('SetWindowSubclass', 'Detours::', 'SetWindowsHookEx', 'SetSysColors', 'CreateSolidBrush',
                          'GetSysColorBrush', 'FindResource', 'InvalidateRect', 'RedrawWindow'):
            self.assertNotIn(forbidden, code)
        # The neighbours that erase on their own are not touched: the root subclass and the panel bridge.
        root = read('WndCompat/Root/RootSubclass.cpp')
        self.assertEqual(root.count('case WM_ERASEBKGND:'), 1)
        for neighbour in ('WndCompat/Controls/EditSubclass.cpp', 'WndCompat/Controls/ListBoxSubclass.cpp'):
            text = read(neighbour)
            self.assertNotIn('SetWindowSubclass(', text)
            self.assertNotIn('WM_ERASEBKGND', text)

        # The suite: the window double erases through the EXE's import with a DC of its own; the DC doubles
        # tell their kind, window and clip.
        shim = read('WndCompat/tests/vcl_background_test_shim.h')
        for marker in ('inline HWND WindowFromDC(HDC dc) { ++BgTest::windowLookups; return dc ? dc->owner : nullptr; }',
                       'inline DWORD GetObjectType(HDC dc) { return !dc ? 0 : dc->owner || dc->screen ? OBJ_DC : OBJ_MEMDC; }',
                       'inline DWORD GetCurrentThreadId() { return BgTest::thread; }',
                       '{ *out = dc->frame ? dc->frameClip : dc->owner ? dc->owner->client : RECT{}; return dc->clipKind; }',
                       'enum class GlassControlKind { None, ListBox, Edit, ComboBox, RichEdit50W, ListView, TreeView };'):
            self.assertIn(marker, shim)
        test = read('WndCompat/tests/vcl_background_rendering_tests.cpp')
        for case in ('erase-outside', 'outside-gates', 'kept-import', 'outside-diagnostics'):
            self.assertEqual(test.count('name == "%s"' % case), 1)
        # The test table is written out on its own, not derived from the production one.
        self.assertNotIn('kEraseClasses', test)
        self.assertEqual(sorted({name for name in re.findall(r'\{L"(\w+)", GlassControlKind::\w+, (?:true|false)\}', test)
                                 if name != 'tedit'}), sorted(name for name, _, _ in rows))

if __name__ == "__main__":
    unittest.main()
