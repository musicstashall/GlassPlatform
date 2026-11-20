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
        self.assertIn('IntersectRect(&overlap, rect, &session.rect)', theme)
        self.assertIn('IntersectRect(&drawClip, &session.rect, clip)', theme)
        self.assertIn('IntersectClipRect(dc, drawClip.left', theme)
        self.assertIn('RestoreDC(dc, saved)', theme)
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
        self.assertIn('!g_editSessionStack.empty() && IsDelphiMemo(sessionHwnd)', core)
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
        for name in ('OHK_DELPHI_GRID_TEXT', 'OHK_DELPHI_GRID_FILL', 'OHK_DELPHI_GRID_THEME'):
            self.assertIn('PushPaint(instMod, ' + name, block)
            self.assertNotIn('PushDc(instMod, ' + name, block)
        gdi = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        grid = gdi.split('static bool IsDelphiGridPaintDC', 1)[1].split('int WINAPI Root_MyDrawTextW(', 1)[0]
        for guard in ('WindowFromDC(dc)', 'GetPaintSessionHostHwnd()', 'IsForeignPaintBarrierActive()',
                      'L"TShownValueList"', 'TA_UPDATECP', 'ETO_GLYPH_INDEX', 'ETO_PDY',
                      'lb.lbStyle == BS_SOLID', 'RGB(255, 255, 255)', 'L"Header"',
                      'ExtTextOutWithAlpha', 'return orig('):
            self.assertIn(guard, grid)
        self.assertIn('options == ETO_CLIPPED && rect && rect->top == 0', grid)
        self.assertIn('HeaderTextClear', grid)
        self.assertIn('[GlassCore][DelphiMemo] DirectGetDC', read("GlassCore/src/Hooks/EditHooks.cpp"))

    def test_targeted_vcl_edit_probes_and_combo_ctlcolor(self):
        combo = read("WndCompat/Popup/ComboLBox/ComboBoxSubclass.cpp")
        ctl = combo.split('case WM_CTLCOLOREDIT:', 1)[1].split('case WM_LBUTTONDOWN:', 1)[0]
        for guard in ('Shared::InjectionMode::Full', 'GlassIsBackdropAllowedForHwnd(hWnd)',
                      'L"TComboBox"', 'info.hwndItem != edit', 'L"Edit"',
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

    def test_tab_brush_is_not_restored_to_another_class(self):
        source = read("WndCompat/Controls/TabControlSubclass.cpp")
        self.assertIn('_wcsicmp(className, L"TPageControl") != 0', source)
        self.assertGreaterEqual(source.count('_wcsicmp(g_brushClass, className) == 0'), 2)

if __name__ == "__main__":
    unittest.main()
