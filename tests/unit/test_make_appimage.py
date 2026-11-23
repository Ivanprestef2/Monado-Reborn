"""make_appimage.py: only a release AppImage is a RecompLauncher package (manifest plus the setup builder)."""
import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'package'))
import make_appimage as ma  # noqa: E402


@unittest.skipUnless(os.name == 'posix' and shutil.which('sh') and shutil.which('nm'),
                     'the AppImage packager and its stand-in tool are POSIX programs')
class AppImageTree(unittest.TestCase):
    """The real main() against stand-in payloads and an appimagetool that keeps a copy of the AppDir."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.build = base / 'linux'
        self.build.mkdir()
        for name in ('monado', 'monado-launcher'):
            (self.build / name).write_bytes(b'')
        self.output = base / 'Monado-x86_64.AppImage'
        self.snap = base / 'appdir'
        self.tool = base / 'appimagetool'
        self.tool.write_text('#!/bin/sh\nset -eu\nrm -rf "$MONADO_TEST_APPDIR"\n'
                             'cp -a "$1" "$MONADO_TEST_APPDIR"\n: > "$2"\n', encoding='utf-8')
        self.tool.chmod(0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def package(self, *args):
        argv = ['make_appimage.py', '--build', str(self.build), '--output', str(self.output),
                '--appimagetool', str(self.tool), *args]
        with mock.patch.dict(os.environ, {'MONADO_TEST_APPDIR': str(self.snap)}, clear=False), \
                mock.patch.object(sys, 'argv', argv), \
                mock.patch.object(ma, 'dependencies', return_value=[]), \
                mock.patch.object(ma, 'linked', return_value=set()), \
                contextlib.redirect_stdout(io.StringIO()):
            ma.main()

    def test_a_personal_image_has_no_manifest_or_builder(self):
        self.package()
        self.assertTrue(self.output.is_file())
        self.assertTrue((self.snap / 'AppRun').is_file())
        self.assertFalse((self.snap / 'recomplauncher.json').exists())
        self.assertFalse((self.snap / 'usr/share/monado/gamebuild').exists())

    def test_a_release_image_has_the_manifest_and_the_builder(self):
        gamebuild = Path(self.tmp.name) / 'gamebuild'
        gamebuild.mkdir()
        for name in ('link.json', 'gamelib-id.txt', 'monado-gamebuild'):
            (gamebuild / name).write_text('', encoding='utf-8')
        (gamebuild / 'monado-gamebuild').chmod(0o755)
        self.package('--release', '--gamebuild', str(gamebuild))
        manifest = self.snap / 'recomplauncher.json'
        self.assertTrue(manifest.is_file())
        self.assertEqual(manifest.read_bytes(), (ROOT / 'recomplauncher.json').read_bytes())
        self.assertTrue((self.snap / 'usr/share/monado/gamebuild/monado-gamebuild').is_file())


if __name__ == '__main__':
    unittest.main()
