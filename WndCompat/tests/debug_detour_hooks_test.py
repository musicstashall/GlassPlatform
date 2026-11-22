#!/usr/bin/env python3
"""Compile and execute the complete actual DebugDetourHooks header and cpp.

No copied DDH implementation or RootGdiHooks/DelphiDrawProbe substitute. Win32,
module/stack metadata, logging and transactional Detours are environment doubles.
Both Debug optimization levels execute the real 40-slot switches/refcounts and
six new passthrough handlers; Release executes the public inline no-op. This is
not Windows/MSVC ABI, real executable detouring, GDI/DWM or concurrency proof.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DebugDetourHooksTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='debug-detour-hooks-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'Windows.h', 'DebugLog.h', 'detours.h', 'intrin.h',
            'dwmapi.h', 'wil/resource.h', 'Helpers/ThemeHelper.h',
            'Core/Intercept/HookHelper.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Debug', ['-O0', '-g', '-D_DEBUG']),
            ('OptimizedDebug', ['-O2', '-D_DEBUG']),
            ('Release', ['-O2']),
        ):
            binary = tmp / ('ddh-tests-' + mode)
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', '-pthread', *flags, '-I', str(stubs),
                '-I', str(ROOT / 'GlassCore/Include'),
                str(ROOT / 'WndCompat/tests/debug_detour_hooks_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(f'DDH {mode} C++ test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode in ('Debug', 'OptimizedDebug'):
            with self.subTest(mode=mode):
                result = subprocess.run([str(self.binaries[mode]), name], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_all_40_public_bits_actual_originals_and_both_detour_switches(self):
        self.run_case('slots')

    def test_nested_refs_mixed_old_w_and_new_a_masks_and_final_detach(self):
        self.run_case('refs')

    def test_begin_mid_attach_mid_detach_and_commit_failure_roll_back_actual_refs(self):
        self.run_case('transactions')

    def test_each_missing_original_aborts_group_without_phantom_enabled_slots(self):
        self.run_case('missing-original')

    def test_actual_drawtexta_passes_counts_flags_rect_pointer_result_dc_and_last_error(self):
        self.run_case('api0')

    def test_actual_drawtextexa_also_preserves_mutable_string_and_params_pointer(self):
        self.run_case('api1')

    def test_actual_exttextouta_preserves_unsigned_counts_options_and_dx_pointer(self):
        self.run_case('api2')

    def test_actual_textouta_preserves_coordinates_signed_counts_and_false_result(self):
        self.run_case('api3')

    def test_actual_tabbedtextouta_preserves_tab_count_array_origin_and_packed_result(self):
        self.run_case('api4')

    def test_actual_tabbedtextoutw_preserves_wide_pointer_without_preview(self):
        self.run_case('api5')

    def test_unreadable_text_rect_dx_tabs_and_params_are_not_read_even_with_minus_one(self):
        self.run_case('no-reads')

    def test_measurement_and_modifystring_mutations_are_native_only_and_occur_once(self):
        self.run_case('mutations')

    def test_metadata_has_caller_hdc_available_window_class_counts_flags_and_colors_not_text(self):
        self.run_case('metadata')

    def test_reentrant_logger_does_not_recurse_or_duplicate_native_calls(self):
        self.run_case('reentry')

    def test_other_thread_metadata_is_not_hidden_by_the_current_thread_logger_guard(self):
        self.run_case('thread-logger')

    def test_logger_exception_cannot_suppress_native_or_stick_the_guard(self):
        self.run_case('logger-failure')

    def test_own_module_is_silent_and_missing_caller_or_window_remains_native(self):
        self.run_case('caller')

    def test_release_actual_header_is_noop_and_does_not_install_or_log(self):
        result = subprocess.run([str(self.binaries['Release']), 'release'], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
