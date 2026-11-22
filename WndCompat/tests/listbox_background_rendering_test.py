#!/usr/bin/env python3
"""Compile complete production TListBoxDraw.cpp in Release and Debug/probe.

Check background brush alpha, native arguments, session/DC isolation and refs.
These are environment doubles, not MSVC/Win32 GDI/DWM or PE/IAT acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ListBoxBackgroundRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='listbox-background-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'windows.h', 'wil.h', 'wil/resource.h', 'uxtheme.h', 'vssym32.h',
            'WinApiUtils.h', 'DebugLog.h', 'GlassControlKind.h', 'GlassBackdropMark.h',
            'Subsystems/RenderText.h', 'Subsystems/Settings.h', 'Subsystems/GdiHookUtils.h',
            'Subsystems/ThemePaintRegistry.h', 'Subsystems/RootSwcaGradient.h',
            'Core/Intercept/HookDispatcher.h', 'Module/ModuleHandler.h', 'Helpers/ThemeHelper.h',
            'Helpers/ColorUtils.h', 'BitmapUtils.h', 'Providers/MenuContext.h',
            'Hooks/delphi/TBackgroundHooks.h', 'Hooks/UxThemeHooks.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG', '-DGLASS_DELPHI_DRAW_PROBE']),
        ):
            binary = tmp / f'listbox-background-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                '-Wno-unknown-pragmas', '-pthread', *flags, '-I', str(stubs),
                '-I', str(ROOT / 'GlassCore/Include'),
                str(ROOT / 'WndCompat/tests/listbox_background_rendering_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise AssertionError(f'{mode} list background build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_custom_full_row_and_opaque_text_background_are_a100(self): self.run_case('custom')
    def test_system_backgrounds_and_selection_keep_previous_policy(self): self.run_case('defaults-selection')
    def test_confirmed_black_background_and_unknown_mapped_black_are_distinct(self): self.run_case('black')
    def test_small_graphics_cross_row_full_client_and_partial_clip(self): self.run_case('geometry')
    def test_exact_tlistbox_only_other_lists_and_combo_fields_are_unchanged(self): self.run_case('classes')
    def test_text_flags_counts_dx_and_native_fallback_are_preserved(self): self.run_case('text')
    def test_drawtext_composites_glyphs_without_erasing_row_or_selection(self): self.run_case('drawtext')
    def test_full_backdrop_kind_parent_dc_thread_and_session_isolation(self): self.run_case('isolation')
    def test_nested_classes_and_ref_delta_release(self): self.run_case('nesting')
    def test_pattern_brush_and_gdi_failure_passthrough(self): self.run_case('failures')
    def test_missing_slot_shutdown_and_captured_original(self): self.run_case('lifecycle')
    def test_reentry_stack_growth_and_exact_supplied_memory_dc(self): self.run_case('reentry-memory')
    def test_native_graphics_and_text_measurement_do_not_get_background_alpha(self): self.run_case('graphics')


if __name__ == '__main__':
    unittest.main()
