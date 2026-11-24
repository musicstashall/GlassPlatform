#!/usr/bin/env python3
"""Run the production WndCompat/Root/DelphiShowErase.h with Win32 doubles.

Both configurations exercise what a VCL top-level window does with its surface
when it is shown again: on its first frame or erase after the show it lays the
transparent base under every point whose owner - the topmost visible window
there - is about to repaint it, and nowhere else. The scenarios cover the form
of LogViewer-25, windows that keep their picture, windows that are left alone,
the gates, every refusal of the system, the Debug line, and four hundred
generated layouts compared point by point with an independent description of
who owns a point.
This is NOT Windows: no window procedure runs, nothing is composed, a region
is a set of points, the moment Windows sends WM_NCPAINT and WM_ERASEBKGND is
the test's own, and it is not an MSVC build acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ShowEraseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='show-erase-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in ('windows.h', 'DebugLog.h', 'GlassBackdropMark.h', 'InjectionMode.h', 'Tracker/WinEventHandler.h'):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG']),
        ):
            binary = tmp / f'show-erase-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                # MapWindowPoints takes a RECT as two POINTs, as its documentation shows;
                # MSVC does not optimise on the types of the two, and neither may this build.
                '-Wno-unknown-pragmas', '-fno-strict-aliasing', *flags, '-I', str(stubs),
                str(ROOT / 'WndCompat/tests/show_erase_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise AssertionError(f'{mode} show erase test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_shown_form_is_cleared_once_under_every_control_that_waits_for_a_paint(self):
        self.run_case('restored')

    def test_window_with_nothing_pending_keeps_its_picture(self):
        self.run_case('kept')

    def test_other_threads_mirrored_layered_and_deep_windows_are_left_alone(self):
        self.run_case('others')

    def test_only_a_vcl_top_level_window_with_glass_and_only_the_paint_of_its_show(self):
        self.run_case('gates')

    def test_whatever_refuses_nothing_is_cleared_and_nothing_is_left_behind(self):
        self.run_case('failures')

    def test_debug_build_writes_one_line_and_a_release_build_none(self):
        self.run_case('line')

    def test_generated_layouts_match_the_owner_of_every_point(self):
        self.run_case('random')


if __name__ == '__main__':
    unittest.main()
