"""Skip launcher on AppImage startup (`skip_launcher` in xcde.ini): AppRun launch behavior and UI/INI contracts."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

STAND_IN = '#!/bin/sh\necho "$(basename "$0")"\nfor a in "$@"; do echo "arg $a"; done\n'


@unittest.skipUnless(os.name == 'posix' and shutil.which('sh'), 'AppRun is a POSIX shell script')
class AppRunSkipLauncher(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.appdir = base / 'appdir'
        self.data = base / 'data'
        self.data.mkdir()
        for rel in ('usr/bin/monado', 'usr/bin/monado-launcher'):
            prog = self.appdir / rel
            prog.parent.mkdir(parents=True, exist_ok=True)
            prog.write_text(STAND_IN, encoding='utf-8')
            prog.chmod(0o755)
        shutil.copy2(ROOT / 'packaging/linux/AppRun', self.appdir / 'AppRun')
        self.dump = base / 'dump'
        (self.dump / 'exefs').mkdir(parents=True)
        (self.dump / 'exefs/main').write_bytes(b'')
        (self.dump / 'romfs').mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def link_dump(self):
        os.symlink(self.dump, self.data / 'targets')

    def write_ini(self, content, binary=False):
        p = self.data / 'xcde.ini'
        if binary:
            p.write_bytes(content)
        else:
            p.write_text(content, encoding='utf-8')

    def run_apprun(self, *args):
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'HOME': self.tmp.name,
               'MONADO_DATA_ROOT': str(self.data)}
        done = subprocess.run(['sh', str(self.appdir / 'AppRun'), *args], env=env, text=True, capture_output=True)
        return done.returncode, done.stdout.splitlines(), done.stderr

    def test_default_starts_launcher(self):
        self.link_dump()
        code, lines, _ = self.run_apprun()
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado-launcher'])

    def test_skip_launcher_0_starts_launcher(self):
        self.link_dump()
        self.write_ini('skip_launcher = 0\n')
        code, lines, _ = self.run_apprun()
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado-launcher'])

    def test_skip_launcher_1_starts_game_directly(self):
        self.link_dump()
        self.write_ini('fullscreen = 1\nskip_launcher = 1\n')
        code, lines, _ = self.run_apprun()
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado', 'arg targets'])

    def test_skip_launcher_handles_bom_and_crlf(self):
        self.link_dump()
        self.write_ini(b'\xef\xbb\xbfskip_launcher = 1\r\n', binary=True)
        code, lines, _ = self.run_apprun()
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado', 'arg targets'])

    def test_skip_launcher_falls_back_to_launcher_without_dump(self):
        self.write_ini('skip_launcher = 1\n')
        code, lines, _ = self.run_apprun()
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado-launcher'])

    def test_launcher_flag_overrides_skip_launcher(self):
        self.link_dump()
        self.write_ini('skip_launcher = 1\n')
        code, lines, _ = self.run_apprun('--launcher')
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado-launcher', 'arg --launcher'])

    def test_explicit_dump_is_forwarded_without_changing_existing_targets(self):
        targets = self.data / 'targets'
        targets.mkdir()
        code, lines, _ = self.run_apprun('--launcher', str(self.dump))
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado-launcher', 'arg --dump', f'arg {self.dump}', 'arg --launcher'])
        self.assertEqual(list(targets.iterdir()), [])

    def test_release_appimage_requires_matching_gamelib(self):
        self.link_dump()
        self.write_ini('skip_launcher = 1\n')
        gb = self.appdir / 'usr/share/monado/gamebuild'
        gb.mkdir(parents=True)
        (gb / 'link.json').write_text('{}', encoding='utf-8')
        (gb / 'gamelib-id.txt').write_text('build-v2\n', encoding='utf-8')

        # Missing gamebuild/lasgame.so -> falls back to launcher to build it
        code, lines, _ = self.run_apprun()
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado-launcher'])

        # Stale gamebuild/lasgame.so.id -> falls back to launcher to rebuild after update
        out_gb = self.data / 'gamebuild'
        out_gb.mkdir()
        (out_gb / 'lasgame.so').write_bytes(b'')
        (out_gb / 'lasgame.so.id').write_text('build-v1\n', encoding='utf-8')
        code, lines, _ = self.run_apprun()
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado-launcher'])

        # Matching id -> skips launcher and launches game directly
        (out_gb / 'lasgame.so.id').write_text('build-v2\n', encoding='utf-8')
        code, lines, _ = self.run_apprun()
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado', 'arg targets'])

    def test_skip_launcher_uses_library_next_to_remembered_dump(self):
        package = self.appdir / 'usr/share/monado/gamebuild'
        package.mkdir(parents=True)
        (package / 'link.json').write_text('{}', encoding='utf-8')
        (package / 'gamelib-id.txt').write_text('build-v2\n', encoding='utf-8')
        compiled = self.dump / 'gamebuild'
        compiled.mkdir()
        (compiled / 'lasgame.so').touch()
        (compiled / 'lasgame.so.id').write_text('build-v2\n', encoding='utf-8')
        self.write_ini(f'skip_launcher = 1\ngamebuild_in_dump = 1\ngame_dump = {self.dump}\n')
        code, lines, _ = self.run_apprun()
        self.assertEqual(code, 0)
        self.assertEqual(lines, ['monado', f'arg {self.dump}'])


class SkipLauncherContracts(unittest.TestCase):
    def test_settings_and_ui_wire_skip_launcher(self):
        settings_h = (ROOT / 'runtime/settings.h').read_text(encoding='utf-8')
        settings_ini = (ROOT / 'runtime/host/settings_ini.c').read_text(encoding='utf-8')
        menu_vk = (ROOT / 'runtime/ui/menu_vk.cpp').read_text(encoding='utf-8')
        launcher = (ROOT / 'tools/launcher/launcher.cpp').read_text(encoding='utf-8')

        self.assertIn('bool skip_launcher;', settings_h)
        self.assertIn('"skip_launcher"', settings_ini)
        self.assertIn('skip_launcher = %d', settings_ini)
        self.assertIn('&g_settings.skip_launcher', menu_vk)
        self.assertIn('"skip_launcher"', launcher)
        self.assertIn('skip_launcher = %d', launcher)
        self.assertIn('g.skip_launcher = skip_ui;', launcher)
        self.assertIn('ImGui::BeginDisabled(!gc_ok);', launcher)
        self.assertIn('!*g_mismatch_desc && gc_ready()', launcher)
        self.assertIn('if (!abs_exists(out + "/" GC_LIB)) return false;', launcher)


if __name__ == '__main__':
    unittest.main()
