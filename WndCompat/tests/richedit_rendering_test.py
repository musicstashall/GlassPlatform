#!/usr/bin/env python3
"""Execute the complete production EditHooks unit with portable Win32 doubles.

Checks actual DIB bytes, native W/A/background routes, DC isolation, leases,
reentry and plain-Edit/TMemo regression. This is not a Windows/MSVC build or
pixel-level validation of the native RichEdit engine. No production logic is
copied/extracted; both release and _DEBUG versions of EditHooks.cpp are compiled.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RichEditRenderingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='richedit-rendering-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in (
            'CorePch.h', 'windows.h', 'Richedit.h', 'wil.h', 'WinApiUtils.h',
            'GlassControlKind.h', 'GlassDelphiMemoBackground.h', 'GlassBackdropMark.h',
            'Core/Intercept/HookDispatcher.h', 'Module/ModuleHandler.h',
            'Subsystems/PaintDispatcher.h', 'Subsystems/RenderText.h',
            'Subsystems/TextInkClientRegistry.h', 'Subsystems/RootSwcaGradient.h',
            'Hooks/delphi/TInputHooks.h',
        ):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = []
        for debug in (False, True):
            binary = tmp / ('richedit-debug' if debug else 'richedit-release')
            command = compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                # Existing preview helpers/debug-only parameters in the full
                # unit are unused in release; keep every other warning fatal.
                '-Wno-unknown-pragmas', '-Wno-unused-parameter', '-Wno-unused-function',
                '-I', str(stubs),
                '-I', str(ROOT / 'GlassCore/Include'),
                str(ROOT / 'WndCompat/tests/richedit_rendering_tests.cpp'), '-o', str(binary),
            ]
            if debug:
                command.append('-D_DEBUG')
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(f'RichEdit C++ build (debug={debug}) failed:\n{result.stdout}{result.stderr}')
            cls.binaries.append(binary)

    def run_case(self, name):
        for binary in self.binaries:
            with self.subTest(build=binary.name):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_all_four_rich_kinds_and_trichedit_share_native_background_apis(self):
        self.run_case('all-kinds-and-apis')

    def test_custom_alpha100_system_transparency_and_opaque_selection(self):
        self.run_case('colors-and-selection')

    def test_empty_client_solid_dc_brush_and_system_pseudo_brush_sources(self):
        self.run_case('empty-and-brush-sources')

    def test_w_a_glyph_options_text_alpha_and_native_failure_fallbacks(self):
        self.run_case('text-options-and-fallbacks')

    def test_native_rectangles_clips_and_repaints_do_not_accumulate_or_reprime(self):
        self.run_case('rectangles-and-repaints')

    def test_exact_bound_dc_memory_ownership_mode_and_backdrop_gates(self):
        self.run_case('dc-and-mode-gates')

    def test_nested_refs_missing_slots_saved_modules_and_failed_beginpaint(self):
        self.run_case('leases-and-nesting')

    def test_getdc_selection_path_and_nested_rich_paint_leave_outer_memo_intact(self):
        self.run_case('getdc-and-memo-isolation')

    def test_every_gdi_failure_cleans_up_and_uses_the_callers_original(self):
        self.run_case('failures-and-originals')

    def test_compositor_reentry_and_unchanged_alpha255_plain_edit_memo(self):
        self.run_case('reentry-and-legacy-edit')


if __name__ == '__main__':
    unittest.main()
