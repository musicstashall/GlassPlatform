#!/usr/bin/env python3
"""Run the production WndCompat/Root/DelphiRestoreProbe.h with Win32 doubles.

The Debug configuration exercises the probe a VCL top-level window carries
through a show or a restore: when it is armed, the three moments at which it
reads the surface under every visible child (before the erase Windows runs on
its own, after it, after the paint), what a line holds, its limits and what it
does when the queue or a DC refuses. The Release configuration checks that
there is no probe at all.
This is NOT Windows: no window procedure runs, nothing is composed, the order
of posted messages, WM_PAINT and WM_TIMER is the test's own, and it is not an
MSVC build acceptance.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RestoreProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest('A GCC-compatible C++20 compiler is required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='restore-probe-')
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        stubs = tmp / 'include'
        for header in ('windows.h', 'DebugLog.h', 'GlassControlKind.h'):
            path = stubs / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#pragma once\n', encoding='utf-8')
        cls.binaries = {}
        for mode, flags in (
            ('Release', ['-O2']),
            ('Debug', ['-O0', '-g', '-D_DEBUG']),
        ):
            binary = tmp / f'restore-probe-{mode}'
            result = subprocess.run(compiler + [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                # MapWindowPoints takes a RECT as two POINTs, as its documentation shows;
                # MSVC does not optimise on the types of the two, and neither may this build.
                '-Wno-unknown-pragmas', '-fno-strict-aliasing', *flags, '-I', str(stubs),
                str(ROOT / 'WndCompat/tests/restore_probe_tests.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, timeout=180)
            if result.returncode:
                raise AssertionError(f'{mode} restore probe test build failed:\n{result.stdout}{result.stderr}')
            cls.binaries[mode] = binary

    def run_case(self, name):
        for mode, binary in self.binaries.items():
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), name], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_surface_is_read_before_the_erase_after_it_and_after_the_paint(self):
        self.run_case('sequence')

    def test_window_that_leaves_the_taskbar_is_probed_like_one_that_is_shown(self):
        self.run_case('restore')

    def test_only_a_vcl_top_level_window_and_only_its_own_messages(self):
        self.run_case('gates')

    def test_every_visible_child_with_an_area_is_listed_within_a_budget(self):
        self.run_case('children')

    def test_one_probe_in_three_seconds_and_a_refused_step_ends_it(self):
        self.run_case('limits')

    def test_release_build_has_no_probe(self):
        self.run_case('absent')


if __name__ == '__main__':
    unittest.main()
