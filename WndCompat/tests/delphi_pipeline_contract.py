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
        for guard in ('g_sessions.back().ready', 'g_sessions.back().hwnd',
                      'IsTopWindowDrawingSession(host)', 'IsPaintStackDcForHwnd(host, dc)',
                      'GetStackOwnerForDc(dc) == host', 'owner && owner != host',
                      'IsFullInjectionMode()', 'GlassIsBackdropAllowedForHwnd(host)'):
            self.assertIn(guard, code)
        post = code.split('bool AfterBeginPaint(', 1)[1].split('DWORD SystemColor(', 1)[0]
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
        for guard in ('const bool comboField', 'if (!comboField) return false;',
                      'if (s.combo && i != FillSlot && i != ExtTextSlot && i != DrawTextSlot',
                      '&& i != BitBltSlot && i != StretchBltSlot) continue;', 's.field = comboField'):
            self.assertIn(guard, begin_draw)
        self.assertIn('opaque-brush', draw)
        self.assertIn('CreateSolidColorBrushWithAlpha(lb.lbColor, 255)', draw)

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
        ctl = combo.split('case WM_CTLCOLOREDIT:', 1)[1].split('case WM_LBUTTONDOWN:', 1)[0]
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
        self.assertNotIn('TSpinEdit', code)
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

if __name__ == "__main__":
    unittest.main()
