"""Portable source-contract checks; Windows paint/lifecycle tests are still required."""
from pathlib import Path
import os
import re
import unittest

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
        }
        for name, kind in expected.items():
            with self.subTest(name=name):
                self.assertRegex(source, r'(?s)if\s*\([^;{}]*L"' + name +
                                 r'"[^;{}]*\)\s*return ControlKind::' + kind + r'\s*;')

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
        for name in ('TGroupBox', 'TTabSheet'):
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

    def test_richedit_custom_background_is_real_alpha100_not_a_pattern_brush(self):
        source = read('GlassCore/src/Hooks/EditHooks.cpp')
        self.assertIn('kRichEditBackgroundAlpha = 100', source)
        apply = source.split('static bool ApplyRichEditBackgroundRect(', 1)[1].split('//  OVERALL DISPATCHER MAP', 1)[0]
        for required in ('IsRichEditDrawingDc(hwnd, dc)', 'IntersectRect(&r, rect, &client)',
                         'GetSysColor(COLOR_WINDOW)', 'GetSysColor(COLOR_BTNFACE)',
                         'CreateDIBSection', 'bi.bmiHeader.biBitCount = 32',
                         'c) * kRichEditBackgroundAlpha + 127) / 255',
                         'pixel[3] = kRichEditBackgroundAlpha',
                         'AC_SRC_OVER, 0, 255, AC_SRC_ALPHA', 'blacken() && GdiAlphaBlend',
                         'SelectObject(mem, old)', 'DeleteObject(bitmap)', 'DeleteDC(mem)'):
            self.assertIn(required, apply)
        self.assertNotIn('CreateSolidColorBrushWithAlpha', apply)
        self.assertNotIn('FillRect(', apply)
        self.assertNotIn('#define EM_GETBKGNDCOLOR', source)
        self.assertNotIn('GetRichEdit20BackgroundFromEm', source)
        self.assertNotIn('g_re20FullClientBgPrimedHwnd', source)

    def test_richedit_background_routes_share_bound_dc_and_selection_guards(self):
        source = read('GlassCore/src/Hooks/EditHooks.cpp')
        kinds = source.split('static bool IsRichEditKind(', 1)[1].split('// A paint host', 1)[0]
        for kind in ('RichEdit20W', 'WinForms_RichEdit20W', 'RichEdit50W', 'WinForms_RichEdit50W'):
            self.assertIn('GlassControlKind::' + kind, kinds)
        gate = source.split('static bool IsRichEditDrawingDc(', 1)[1].split('static HWND RichEditHwndForDrawingDc(', 1)[0]
        for guard in ('g_shutdown', 'g_inRichEditBackground', 'g_inRichEditText',
                      'GlassIsBackdropAllowedForHwnd', 'IsFullInjectionMode',
                      'GetPaintSessionHostHwnd() != hwnd', 'owner && owner != hwnd',
                      'it->paintDc == dc', 'GetStackOwnerForDc(dc) == hwnd',
                      'IsTopWindowDrawingSession(hwnd)'):
            self.assertIn(guard, gate)
        for api in ('L"FillRect"', 'L"PatBlt"'):
            self.assertRegex(source, r'ApplyRichEditBackgroundRect\([^;]*' + re.escape(api) + r'\)')
        self.assertIn('L"ExtTextOut", callerMod)', source)
        self.assertIn('GetSysColor(COLOR_HIGHLIGHT)', source)
        self.assertIn('SendMessageW(hwnd, EM_EXGETSEL', source)
        self.assertIn('return selected.cpMin != selected.cpMax', source)
        self.assertIn('GetCurrentGetDcSessionHdc()', source)
        self.assertIn('&& !IsRichEditKind(GlassShared::GlassGetWindowKind(hWnd))', source)
        self.assertIn('rop != PATCOPY', source)
        self.assertIn('x > (std::numeric_limits<int>::max)() - width', source)

    def test_richedit_native_text_dc_policy_is_separate_from_standalone_fills(self):
        source = read('GlassCore/src/Hooks/EditHooks.cpp')
        text_gate = source.split('static bool IsRichEditTextDrawingDc(', 1)[1].split('// Keep the existing system-selection', 1)[0]
        for guard in ('IsRichEditDrawingDc(hwnd, dc)', 'IsRichEditKind(GlassShared::GlassGetWindowKind(hwnd))',
                      'WindowFromDC(dc)', 'GetObjectType(dc) != OBJ_MEMDC',
                      'GetStackOwnerForDc(dc)', 'IsTopWindowDrawingSession(hwnd)',
                      'L"riched20.dll"', 'L"msftedit.dll"', 'L"gdi32full.dll"',
                      'it->paintDc', 'WindowFromDC(it->paintDc) == hwnd',
                      'IsRichEditDrawingDc(hwnd, it->paintDc)',
                      'boundScope(g_editSessionStack) || boundScope(g_editDcStack)'):
            self.assertIn(guard, text_gate)
        resolve = source.split('static HWND ResolveEditSessionHwndForExtTextOut(', 1)[1].split('// Служебные проходы', 1)[0]
        self.assertLess(resolve.index('IsRichEditTextDrawingDc(host, hdc, callerMod)'),
                        resolve.index('HWND s = GetSessionHwnd()'))
        core = source.split('static BOOL Edit_WorkExtTextOutCore(', 1)[1].split('BOOL WINAPI Edit_MyExtTextOutW(\n', 1)[0]
        self.assertIn('IsRichEditTextDrawingDc(sessionHwnd, hdc, callerMod)', core)
        self.assertNotIn('!IsRichEditDrawingDc(sessionHwnd, hdc)', core)
        self.assertIn('L"ExtTextOut", callerMod)', core)
        apply = source.split('static bool ApplyRichEditBackgroundRect(', 1)[1].split('//  OVERALL DISPATCHER MAP', 1)[0]
        self.assertIn('HMODULE nativeTextCaller = nullptr', apply)
        self.assertIn('IsRichEditTextDrawingDc(hwnd, dc, nativeTextCaller) : IsRichEditDrawingDc(hwnd, dc)', apply)
        fills = source.split('BOOL WINAPI Edit_MyFillRect(HDC hdc', 1)[1].split('BOOL WINAPI Edit_MyExtTextOutA(\n', 1)[0]
        self.assertNotIn('IsRichEditTextDrawingDc', fills)
        self.assertNotIn('L"FillRect", callerMod', fills)
        self.assertNotIn('L"PatBlt", callerMod', fills)
        self.assertEqual(source.count('ResolveEditSessionHwndForExtTextOut(hdc, callerMod,'), 2)
        self.assertIn('[GlassCore][RichEdit] TextDc', core)
        self.assertIn('budget = 16', core)

    def test_richedit_hook_refs_release_exact_saved_acquisitions(self):
        source = read('GlassCore/src/Hooks/EditHooks.cpp')
        acquire = source.split('static void AcquireRichEditHooks(', 1)[1].split('// Plain Edit keeps', 1)[0]
        for required in ('L"riched20.dll"', 'L"msftedit.dll"', 'L"gdi32full.dll"',
                         'std::find(modules.begin()', 'acquire(EHK_FILLRECT)',
                         'acquire(EHK_PATBLT)', 'acquire(EHK_EXTTEXTOUT)', 'acquire(EHK_EXTTEXTOUTA)',
                         'session.richHooks.push_back({ mod, index })',
                         'EnableHooksOnModule(it->mod, it->index, false)'):
            self.assertIn(required, acquire)
        self.assertIn('const int before = it->second.GetRef(index)', source)
        self.assertIn('return enable ? after > before : after < before', source)
        for stack in ('g_editSessionStack', 'g_editDcStack'):
            self.assertIn('const EditPaintSession s = std::move(' + stack + '.back())', source)
        self.assertEqual(source.count('if (IsRichEditKind(s.kind)) ReleaseRichEditHooks(s)'), 2)
        self.assertIn('if (!IsRichEditKind(g_editSessionStack.back().kind))', source)

    def test_richedit_runtime_runner_compiles_the_whole_production_unit(self):
        runner = read('WndCompat/tests/richedit_rendering_tests.cpp')
        self.assertIn('#include "../../GlassCore/src/Hooks/EditHooks.cpp"', runner)
        script = read('WndCompat/tests/richedit_rendering_test.py')
        self.assertIn('for debug in (False, True)', script)
        self.assertIn("command.append('-D_DEBUG')", script)
        self.assertNotIn('.read_text(', script)  # no extracted/reimplemented production path

    def test_memo_background_is_composed_separately_from_text(self):
        source = read("GlassCore/src/Hooks/EditHooks.cpp")
        self.assertIn('std::vector<MemoBackgroundOp> memoBackgrounds;', source)
        apply = source.split('static void ApplyEditBackgroundRect(', 1)[1].split('//  RichEdit backgrounds', 1)[0]
        self.assertRegex(apply, r'(?s)PatBlt\(targetDC.*?if \(useSession && g_activeEditBuffer && targetDC == g_activeEditBuffer->memDC\).*?if \(g_activeEditBuffer->delphiMemo\).*?memoBackgrounds.push_back\(\{ r, color, fill \}\);\s*return;')
        self.assertIn('PushSessionBlacknessRect(r,', apply)
        self.assertIn('FillRect(targetDC, &r, brush.get())', apply)
        replay = source.split('static void ReplayMemoBackgrounds(', 1)[1].split('// ── FlushEditBuffer', 1)[0]
        self.assertRegex(replay, r'(?s)for \(const auto& op : buf.memoBackgrounds\).*?PatBlt\(dc.*?if \(op.fill\).*?CreateSolidColorBrushWithAlpha\(op.color, 255\).*?FillRect\(dc')
        flush = source.split('static void FlushEditBuffer()', 1)[1].split('static void ApplyEditBackgroundRect(', 1)[0]
        self.assertRegex(flush, r'(?s)if \(buf->delphiMemo\)\s*\{.*?ReplayMemoBackgrounds\(composeDC, \*buf\);.*?GdiAlphaBlend\(composeDC')
        self.assertRegex(flush, r'(?s)if \(buf->delphiMemo\)\s*\{.*?ReplayMemoBackgrounds\(buf->screenDC, \*buf\);.*?GdiAlphaBlend\(buf->screenDC')
        self.assertIn('memoBackgrounds=%zu', flush)

    def test_memo_solid_background_requires_current_color_confirmation(self):
        root = read('WndCompat/Root/RootSubclass.cpp')
        capture = root.split('const bool validBrush = GetObject(', 1)[1].split('if (validBrush)', 1)[0]
        for guard in ('WM_CTLCOLORSTATIC', 'L"TMemo"', 'validBrush', 'BS_SOLID', 'OPAQUE',
                      'GetBkColor(hdc)', 'COLOR_WINDOW', 'COLOR_BTNFACE'):
            self.assertIn(guard, capture)
        self.assertIn('SetDelphiMemoBackground(memo, rgb)', capture)
        self.assertIn('ClearDelphiMemoBackground(memo)', capture)
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
        self.assertIn('CreateSolidColorBrushWithAlpha(buf.memoSolidBackground, 255)', prime)
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

    def test_panel_background_uses_real_alpha_and_only_two_default_colors(self):
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
        self.assertIn('pixel[3] = kCustomBackgroundAlpha', policy)
        cache = read('GlassCore/src/Hooks/RootWindowHooks.cpp')
        self.assertIn('if (!GlassShared::IsDelphiPanelClassName(cls))', cache)
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

    def test_delphi_grid_clears_repaint_area_after_beginpaint(self):
        root = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        body = root.split('Root_OnDelphiGridAfterBeginPaint(', 1)[1].split('static void EnableSpecs', 1)[0]
        for marker in ('IsRectEmpty(&ps->rcPaint)', 'L"TStringGrid"', 'L"TDrawGrid"',
                       'L"TShownValueList"', 'PatBlt(hdc, rc.left, rc.top', 'return ok != FALSE;'):
            self.assertIn(marker, body)
        self.assertIn('RegisterPostPaintProvider({ Root_OnDelphiGridAfterBeginPaint })', root)

if __name__ == "__main__":
    unittest.main()
