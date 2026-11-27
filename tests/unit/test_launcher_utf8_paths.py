"""GitHub #218: the launcher's UTF-8 paths go to the wide Windows APIs, not to the ANSI (…) ones.

Source contracts for the fix. The runtime behaviour (a folder named "João" opens, renames, replaces and removes its
files) is checked by tests/native/test_utf8_paths.c on Linux and on Windows.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = (ROOT / "tools/launcher/launcher.cpp").read_text(encoding="utf-8")
PATHS = (ROOT / "runtime/host/utf8_path.h").read_text(encoding="utf-8")
INI_WRITER = (ROOT / "runtime/host/ini_writer.h").read_text(encoding="utf-8")
ATOMIC_FILE = (ROOT / "runtime/host/atomic_file.h").read_text(encoding="utf-8")
NATIVE_TEST = (ROOT / "tests/native/test_utf8_paths.c").read_text(encoding="utf-8")


def code_only(source):
    """The source with comments and string/char literals blanked (the checks below are about calls, not prose)."""
    out, i, n = [], 0, len(source)
    while i < n:
        c = source[i]
        if c == "/" and source[i + 1:i + 2] == "/":
            while i < n and source[i] != "\n":
                i += 1
        elif c == "/" and source[i + 1:i + 2] == "*":
            i += 2
            while i + 1 < n and source[i:i + 2] != "*/":
                i += 1
            i += 2
            out.append(" ")
        elif c in "\"'":
            quote = c
            i += 1
            while i < n:
                if source[i] == "\\":
                    i += 2
                    continue
                if source[i] == quote:
                    i += 1
                    break
                i += 1
            out.append('""')
        else:
            out.append(c)
            i += 1
    return "".join(out)


CODE = code_only(LAUNCHER)


class LauncherUtf8Paths(unittest.TestCase):
    def assertHas(self, needle, haystack, what):
        self.assertTrue(needle in haystack, what)

    def assertHasNot(self, needle, haystack, what):
        self.assertFalse(needle in haystack, what)

    def test_no_ansi_win32_call_is_left(self):
        # These read the UTF-8 path in the system code page: under C:\Users\João\ the folder came out as "JoÃ£o" and
        # CreateProcessA failed with error 267 (game build, update check) while the CRT's fopen missed xcde.ini.
        for ansi in ("CreateProcessA", "CreateFileA", "MoveFileExA", "CreateDirectoryA", "DeleteFileA",
                     "ShellExecuteA", "SetEnvironmentVariableA", "GetEnvironmentVariableA", "GetSystemDirectoryA",
                     "FindFirstFileA", "FindNextFileA", "GetFileAttributesA", "WIN32_FIND_DATAA", "STARTUPINFOA",
                     "MessageBoxA"):
            self.assertHasNot(ansi, CODE, ansi)

    def test_every_child_process_gets_a_utf16_command_line(self):
        calls = re.findall(r"CreateProcessW\((.*?)\);", CODE, re.S)
        # game, shader precompile, shader precompile status (T282), game code build, curl, report
        self.assertEqual(len(calls), 6)
        for call in calls:
            self.assertHas("wcmd.data()", call, "the UTF-8 command line, converted")
            self.assertHas("WPath(", call, "the executable and/or the working folder")
        self.assertHasNot("std::vector<char> buf", CODE, "the old narrow command-line buffer")

    def test_the_launcher_opens_its_files_through_the_utf8_wrappers(self):
        for crt in ("fopen", "remove", "rename"):
            self.assertIsNone(re.search(r"(?<![\w.])" + crt + r"\(", CODE), crt)
        self.assertHas("xcde_utf8_fopen(", CODE, "fopen wrapper")
        self.assertHas("xcde_utf8_remove(", CODE, "remove wrapper")
        self.assertHas("xcde_utf8_rename(", CODE, "rename wrapper")
        # a path's existence: SDL reads UTF-8 paths on every platform (the precompile status, T282, asks the runtime
        # instead of reading the done stamp here)
        self.assertIsNone(re.search(r"(?<![\w.])stat\(", CODE), "stat")
        self.assertHas("SDL_GetPathInfo(path(rel)", CODE, "exists() through SDL")

    def test_the_update_check_hands_curl_a_relative_output_path(self):
        # C:\Windows\System32\curl.exe is not ours: give it no non-ASCII bytes in argv, its working folder is the root.
        self.assertHas('"-o", "build/update_check.json"', LAUNCHER, "curl -o")

    def test_urls_keep_their_forward_slashes(self):
        # shell_open() turns '/' into '\' for Windows paths; an URL must reach the browser as it is.
        self.assertHas("shell_open_url(url)", CODE, "the URL fallback")
        self.assertHas('ShellExecuteW(NULL, L"open", W(url).c_str()', LAUNCHER, "URL as it is")

    def test_the_environment_is_read_and_written_as_utf16(self):
        self.assertHas("GetEnvironmentVariableW", CODE, "environment read")
        self.assertHas("SetEnvironmentVariableW", CODE, "environment write")
        self.assertHas('env_w(L"PATH")', LAUNCHER, "PATH with accents")
        # the only narrow read is the POSIX implementation of env_get_u8
        self.assertEqual(len(re.findall(r"(?<![\w.])getenv\(", CODE)), 1)

    def test_environment_presence_distinguishes_unset_from_empty(self):
        # Restoring a removed Diagnostics row must not use value.empty() as a proxy for presence.
        self.assertHas("static bool env_get_u8(const char *name, std::string &value)", LAUNCHER,
                       "a read that reports whether the variable existed")
        self.assertHas("SetEnvironmentVariableW(W(name).c_str(), nullptr)", LAUNCHER,
                       "Windows unsets through the wide API")
        self.assertHas("SetLastError(ERROR_SUCCESS)", LAUNCHER,
                       "an empty Win32 environment value is not a failed read")
        self.assertHas("if (!got && GetLastError() != ERROR_SUCCESS) return false;", LAUNCHER,
                       "only a real Win32 error means the variable disappeared")
        self.assertHas("value = got ? U8(w.data()) : std::string();", LAUNCHER,
                       "a successful zero-character read preserves presence")
        self.assertHas("static void env_unset_u8(const char *name) { unsetenv(name); }", LAUNCHER,
                       "POSIX unsets the variable")
        self.assertHasNot("old.empty() ? nullptr", LAUNCHER, "empty is not the same as unset")

    def test_removed_environment_rows_restore_the_first_original_value(self):
        apply = LAUNCHER[LAUNCHER.index("static void apply_env() {"):LAUNCHER.index("static void load_env() {")]
        self.assertHas("struct AppliedEnv { std::string name; bool was_set = false; std::string value; };", LAUNCHER,
                       "presence and value are tracked together")
        self.assertHas("original.was_set = env_get_u8(name, original.value);", apply,
                       "capture the original before the first override")
        self.assertHas("if (applied.was_set) env_set_u8(applied.name.c_str(), applied.value);", apply,
                       "restore a previous value, including an empty one")
        self.assertHas("else env_unset_u8(applied.name.c_str());", apply, "unset a variable that was originally absent")
        self.assertHas("g_applied_env.erase", apply, "a removed name stops being tracked")
        self.assertLess(apply.index("if (applied.was_set)"), apply.index("env_set_u8(name, g.env[i].value);"),
                        "removed names are restored before current rows are applied")
        self.assertHas("_stricmp(a, b)", LAUNCHER, "Windows names compare case-insensitively")
        self.assertHas("return strcmp(a, b) == 0;", LAUNCHER, "POSIX names compare case-sensitively")

    def test_temporary_force_environment_also_preserves_empty_values(self):
        force = LAUNCHER[LAUNCHER.index("struct PcForce {"):LAUNCHER.index("static bool pc_start(")]
        self.assertHas('was_set = env_get_u8("XCDE_PRECOMPILE_FORCE", old);', force,
                       "remember whether FORCE was present, not merely whether it was non-empty")
        self.assertHas('if (was_set) env_set_u8("XCDE_PRECOMPILE_FORCE", old);', force,
                       "restore a set-but-empty FORCE")
        self.assertHas('else env_unset_u8("XCDE_PRECOMPILE_FORCE");', force, "otherwise unset FORCE")

    def test_shared_file_helpers_convert_the_path(self):
        self.assertHas("xcde_utf8_replace(tmp, path)", ATOMIC_FILE, "atomic replace")
        self.assertHas('xcde_utf8_fopen(tmp, "w+")', INI_WRITER, "INI temporary file")
        self.assertHas('xcde_utf8_fopen(path, "r")', INI_WRITER, "INI read")
        self.assertHas("xcde_utf8_remove(tmp)", INI_WRITER, "INI temporary file removal")
        self.assertHas("MultiByteToWideChar(CP_UTF8", PATHS, "conversion")
        self.assertHas("MoveFileExW(", PATHS, "wide replacement")
        self.assertHas("_wfopen(", PATHS, "wide fopen")

    def test_the_native_test_uses_the_reporters_folder_name_on_both_hosts(self):
        self.assertHas('utf8-Jo\\xC3\\xA3" "o-\\xF0\\x9F\\x98\\x80', NATIVE_TEST, "João + emoji folder")
        self.assertHas("CreateDirectoryW(", NATIVE_TEST, "the wide API makes the folder on Windows")
        self.assertHas("CreateProcessA(", NATIVE_TEST, "the #218 call")
        self.assertHas("CreateProcessW(", NATIVE_TEST, "the fix's call")
        self.assertHas("run_test test_utf8_paths",
                       (ROOT / "scripts/native_tests_linux.sh").read_text(encoding="utf-8"), "Linux runner")
        self.assertHas("test_utf8_paths.exe", (ROOT / "scripts/native_tests.sh").read_text(encoding="utf-8"),
                       "Windows runner")
        self.assertHas("'test_utf8_paths'", (ROOT / "tools/diag/validate_port.py").read_text(encoding="utf-8"),
                       "port validation")


if __name__ == "__main__":
    unittest.main()
