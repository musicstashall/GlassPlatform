#!/usr/bin/env python3
"""Run every portable *_mock.py, retaining failures and returning nonzero.
Requires Python 3 and a C++17 compiler (CXX, g++/clang++), not Windows.
Does not run browser/DOM tests, MSVC/WiX, real MSI or Windows acceptance.
"""
from pathlib import Path
import argparse
import concurrent.futures
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--timeout', type=int, default=120, help='seconds per test')
    parser.add_argument('--log-dir', type=Path, help='default: new directory in system temp')
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error('jobs and timeout must be positive')
    root = Path(__file__).resolve().parents[2]
    logs = args.log_dir or Path(tempfile.mkdtemp(prefix='glass-mocks-'))
    logs.mkdir(parents=True, exist_ok=True)
    tests = sorted(Path(__file__).parent.glob('*_mock.py'))

    def run(test):
        try:
            result = subprocess.run([sys.executable, str(test)], cwd=root,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=args.timeout)
            output, code = result.stdout, result.returncode
        except subprocess.TimeoutExpired as error:
            output, code = (error.stdout or b'') + b'\nTIMEOUT\n', 124
        (logs / (test.stem + '.log')).write_bytes(output)
        return test.name, code

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(run, tests))
    for name, code in results:
        print(f'{"PASS" if code == 0 else "FAIL"} {name}' + (f' (exit {code})' if code else ''))
    failed = sum(code != 0 for _, code in results)
    summary = f'{len(results)} tests: {len(results)-failed} passed, {failed} failed'
    (logs / 'summary.txt').write_text(summary + '\n', encoding='utf-8')
    print(f'{summary}\nLogs: {logs}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
