"""RecompLauncher support (T279): AppRun's --game / --prepare modes, run against stand-in programs, the builder's
progress lines, and source contracts for the Big Picture fullscreen and the launcher's update check (no game data)."""
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'gamebuild'))
# The lifter needs unicorn and these tests never lift (as in test_gamebuild_progress).
_stub = 'gen' not in sys.modules
if _stub:
    sys.modules['gen'] = types.ModuleType('gen')
import gamebuild as gb  # noqa: E402
if _stub:
    del sys.modules['gen']

# A stand-in that prints its own name, its arguments and the folder it runs in, one per line.
STAND_IN = ('#!/bin/sh\necho "$(basename "$0")"\nfor a in "$@"; do echo "arg $a"; done\n'
            'echo "game ${MONADO_GAME_EXE:-}"\necho "cwd $(pwd -P)"\n')


@unittest.skipUnless(os.name == 'posix' and shutil.which('sh'), 'AppRun is a POSIX shell script')
class AppRunModes(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.appdir = base / 'install'
        for rel in ('usr/bin/monado', 'usr/bin/monado-launcher', 'usr/share/monado/gamebuild/monado-gamebuild'):
            program = self.appdir / rel
            program.parent.mkdir(parents=True, exist_ok=True)
            program.write_text(STAND_IN, encoding='utf-8')
            program.chmod(0o755)
        shutil.copy2(ROOT / 'packaging/linux/AppRun', self.appdir / 'AppRun')
        self.dump = base / 'dump'
        (self.dump / 'exefs').mkdir(parents=True)
        (self.dump / 'exefs/main').write_bytes(b'')
        (self.dump / 'romfs').mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def run_apprun(self, *args):
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'HOME': self.tmp.name,
               'MONADO_DATA_ROOT': str(self.appdir)}
        done = subprocess.run(['sh', str(self.appdir / 'AppRun'), *args], env=env, text=True, capture_output=True)
        return done.returncode, done.stdout.splitlines(), done.stderr.splitlines()

    def test_game_plays_the_dump_directly(self):
        code, lines, _ = self.run_apprun('--game', str(self.dump))
        self.assertEqual(code, 0)
        self.assertEqual(lines[:3], ['monado', 'arg --game', f'arg {self.dump.resolve()}'])

    def test_game_runs_in_the_data_folder(self):
        _, lines, _ = self.run_apprun('--game', str(self.dump))
        self.assertEqual(lines[-1], f'cwd {self.appdir.resolve()}')

    def test_game_leaves_no_link_to_the_dump_in_the_install_folder(self):
        self.run_apprun('--game', str(self.dump))
        self.assertFalse(os.path.lexists(self.appdir / 'targets'))

    def test_game_without_a_dump_stops_with_code_2(self):
        code, _, _ = self.run_apprun('--game')
        self.assertEqual(code, 2)

    def test_prepare_runs_the_builder_with_its_arguments(self):
        code, lines, _ = self.run_apprun('--prepare', '--root', str(self.dump), '--out', 'x')
        self.assertEqual(code, 0)
        self.assertEqual(lines[:5], ['monado-gamebuild', 'arg --root', f'arg {self.dump}', 'arg --out', 'arg x'])

    def test_prepare_names_the_game_for_the_shader_precompile(self):
        _, lines, _ = self.run_apprun('--prepare', '--precompile')
        self.assertIn(f'game {self.appdir}/usr/bin/monado', lines)

    def test_prepare_without_a_builder_stops_with_code_126(self):
        (self.appdir / 'usr/share/monado/gamebuild/monado-gamebuild').unlink()
        code, _, errors = self.run_apprun('--prepare')
        self.assertEqual(code, 126)
        self.assertTrue(any('game-code builder' in line for line in errors), errors)

    def test_prepare_with_a_builder_that_is_not_executable_stops_with_code_126(self):
        builder = self.appdir / 'usr/share/monado/gamebuild/monado-gamebuild'
        builder.chmod(0o644)
        code, _, errors = self.run_apprun('--prepare')
        self.assertEqual(code, 126)
        self.assertTrue(any('game-code builder' in line for line in errors), errors)

    def test_play_still_links_the_dump(self):
        self.run_apprun('--play', str(self.dump))
        self.assertEqual((self.appdir / 'targets').resolve(), self.dump.resolve())


