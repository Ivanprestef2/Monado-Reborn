"""Game code builder: the launcher's progress file, the time-weighted bar and the default job count (no game data)."""
import os
import json
import shutil
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'gamebuild'))
# The lifter needs unicorn and these tests never lift: a stand-in for the import, taken out again at once so the
# lifter's own tests (test_jumptables) still see whether the real one is installed.
_stub = 'gen' not in sys.modules
if _stub:
    sys.modules['gen'] = types.ModuleType('gen')
import gamebuild as gb  # noqa: E402
if _stub:
    del sys.modules['gen']


class OverallProgress(unittest.TestCase):
    def test_steps_follow_each_other(self):
        gb.set_shares(16)
        points = ([gb.overall('generate', f / 10) for f in range(11)] +
                  [gb.overall('compile', f / 10) for f in range(11)] + [gb.overall('link', 0), gb.overall('link', 1)])
        self.assertEqual(points, sorted(points))
        self.assertEqual(points[0], 0)
        self.assertAlmostEqual(points[-1], 1)
        self.assertAlmostEqual(gb.overall('generate', 1), gb.overall('compile', 0))

    def test_more_jobs_make_compiling_a_smaller_share(self):
        gb.set_shares(4)
        four = gb.overall('generate', 1)
        gb.set_shares(16)
        sixteen = gb.overall('generate', 1)
        self.assertLess(four, sixteen)
        self.assertLess(sixteen, 1 - gb.LINK_SHARE)

    def test_progress_file_keeps_the_old_first_line(self):
        gb.set_shares(8)
        with tempfile.TemporaryDirectory() as out:
            gb.report(Path(out), 'compile', 3, 10, 'Compiling main/x.c', 0.5)
            lines = (Path(out) / 'progress.txt').read_text(encoding='utf-8').splitlines()
        self.assertEqual(lines[0], 'compile 3 10 Compiling main/x.c')
        name, value = lines[1].split()
        self.assertEqual(name, 'overall')
        self.assertAlmostEqual(float(value), gb.overall('compile', 0.5), places=4)


class CompileCosts(unittest.TestCase):
    def test_times_from_a_log_round_trip(self):
        with tempfile.TemporaryDirectory() as temp:
            log, resources = Path(temp) / 'gamebuild.log', Path(temp) / 'res'
            log.write_text('[10:00:00 +1s] compile sdk/sdk_090.c: zig cc ...\n'
                           '[10:01:07 +68s] compile sdk/sdk_090.c: exit 0 after 66.84s\n'
                           '[10:00:03 +3s] compile main/main_001.c: exit 0 after 0.2s\n', encoding='utf-8')
            (resources / 'games').mkdir(parents=True)
            with mock.patch('builtins.print'):
                gb.write_compile_costs(log, resources / 'games/compile_cost.json')
            self.assertEqual(gb.compile_costs(resources), {'main/main_001.c': 0.2, 'sdk/sdk_090.c': 66.8})

    def test_missing_or_broken_file_is_no_hint(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual(gb.compile_costs(Path(temp)), {})
            (Path(temp) / 'games').mkdir()
            (Path(temp) / 'games/compile_cost.json').write_text('[1, 2]', encoding='utf-8')
            self.assertEqual(gb.compile_costs(Path(temp)), {})

    def test_shipped_hints_read_back(self):
        # The repository keeps it in games/xcde; a package stages it as games/compile_cost.json.
        with tempfile.TemporaryDirectory() as temp:
            (Path(temp) / 'games').mkdir()
            shutil.copy(Path(__file__).resolve().parents[2] / 'games/xcde/compile_cost.json', Path(temp) / 'games')
            seconds = gb.compile_costs(Path(temp))
        self.assertGreater(len(seconds), 100)
        self.assertTrue(all(key.endswith('.c') and '/' in key and value >= 0 for key, value in seconds.items()))


class DefaultJobs(unittest.TestCase):
    def test_one_per_cpu_when_memory_allows(self):
        with mock.patch.object(gb.os, 'cpu_count', return_value=16), \
             mock.patch.object(gb, 'available_memory', return_value=64 << 30):
            self.assertEqual(gb.default_jobs(), 16)

    def test_memory_limits_the_jobs(self):
        with mock.patch.object(gb.os, 'cpu_count', return_value=16), \
             mock.patch.object(gb, 'available_memory', return_value=4 * gb.JOB_MEMORY):
            self.assertEqual(gb.default_jobs(), 4)

    def test_at_least_two(self):
        with mock.patch.object(gb.os, 'cpu_count', return_value=16), \
             mock.patch.object(gb, 'available_memory', return_value=0):
            self.assertEqual(gb.default_jobs(), 2)

    def test_unknown_memory_keeps_the_old_limit(self):
        with mock.patch.object(gb.os, 'cpu_count', return_value=16), \
             mock.patch.object(gb, 'available_memory', return_value=None):
            self.assertEqual(gb.default_jobs(), 8)


class BuildCache(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.dump = self.root / 'dump'
        (self.dump / 'exefs').mkdir(parents=True)
        self.main = self.dump / 'exefs/main'
        self.main.write_bytes(b'one')
        (self.dump / 'exefs/sdk').write_bytes(b'sdk')
        self.resources = self.root / 'resources'
        self.resources.mkdir()
        (self.resources / 'gamelib-id.txt').write_text('sample-abi', encoding='utf-8')
        self.imports = self.resources / 'imports.a'
        self.imports.write_bytes(b'old')
        self.link = {'library': 'lasgame.so', 'link_files': ['imports.a']}

    def test_same_size_import_library_and_module_changes_invalidate(self):
        before = gb.fingerprint(self.dump, self.resources, self.link)
        self.imports.write_bytes(b'new')
        after_imports = gb.fingerprint(self.dump, self.resources, self.link)
        self.assertNotEqual(before, after_imports)

        stamp = self.main.stat()
        self.main.write_bytes(b'two')
        os.utime(self.main, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assertNotEqual(after_imports, gb.fingerprint(self.dump, self.resources, self.link))

    @unittest.skipUnless(os.name == 'posix', 'exercise Linux advisory locking')
    def test_competing_build_cannot_lock_the_same_output(self):
        lock = self.root / '.build.lock'
        with lock.open('a+b') as first, lock.open('a+b') as second:
            gb.lock_build(first)
            with self.assertRaises(BlockingIOError):
                gb.lock_build(second)
        with lock.open('a+b') as next_build:
            gb.lock_build(next_build)

    def test_strict_validation_runs_even_with_matching_build(self):
        (self.resources / 'link.json').write_text(json.dumps(self.link), encoding='utf-8')
        out = self.root / 'out'
        out.mkdir()
        (out / 'lasgame.so').touch()
        (out / 'lasgame.so.id').write_text('sample-abi', encoding='utf-8')
        current = gb.fingerprint(self.dump, self.resources, self.link)
        (out / 'build.json').write_text(json.dumps({'fingerprint': current}), encoding='utf-8')
        args = ['--root', str(self.dump), '--out', str(out), '--resources', str(self.resources), '--jobs', '1']
        with mock.patch.object(gb, 'validate', side_effect=SystemExit('strict dump mismatch')) as validate:
            with mock.patch('builtins.print'):
                self.assertEqual(gb.main(args), 0)
            validate.assert_not_called()
            with self.assertRaisesRegex(SystemExit, 'strict dump mismatch'):
                gb.main(args + ['--strict-dump'])
            validate.assert_called_once_with(self.dump.resolve(), self.resources.resolve(), strict=True)


if __name__ == '__main__':
    unittest.main()
