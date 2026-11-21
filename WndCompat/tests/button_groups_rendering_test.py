#!/usr/bin/env python3
"""Compile production group drawing + actual root gate, dependency rows and thunks.

Checks exact routing, preserved native arguments, alpha composition parameters,
fallbacks and isolation with Win32/IAT doubles. Not Windows pixels or an MSVC build.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ButtonGroupsRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='button-groups-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in ('CorePch.h', 'windows.h', 'uxtheme.h', 'wil.h', 'DebugLog.h',
                       'Subsystems/PaintDispatcher.h', 'Subsystems/RenderText.h',
                       'Subsystems/RootSwcaGradient.h', 'GlassBackdropMark.h'):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        root = (ROOT / 'GlassCore/src/Hooks/RootWindowHooks.cpp').read_text(encoding='utf-8')
        gate = root.split('// Custom-painted VCL groups:', 1)[1].split('// VCL-таблицы', 1)[0]
        gate = gate[gate.index('            if ('):]
        gdi = (ROOT / 'GlassCore/src/Hooks/RootGdiHooks.cpp').read_text(encoding='utf-8')
        thunks = gdi.split('// Dedicated EXE slots:', 1)[1].split('        BOOL WINAPI Root_MyExtTextOutW(', 1)[0]
        thunks = thunks[thunks.index('        int WINAPI Root_MyDelphiGroupText('):]
        row = gdi.index('"DrawTextExW", reinterpret_cast<PVOID>(Root_MyDelphiGroupText)')
        dep = gdi[gdi.rfind('            std::tuple{', 0, row):].split('#if defined(_DEBUG)', 1)[0]
        (stubs / 'root_button_groups_test.inc').write_text(
            'namespace GlassCore::Root {\nusing namespace std::literals;\n'
            'inline RootHookCache GroupSpecsForTest(HMODULE instMod, LPCWSTR cls) {\n'
            'RootHookCache cache;\n' + gate + '\nreturn cache;\n}\n' + thunks + '\n'
            'inline const GlassCore::Hooks::HookDispatcherDependency<4,3> GroupDependenciesForTest{{\n'
            + dep + '\n}};\n}\n', encoding='utf-8')
        cls.binaries = []
        for config, flags in (('release', ['-O2']), ('debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE'])):
            binary = tmp / ('group-tests-' + config)
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', *flags, '-I', str(stubs),
                '-I', str(ROOT / 'GlassCore/Include'), '-I', str(ROOT / 'GlassShared/include'),
                str(ROOT / 'WndCompat/tests/button_groups_rendering_tests.cpp'), '-o', str(binary),
            ], capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(f'{config} group C++ test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries.append((config, binary))

    def run_case(self, name):
        for config, binary in self.binaries:
            with self.subTest(config=config):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_actual_root_slots_exact_classes_and_exe_gate(self):
        self.run_case('class-gate')

    def test_probe_text_flags_rects_font_and_native_measurement(self):
        self.run_case('probe-text')

    def test_system_and_custom_solid_fills_and_nonaccumulating_repaints(self):
        self.run_case('fills')

    def test_both_gradient_imports_keep_native_mesh_rgb_and_black_endpoint(self):
        self.run_case('gradients')

    def test_known_dc_owner_current_session_mode_and_backdrop(self):
        self.run_case('dc-isolation')

    def test_unsupported_mesh_chrome_pattern_brush_and_geometry_stay_native(self):
        self.run_case('unsupported')

    def test_failure_discards_buffer_before_clear_and_reentry_stays_native(self):
        self.run_case('failures')

    def test_slot_originals_are_not_inferred_from_return_address(self):
        self.run_case('slot-originals')


if __name__ == '__main__':
    unittest.main()