class BuilderProgressLines(unittest.TestCase):
    def report(self, env, *args):
        printed = io.StringIO()
        with tempfile.TemporaryDirectory() as out, mock.patch.dict(os.environ, env, clear=False), \
                contextlib.redirect_stdout(printed):
            gb.report(Path(out), *args)
        return printed.getvalue().splitlines()

    def test_recomplauncher_gets_a_progress_line(self):
        gb.set_shares(8)
        lines = self.report({'RECOMPLAUNCHER': '1'}, 'compile', 3, 10, 'Compiling main/x.c', 0.5)
        expected = f'progress {gb.overall("compile", 0.5):.4f} Compiling the game code: 3 of 10 files'
        self.assertEqual(lines, [expected])

    def test_own_launcher_runs_stay_quiet(self):
        lines = self.report({'RECOMPLAUNCHER': ''}, 'compile', 3, 10, 'Compiling main/x.c', 0.5)
        self.assertEqual(lines, [])

    def test_link_step_shows_its_own_text(self):
        self.assertEqual(gb.launcher_step_text('link', 1, 1, 'Already built'), 'Already built')

    def test_prepare_switch_is_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / 'gamebuild.log'
            log.write_text('[00:00:01 +1s] compile main/a.c: exit 0 after 2.5s\n', encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gb.main(['--prepare', '--write-costs', str(log), str(Path(d) / 'cost.json')]), 0)


# A stand-in game for `--precompile <dump>`: writes the progress file twice and the done stamp, as precompile.c does.
GAME_PRECOMPILES = ('#!/bin/sh\n[ "$1" = --precompile ] || exit 9\n'
                    'echo "scan 5 10 4 2" > cache/precompile_progress.txt\nsleep 1\n'
                    'echo "pipelines 3 3 3 3" > cache/precompile_progress.txt\nsleep 1\n'
                    'echo "programs 3 built 3" > cache/precompile_done.txt\n')
GAME_FAILS = '#!/bin/sh\nexit 3\n'


