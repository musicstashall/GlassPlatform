#!/usr/bin/env python3
"""Run the complete production RestoratorHexDump unit with Win32 doubles.

Optimized and Debug/probe configurations exercise what reaches the surface for
every ANSI row call (background, glyph string, position, advances), the tab
layout, the class/session/caller/DC gates and the two-function Detours lease.
This is NOT Windows GDI/DWM rasterization, the shared text renderer, user32's
own tab expansion, real export patching, the Detours ABI or a Windows/MSVC
build acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RestoratorHexDumpRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='restorator-hexdump-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'windows.h', 'wil.h', 'DebugLog.h', 'GlassBackdropMark.h', 'WinApiUtils.h',
            'Subsystems/PaintDispatcher.h', 'Subsystems/RenderText.h', 'Subsystems/RootSwcaGradient.h',
            'Module/ModuleHandler.h', 'Core/Intercept/HookHelper.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / f'restorator-hexdump-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', '-pthread', *flags,
                '-I', str(stubs), '-I', str(ROOT / 'GlassCore/Include'),
                str(ROOT / 'WndCompat/tests/restorator_hexdump_rendering_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise AssertionError(f'{mode} Restorator hex dump test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_address_bytes_and_characters_of_a_row_are_drawn_at_the_callers_positions(self):
        self.run_case('rows')

    def test_tab_stops_become_glyph_advances_and_the_extent_user32_returns(self):
        self.run_case('tabs')

    def test_other_tab_layouts_alignments_fonts_and_failures_are_one_native_call(self):
        self.run_case('tab-gates')

    def test_opaque_background_is_cleared_for_system_colours_and_opaque_for_others(self):
        self.run_case('background')

    def test_ansi_bytes_are_decoded_with_the_code_page_of_the_fonts_charset(self):
        self.run_case('ansi')

    def test_exact_class_exe_caller_instance_mode_and_top_session_only(self):
        self.run_case('scope')

    def test_other_dc_caller_thread_session_mapping_and_reentry_are_native(self):
        self.run_case('isolation')

    def test_glyph_index_paired_offsets_and_current_position_are_native(self):
        self.run_case('options')

    def test_two_detours_are_leased_in_one_transaction_and_survive_failures(self):
        self.run_case('leases')

    def test_native_results_and_last_error_are_returned_once(self):
        self.run_case('failures')

    def test_metadata_only_rate_limited_reasons(self):
        self.run_case('diagnostics')


if __name__ == '__main__':
    unittest.main()
