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

    def test_containers_use_guarded_root_attach(self):
        source = read("WndCompat/Tracker/WinEventHandler.cpp")
        self.assertRegex(source, r'(?s)case ControlKind::DelphiContainer:\s*case ControlKind::GenericContainer:.*?if \(isFull && allowGlassForHwnd\).*?DispatchRootAttachToOwnerThread\(hWnd\);')

    def test_theme_text_is_session_scoped_and_balanced(self):
        root = read("GlassCore/src/Hooks/RootWindowHooks.cpp")
        gdi = read("GlassCore/src/Hooks/RootGdiHooks.cpp")
        header = read("GlassCore/Include/Hooks/RootGdiHooks.h")
        self.assertIn("OHK_DELPHI_DRAWTHEMETEXTEX,\n        OHK_COUNT", header)
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
        attach = root.split("void Root_Attach(HWND hRoot)", 1)[1].split("void Root_Detach(HWND hRoot)", 1)[0]
        self.assertRegex(attach, r'(?s)if \(_wcsicmp\(rootClassName, L"TApplication"\) == 0\).*?return;.*?SetWindowSubclass')

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

    def test_tab_brush_is_not_restored_to_another_class(self):
        source = read("WndCompat/Controls/TabControlSubclass.cpp")
        self.assertIn('_wcsicmp(className, L"TPageControl") != 0', source)
        self.assertGreaterEqual(source.count('_wcsicmp(g_brushClass, className) == 0'), 2)

if __name__ == "__main__":
    unittest.main()