class ShaderPrecompile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        gb._launcher_build_share[0] = gb.PRECOMPILE_BUILD_SHARE

    def tearDown(self):
        gb._launcher_build_share[0] = 1.0
        self.tmp.cleanup()

    def game(self, script):
        exe = self.root / 'monado'
        exe.write_text(script, encoding='utf-8')
        exe.chmod(0o755)
        return exe

    def run_precompile(self, exe):
        env = {'RECOMPLAUNCHER': '1', 'MONADO_DATA_ROOT': str(self.root), 'MONADO_GAME_EXE': str(exe or '')}
        printed = io.StringIO()
        with mock.patch.dict(os.environ, env, clear=False), contextlib.redirect_stdout(printed):
            gb.precompile_shaders(self.root / 'dump')
        return [line for line in printed.getvalue().splitlines() if line.startswith('progress ')]

    def test_scan_line_is_the_first_part_of_the_shaders(self):
        share, text = gb.precompile_step('scan 5 10 4 2')
        self.assertAlmostEqual(share, gb.PRECOMPILE_SCAN_SHARE * (0.15 * 0.5 + 0.85 * 0.5 * 0.5))
        self.assertEqual(text, 'Preparing the shaders: 5 of 10 files')

    def test_pipelines_pass_follows_the_scan(self):
        share, text = gb.precompile_step('pipelines 1 4 4 1')
        self.assertAlmostEqual(share, gb.PRECOMPILE_SCAN_SHARE + (1 - gb.PRECOMPILE_SCAN_SHARE) * 0.25)
        self.assertEqual(text, 'Preparing the shaders: final pass, 1 of 4')

    def test_done_line_ends_the_bar(self):
        self.assertEqual(gb.precompile_step('done 10 10 5 5'), (1.0, 'Shaders ready'))

    def test_seed_line_is_labelled_and_holds_the_bar(self):
        self.assertEqual(gb.precompile_step('seed 10 10 5 5'),
                         (gb.PRECOMPILE_SCAN_SHARE, 'Preparing the shaders: seeding variants'))

    def test_pipelines_then_done_does_not_move_the_bar_back(self):
        pipeline = gb.precompile_step('pipelines 4 4 4 4')[0]
        self.assertEqual(pipeline, 1.0)
        self.assertEqual(gb.precompile_step('done 10 10 5 5')[0], 1.0)

    def test_unreadable_progress_is_skipped(self):
        self.assertIsNone(gb.precompile_step('scan 1 0 0 0'))
        self.assertIsNone(gb.precompile_step('half a line'))

    def test_precompile_for_this_executable_is_current(self):
        exe = self.game(GAME_FAILS)
        stat = exe.stat()
        (self.root / 'cache').mkdir()
        (self.root / 'cache/precompile_done.txt').write_text(f'programs 1\nexe {stat.st_size} {int(stat.st_mtime)}\n')
        self.assertTrue(gb.precompile_is_current(self.root, exe))

    def test_precompile_for_another_build_is_not_current(self):
        exe = self.game(GAME_FAILS)
        (self.root / 'cache').mkdir()
        (self.root / 'cache/precompile_done.txt').write_text('programs 1\nexe 1 2\n')
        self.assertFalse(gb.precompile_is_current(self.root, exe))

    def test_frozen_builders_library_path_is_given_back(self):
        env = {'LD_LIBRARY_PATH': '/frozen/_internal', 'LD_LIBRARY_PATH_ORIG': '/app/usr/lib'}
        with mock.patch.dict(os.environ, env, clear=False):
            self.assertEqual(gb.child_environment()['LD_LIBRARY_PATH'], '/app/usr/lib')

    def test_no_game_skips_without_failing(self):
        lines = self.run_precompile(None)
        self.assertEqual(lines, ['progress 1.0000 Shaders compile while you play'])

    @unittest.skipUnless(os.name == 'posix', 'the stand-in game is a shell script')
    def test_precompile_reports_its_progress_after_the_build(self):
        lines = self.run_precompile(self.game(GAME_PRECOMPILES))
        values = [float(line.split()[1]) for line in lines]
        self.assertEqual(values, sorted(values))
        self.assertEqual(values[0], gb.PRECOMPILE_BUILD_SHARE)
        self.assertEqual(lines[-1], 'progress 1.0000 Shaders ready')
        self.assertIn('final pass', ' '.join(lines))

    @unittest.skipUnless(os.name == 'posix', 'the stand-in game is a shell script')
    def test_a_failed_precompile_still_finishes_the_setup(self):
        lines = self.run_precompile(self.game(GAME_FAILS))
        self.assertEqual(lines[-1], 'progress 1.0000 Shaders compile while you play')

    def test_precompile_switch_is_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / 'gamebuild.log'
            log.write_text('[00:00:01 +1s] compile main/a.c: exit 0 after 2.5s\n', encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gb.main(['--precompile', '--write-costs', str(log), str(Path(d) / 'cost.json')]), 0)


