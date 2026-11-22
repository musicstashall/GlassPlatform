#!/usr/bin/env python3
"""Compile the production VCL probe with deterministic environment doubles.

The actual header, full dependency table, complete probe block and root-enable
body are used unchanged. Other root renderers are link-only stubs, not tested
or copied policies. This is not a complete RootGdiHooks.cpp/MSVC/PE-IAT build.
"""
from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract_production(source):
    guard = '#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)'
    start = source.index(guard + '\n        int WINAPI VclProbeFill(')
    end = source.index('\n        // ── EnableRootHook / DisableAllRootHooks', start)
    enable = source.index('        bool EnableRootHook(void* baseAddress,', end)
    prewarm = source.index('        void PrewarmRootHook(', enable)
    production = source[start:end] + '\n' + source[enable:prewarm]
    # These unrelated functions are only addresses in the real dependency table.
    # Their bodies must not be called by any probe test.
    names = sorted(set(re.findall(r'reinterpret_cast<PVOID>\((Root_My\w+)', production)))
    stubs = '\n'.join(
        ('template<size_t> ' if name == 'Root_MyDelphiGroupGradient' else '')
        + 'void ' + name + '() { throw std::runtime_error("unrelated root hook called"); }'
        for name in names
    )
    return production, stubs


class VclDrawProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='vcl-draw-probe-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        (tmp / 'windows.h').write_text('#pragma once\n', encoding='utf-8')
        source = (ROOT / 'GlassCore/src/Hooks/RootGdiHooks.cpp').read_text(encoding='utf-8')
        production, stubs = extract_production(source)
        (tmp / 'vcl_draw_probe_production.inc').write_text(production, encoding='utf-8')
        (tmp / 'vcl_draw_probe_root_stubs.inc').write_text(stubs, encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG']),
            ('ReleaseProbe', ['-O2', '-DGLASS_DELPHI_DRAW_PROBE']),
            ('DebugProbe', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / ('vcl-draw-probe-' + mode)
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', *flags,
                '-I', str(tmp), '-I', str(ROOT / 'GlassCore/Include'),
                str(ROOT / 'WndCompat/tests/vcl_draw_probe_tests.cpp'), '-o', str(binary),
            ], capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise AssertionError(f'{mode} probe build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        result = subprocess.run([str(self.binaries['DebugProbe']), name],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_disabled_without_both_debug_and_probe_defines(self):
        for mode, binary in self.binaries.items():
            if mode == 'DebugProbe':
                continue
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), 'disabled'], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if shutil.which('nm'):
                    symbols = subprocess.check_output(['nm', '-C', str(binary)], text=True)
                    self.assertNotIn('VclProbe', symbols)
                    self.assertNotIn('VclDrawProbe', symbols)

    def test_real_enum_and_dependency_table_order_dlls_and_new_refs(self): self.run_case('slots')
    def test_exact_tpage_and_old_classes_not_tabcontrol_or_panel_prefixes(self): self.run_case('classes')
    def test_begin_end_and_rejected_acquisition_preserve_entry_error(self): self.run_case('session-errors')
    def test_drawtexta_bytes_counts_measurement_flags_and_supplied_dc(self): self.run_case('draw-ansi')
    def test_drawtextexa_mutable_buffer_rect_params_and_native_outputs(self): self.run_case('drawex-ansi')
    def test_exttextouta_flags_uint_count_rect_dx_and_opaque_pointers(self): self.run_case('ext-ansi')
    def test_textouta_and_w_signed_coordinates_counts_and_native_bool(self): self.run_case('direct-text')
    def test_tabbedtextouta_and_w_tabs_origin_and_packed_long_result(self): self.run_case('tabbed')
    def test_all_text_originals_and_matching_native_fallback_once(self): self.run_case('original-fallback')
    def test_existing_graphics_probes_keep_native_returns_and_errors(self): self.run_case('legacy')
    def test_nested_paint_getdc_same_hwnd_and_non_top_matching_release(self): self.run_case('nested')
    def test_missing_slots_registration_failure_and_inherited_ref_delta(self): self.run_case('partial')
    def test_rate_limit_never_suppresses_native_calls_and_resets(self): self.run_case('budget')
    def test_shared_import_does_not_share_thread_local_probe_context(self): self.run_case('thread')
    def test_native_failure_and_unset_native_last_error_are_not_replaced(self): self.run_case('native-failures')
    def test_shutdown_and_empty_tls_remain_native_without_api_logs(self): self.run_case('shutdown')
    def test_no_text_capture_no_dc_or_pixel_mutation_and_text_state_diagnostics(self): self.run_case('privacy-state')
    def test_first_active_import_occupant_is_not_bypassed(self): self.run_case('occupant')


if __name__ == '__main__':
    unittest.main()
