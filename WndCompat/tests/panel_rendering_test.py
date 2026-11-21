#!/usr/bin/env python3
"""Execute production VCL panel hooks/message bridge with Win32/IAT doubles.

The root themed-text gate and IAT handler are also compiled directly from
production source excerpts. Checks routing, DIB bytes, options and scoped state,
not Windows pixels, real IAT chains, uxtheme alpha, or MSVC build compatibility.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PanelRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='panel-rendering-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'WndPch.h', 'windows.h', 'uxtheme.h', 'wil.h', 'DebugLog.h',
            'GlassBackdropMark.h', 'InjectionMode.h', 'WndCompat.h',
            'Tracker/WinEventHandler.h', 'Core/Intercept/HookDispatcher.h',
            'Module/ModuleHandler.h', 'Subsystems/PaintDispatcher.h',
            'Subsystems/RootSwcaGradient.h', 'Subsystems/RenderText.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        # Compile the real root gate and handler without dragging the unrelated
        # MMC/Explorer hooks into the Win32 double. Do not duplicate their logic.
        root = (ROOT / 'GlassCore/src/Hooks/RootWindowHooks.cpp').read_text(encoding='utf-8')
        gate = root.split('// VCL paints windowless labels and group captions', 1)[1]
        gate = gate[gate.index('            if ('):].split('// VCL-таблицы', 1)[0]
        gdi = (ROOT / 'GlassCore/src/Hooks/RootGdiHooks.cpp').read_text(encoding='utf-8')
        handler = '        HRESULT WINAPI Root_MyDelphiDrawThemeTextEx(\n'
        handler += gdi.split(handler, 1)[1].split('        BOOL WINAPI Root_MyExtTextOutW(', 1)[0]
        (stubs / 'root_panel_theme_test.inc').write_text(
            'namespace GlassCore::Root {\n'
            'inline RootHookCache PanelTextSpecsForTest(HMODULE instMod, LPCWSTR cls) {\n'
            'RootHookCache cache;\n' + gate + '\nreturn cache;\n}\n' + handler + '\n}\n',
            encoding='utf-8')
        cls.binary = tmp / 'panel-tests'
        result = subprocess.run(compiler + [
            '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
            '-Wno-unknown-pragmas', '-I', str(stubs),
            '-I', str(ROOT / 'GlassCore/Include'), '-I', str(ROOT / 'WndCompat'),
            '-I', str(ROOT / 'GlassShared/include'),
            str(ROOT / 'WndCompat/tests/panel_rendering_tests.cpp'), '-o', str(cls.binary),
        ], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(f'Panel C++ test build failed:\n{result.stdout}{result.stderr}')

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_classes_and_native_erase_reach_color_brush(self):
        self.run_case('classes-and-erase')

    def test_custom_alpha_100_system_black_and_nonaccumulating_repaints(self):
        self.run_case('alpha-colors')

    def test_native_brush_apis_and_preserved_chrome(self):
        self.run_case('apis-and-chrome')

    def test_dc_ownership_and_native_printclient(self):
        self.run_case('dc-and-print')

    def test_mode_backdrop_missing_bridge_and_balanced_sessions(self):
        self.run_case('gates-and-sessions')

    def test_gdi_failure_cleanup_and_reentry_passthrough(self):
        self.run_case('failures-and-reentry')

    def test_actual_root_class_gate_and_handler_keep_the_exe_original(self):
        self.run_case('root-themed-route')

    def test_themed_probe_captions_use_transparent_compositor_and_keep_dttopts(self):
        self.run_case('probe-themed-captions')

    def test_themed_dc_isolation_print_and_existing_vcl_containers(self):
        self.run_case('themed-dc-and-legacy')

    def test_themed_measurement_callback_failure_and_reentry_are_native(self):
        self.run_case('themed-native-semantics')


if __name__ == '__main__':
    unittest.main()
