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

    def test_tab_brush_is_not_restored_to_another_class(self):
        source = read("WndCompat/Controls/TabControlSubclass.cpp")
        self.assertIn('_wcsicmp(className, L"TPageControl") != 0', source)
        self.assertGreaterEqual(source.count('_wcsicmp(g_brushClass, className) == 0'), 2)

if __name__ == "__main__":
    unittest.main()
