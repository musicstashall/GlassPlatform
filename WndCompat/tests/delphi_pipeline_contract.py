"""Portable source-contract checks; Windows paint/lifecycle tests are still required."""
from pathlib import Path
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
        self.assertIn('enum : size_t { FillSlot, ExtTextSlot, DrawTextSlot, RectangleSlot, Count }', code)
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
        root = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        gdi = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        header = read("GlassCore/Include/Hooks/RootGdiHooks.h")
        self.assertIn("OHK_DELPHI_DRAWTHEMETEXTEX,", header)
        self.assertIn('"DrawThemeTextEx", reinterpret_cast<PVOID>(Root_MyDelphiDrawThemeTextEx)', gdi)
        self.assertIn("PushPaint(instMod, OHK_DELPHI_DRAWTHEMETEXTEX, cache)", root)
        self.assertIn("PushDc(instMod, OHK_DELPHI_DRAWTHEMETEXTEX, cache)", root)
        for guard in ("hwnd != owner", "DT_CALCRECT", "DTT_CALCRECT | DTT_CALLBACK", "DTT_COMPOSITED"):
            self.assertIn(guard, gdi)
        for cls in ("TGroupBox", "TTabSheet", "TPanel", "TScrollBox"):
            self.assertIn('L"' + cls + '"', root)
            self.assertIn('L"' + cls + '"', gdi)

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

    def test_memo_background_is_composed_separately_from_text(self):
        source = read("GlassCore/src/Hooks/EditHooks.cpp")
        self.assertIn('std::vector<MemoBackgroundOp> memoBackgrounds;', source)
        apply = source.split('static void ApplyEditBackgroundRect(', 1)[1].split('static COLORREF GetRichEdit20BackgroundFromEm', 1)[0]
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
        for grid_cls in ('L"TStringGrid"', 'L"TDrawGrid"'):
            self.assertIn(grid_cls, block + root.split('StrEqI(cls, L"TShownValueList")', 1)[0][-400:])
        for name in ('OHK_DELPHI_GRID_TEXT', 'OHK_DELPHI_GRID_FILL', 'OHK_DELPHI_GRID_THEME'):
            self.assertIn('PushPaint(instMod, ' + name, block)
            self.assertNotIn('PushDc(instMod, ' + name, block)
        gdi = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        grid = gdi.split('static bool IsDelphiGridClass', 1)[1].split('int WINAPI Root_MyDrawTextW(', 1)[0]
        for guard in ('WindowFromDC(dc)', 'GetPaintSessionHostHwnd()', 'IsForeignPaintBarrierActive()',
                      'L"TShownValueList"', 'L"TStringGrid"', 'L"TDrawGrid"',
                      'TA_UPDATECP', 'ETO_GLYPH_INDEX', 'ETO_PDY',
                      'lb.lbStyle == BS_SOLID', 'RGB(255, 255, 255)', 'L"Header"',
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
                      'if (s.combo && i != FillSlot && i != ExtTextSlot && i != DrawTextSlot) continue;', 's.field = comboField'):
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

    def test_delphi_grid_background_has_no_alpha_brushes(self):
        wc = read("WndCompat/Tracker/WindowClassifier.cpp")
        edit_block = wc.split('Equals(cls, L"TInplaceEditList")', 1)[0][-200:]
        self.assertIn('Equals(cls, L"TInplaceEdit")', edit_block)
        root = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        brush = root.split('H BRUSH WINAPI Root_MyCreateSolidBrush'.replace('H B', 'HB'), 1)[1]
        brush = brush.split('// BWM: COMDLG32', 1)[0]
        for marker in ('IsDelphiGridClass(hostCls)',
                       'plainGrid == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF)',
                       'return o(RGB(0, 0, 0))', 'return o(color)'):
            self.assertIn(marker, brush)
        self.assertNotIn('CreateSolidColorBrushWithAlpha', brush)
        fill = root.split('int WINAPI Root_MyDelphiGridFill(HDC dc', 1)[1].split('HRESULT WINAPI Root_MyDelphiGridTheme', 1)[0]
        for marker in ('[GlassCore][DelphiGrid] Fill', 'color=0x%08X', 'brush=%d'):
            self.assertIn(marker, fill)
        self.assertIn('count=%u rect=[', root)

if __name__ == "__main__":
    unittest.main()
