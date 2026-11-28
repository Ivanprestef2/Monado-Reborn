"""Source-contract checks for 5.1 surround sound support across runtime, launcher, and RecompLauncher."""
import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class AudioSurroundContracts(unittest.TestCase):
    def test_audren_and_audout_wire_active_channel_count(self):
        src = (ROOT / "runtime" / "services" / "audio.c").read_text(encoding="utf-8")
        playback = (ROOT / "runtime" / "services" / "audio_playback.h").read_text(encoding="utf-8")
        self.assertIn('#include "../audio_options.h"', src)
        self.assertIn("case 5: OUT(uint32_t, 0) = (uint32_t)audio_active_channels();", src)
        self.assertNotIn("case 5: OUT(uint32_t, 0) = 2;", src)
        self.assertIn("audio_sink_mix_frame(ch, r->sink_inputs, (int)out_ch", src)
        self.assertIn("SDL_AudioSpec spec = {SDL_AUDIO_F32, (int)ch, (int)r->sample_rate};", src)
        self.assertIn("audio_downmix_s16_6to2(", src)
        self.assertIn("SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK", src)
        self.assertIn("SDL_GetAudioDeviceFormat(audio_pick_playback_device()", src)
        self.assertIn('getenv("XCDE_AUDIO_CHANNELS")', src)
        self.assertIn("void audio_notify_device_change(void)", src)
        self.assertIn("xcde_audio_reconfigure(", src)
        self.assertIn("SDL_SetAudioStreamFormat(", playback)
        self.assertIn("case 4: case 12: resp->copy_handles[resp->ncopy++] = new_event_handle(true);", src)

    def test_hot_swap_wired_from_sdl_events_and_f1_menu(self):
        device_c = (ROOT / "runtime" / "nvn" / "vulkan" / "device.c").read_text(encoding="utf-8")
        menu_vk = (ROOT / "runtime" / "ui" / "menu_vk.cpp").read_text(encoding="utf-8")
        self.assertIn("SDL_EVENT_AUDIO_DEVICE_ADDED", device_c)
        self.assertIn("SDL_EVENT_AUDIO_DEVICE_REMOVED", device_c)
        self.assertIn("SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED", device_c)
        self.assertIn("audio_notify_device_change();", device_c)
        self.assertIn("audio_select_device(dn);", menu_vk)
        self.assertIn("audio_select_channels(am);", menu_vk)

    def test_settings_menu_and_launcher_persist_audio_channels(self):
        settings_h = (ROOT / "runtime" / "settings.h").read_text(encoding="utf-8")
        settings_ini = (ROOT / "runtime" / "host" / "settings_ini.c").read_text(encoding="utf-8")
        menu_vk = (ROOT / "runtime" / "ui" / "menu_vk.cpp").read_text(encoding="utf-8")
        launcher = (ROOT / "tools" / "launcher" / "launcher.cpp").read_text(encoding="utf-8")

        self.assertIn("int audio_channels;", settings_h)
        self.assertIn('"audio_channels"', settings_ini)
        self.assertIn("audio_out_parse(v)", settings_ini)
        self.assertIn("g_settings.audio_channels", menu_vk)
        self.assertIn("audio_out_parse(v)", launcher)
        self.assertIn("g.audio_channels", launcher)

    def test_recomplauncher_exposes_audio_channels(self):
        manifest = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["config"][0]["new"].get("audio_channels"), "auto")
        setting = next((s for s in manifest["settings"] if s["id"] == "audio_channels"), None)
        self.assertIsNotNone(setting)
        values = [c["set"]["audio_channels"] for c in setting["choices"]]
        self.assertEqual(values, ["auto", "stereo", "5.1"])


if __name__ == "__main__":
    unittest.main()