class SetupStamp(unittest.TestCase):
    """build.json is RecompLauncher's prepare.done: it may exist only after a build that finished."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.resources = base / 'resources'
        self.resources.mkdir()
        self.link = {'library': 'lasgame.so'}
        (self.resources / 'link.json').write_text(json.dumps(self.link), encoding='utf-8')
        (self.resources / 'gamelib-id.txt').write_text('test-gamelib\n', encoding='utf-8')
        self.dump = base / 'dump'
        (self.dump / 'exefs').mkdir(parents=True)
        (self.dump / 'exefs/main').write_bytes(b'main')
        (self.dump / 'exefs/sdk').write_bytes(b'sdk')
        (self.dump / 'romfs').mkdir()
        self.out = base / 'out'
        self.out.mkdir()
        (self.out / 'lasgame.so').write_bytes(b'library')
        self.stamp = self.out / 'build.json'
        gb._launcher_build_share[0] = gb.PRECOMPILE_BUILD_SHARE

    def tearDown(self):
        gb._launcher_build_share[0] = 1.0
        self.tmp.cleanup()

    def argv(self, *extra):
        return ['--root', str(self.dump), '--out', str(self.out), '--resources', str(self.resources),
                '--jobs', '1', *extra]

    def fingerprint(self):
        return gb.fingerprint(self.dump, self.resources, self.link)

    def write_stamp(self, fingerprint, units=None):
        self.stamp.write_text(json.dumps({'fingerprint': fingerprint, 'units': units or {}}), encoding='utf-8')

    def test_a_rebuild_that_fails_leaves_no_done_stamp(self):
        self.write_stamp('0' * 64)
        with mock.patch.object(gb, 'validate', side_effect=SystemExit('bad game files')), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit):
                gb.main(self.argv())
        self.assertFalse(self.stamp.exists())

    def test_a_precompile_oserror_on_the_up_to_date_path_still_finishes_the_setup(self):
        self.write_stamp(self.fingerprint())
        printed = io.StringIO()
        with mock.patch.dict(os.environ, {'RECOMPLAUNCHER': '1'}, clear=False), \
                mock.patch.object(gb, 'precompile_shaders', side_effect=OSError('disk full')), \
                contextlib.redirect_stdout(printed):
            self.assertEqual(gb.main(self.argv('--precompile')), 0)
        lines = printed.getvalue().splitlines()
        self.assertIn('progress 1.0000 Shaders compile while you play', lines)
        self.assertTrue(any('disk full' in line for line in lines), lines)
        self.assertTrue(self.stamp.is_file())  # an up-to-date build keeps its prepare.done

    def test_a_precompile_oserror_after_a_build_still_finishes_the_setup_and_reuses_the_units(self):
        units = {'main/main_001.c': '12 34'}
        self.write_stamp('0' * 64, units)
        seen = {}

        def compile_units(resources, out, link, jobs, log, previous=None):
            seen['previous'] = previous
            return []

        printed = io.StringIO()
        with mock.patch.dict(os.environ, {'RECOMPLAUNCHER': '1'}, clear=False), \
                mock.patch.object(gb, 'validate'), mock.patch.object(gb, 'describe_compiler'), \
                mock.patch.object(gb, 'generate_modules', return_value=[]), \
                mock.patch.object(gb, 'compile_units', side_effect=compile_units), \
                mock.patch.object(gb, 'link_library'), \
                mock.patch.object(gb, 'precompile_shaders', side_effect=OSError('no space left')), \
                contextlib.redirect_stdout(printed):
            self.assertEqual(gb.main(self.argv('--precompile', '--force')), 0)
        self.assertEqual(seen['previous'], units)  # the done stamp went, but its unit signatures did not
        lines = printed.getvalue().splitlines()
        self.assertIn('progress 1.0000 Shaders compile while you play', lines)
        self.assertTrue(any('no space left' in line for line in lines), lines)
        self.assertEqual(json.loads(self.stamp.read_text(encoding='utf-8'))['fingerprint'], self.fingerprint())


class SourceContracts(unittest.TestCase):
    def test_big_picture_opens_fullscreen(self):
        device = (ROOT / 'runtime/nvn/vulkan/device.c').read_text(encoding='utf-8')
        self.assertIn('getenv("RECOMPLAUNCHER_MODE")', device)
        self.assertIn('bool forced_fs = gamescope_fs || bigpicture_fs;', device)

    def test_big_picture_keeps_the_saved_setting(self):
        device = (ROOT / 'runtime/nvn/vulkan/device.c').read_text(encoding='utf-8')
        self.assertNotIn('g_settings.fullscreen = true', device)

    def test_no_update_banner_under_recomplauncher(self):
        launcher = (ROOT / 'tools/launcher/launcher.cpp').read_text(encoding='utf-8')
        self.assertIn('upd_is_release_build() && !from_recomplauncher) upd_start(false);', launcher)


if __name__ == '__main__':
    unittest.main()
