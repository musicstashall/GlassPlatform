#!/usr/bin/env python3
"""Compile production Bome drawing + actual class gate, root slots and wrappers.

Checks API inputs, ANSI conversion, DC/bitmap gates and native fallbacks with
Win32/IAT doubles. Does not verify real GDI/DDB alpha, DWM pixels or MSVC x86 ABI.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BomeMemoRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='bome-memo-')
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
        gate = root.split('// Custom-painted TBomeMemo/TBMGutter:', 1)[1].split(
            '// VCL paints windowless labels', 1)[0]
        gate = gate[gate.index('            if ('):]
        gdi = (ROOT / 'GlassCore/src/Hooks/RootGdiHooks.cpp').read_text(encoding='utf-8')
        wrappers = gdi.split('// Bome memo EXE wrappers:', 1)[1].split(
            '        int WINAPI Root_MyDrawTextW(', 1)[0]
        wrappers = wrappers[wrappers.index('        int WINAPI Root_MyBomeMemoText('):]
        rows = gdi.split('// Bome memo ANSI paint slots', 1)[1].split(
            '// Custom VCL group paint slots.', 1)[0]
        rows = rows[rows.index('            std::tuple{'):]
        classifier = (ROOT / 'WndCompat/Tracker/WindowClassifier.cpp').read_text(encoding='utf-8')
        classifier = classifier.split('// ── Delphi-контейнеры', 1)[1].split(
            '// ── Прочие WS_CHILD', 1)[0]
        classifier = classifier[classifier.index('    if ('):]
        (stubs / 'root_bome_memo_test.inc').write_text(
            '#include <GlassDelphiPanel.h>\n#include <GlassDelphiButtonGroups.h>\n'
            'namespace BomeClassifier {\n'
            'enum class ControlKind { Unknown, DelphiContainer };\n'
            'inline bool Equals(const std::wstring& a, LPCWSTR b) { return _wcsicmp(a.c_str(),b) == 0; }\n'
            'inline ControlKind KindForTest(const std::wstring& cls) {\n' + classifier
            + '\nreturn ControlKind::Unknown;\n}\n}\n'
            'namespace GlassCore::Root {\nusing namespace std::literals;\n'
            'inline RootHookCache BomeSpecsForTest(HMODULE instMod, LPCWSTR cls) {\n'
            'RootHookCache cache;\n' + gate + '\nreturn cache;\n}\n' + wrappers
            + '\ninline const GlassCore::Hooks::HookDispatcherDependency<2,3> BomeDependenciesForTest{{\n'
            + rows + '\n}};\n}\n', encoding='utf-8')
        cls.binaries = []
        for config, flags in (('release', ['-O2']), ('debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE'])):
            binary = tmp / ('bome-tests-' + config)
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', *flags, '-I', str(stubs),
                '-I', str(ROOT / 'GlassCore/Include'), '-I', str(ROOT / 'GlassShared/include'),
                str(ROOT / 'WndCompat/tests/bome_memo_rendering_tests.cpp'), '-o', str(binary),
            ], capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(f'{config} Bome C++ test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries.append((config, binary))

    def run_case(self, name):
        for config, binary in self.binaries:
            with self.subTest(config=config):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_classes_container_route_and_paint_only_exe_slots(self):
        self.run_case('classes')

    def test_observed_xml_rows_in_memory_and_direct_line_number_gutter(self):
        self.run_case('captured-rows')

    def test_ansi_measurement_mutation_empty_and_invalid_input_stay_native(self):
        self.run_case('native-semantics')

    def test_selected_charset_acp_explicit_dbcs_counts_and_conversion_failure(self):
        self.run_case('ansi')

    def test_only_current_system_defaults_clear_custom_black_and_selection_stay_native(self):
        self.run_case('backgrounds')

    def test_paint_dc_known_32bpp_memory_foreign_nc_mode_layout_and_backdrop(self):
        self.run_case('dc-gates')

    def test_clear_renderer_conversion_failure_and_reentry_keep_fallback_and_state(self):
        self.run_case('failures')

    def test_entry_point_slot_originals_missing_original_and_shutdown(self):
        self.run_case('originals')


if __name__ == '__main__':
    unittest.main()
