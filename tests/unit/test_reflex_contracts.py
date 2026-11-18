"""Source-level contracts for the opt-in NVIDIA Reflex integration (T272).

No GPU or vendor DLL is needed: these checks keep the frame-token/marker plumbing, the setting and the deployment
lists from drifting apart silently.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding='utf-8')


class ReflexContracts(unittest.TestCase):
    def test_markers_and_token_are_shared_with_dlss(self):
        stream = read('runtime/nvn/vulkan/dlss.cpp')
        self.assertIn('sl::kFeatureReflex', stream)
        self.assertIn('sl::kFeaturePCL', stream)
        self.assertIn('slReflexSleep', stream)
        self.assertIn('slPCLSetMarker', stream)
        self.assertIn('token_for(', stream)
        self.assertIn('g_present_frame', stream)
        # the extension comes from slGetFeatureRequirements, not a hardcoded device extension
        self.assertIn('slGetFeatureRequirements', stream)
        self.assertNotIn('VK_NV_low_latency', stream)

    def test_frame_loop_marks_simulation_and_present(self):
        pace = read('runtime/hle/framepace.c')
        self.assertIn('reflex_frame_begin()', pace)
        self.assertIn('REFLEX_SIM_START', pace)
        self.assertIn('reflex_begun = false', pace)
        presents = read('runtime/nvn/objects.c')
        self.assertIn('REFLEX_SIM_END', presents)
        self.assertIn('REFLEX_SUBMIT_START', presents)
        self.assertIn('reflex_current_frame()', presents)
        device = read('runtime/nvn/vulkan/device.c')
        self.assertIn('reflex_present_marker(REFLEX_PRESENT_START)', device)
        self.assertIn('reflex_present_marker(REFLEX_PRESENT_END)', device)
        worker = read('runtime/nvn/vulkan/exec.c')
        self.assertIn('reflex_set_present_frame(j.frame)', worker)
        self.assertIn('reflex_clear_present_frame()', worker)

    def test_setting_round_trips_in_both_writers(self):
        header = read('runtime/settings.h')
        self.assertIn('int reflex;', header)
        ini = read('runtime/host/settings_ini.c')
        self.assertIn('"reflex"', ini)
        self.assertIn('"boost"', ini)
        launcher = read('tools/launcher/launcher.cpp')
        self.assertIn('"reflex"', launcher)
        self.assertIn('sl.reflex.dll', launcher)
        menu = read('runtime/ui/menu_vk.cpp')
        self.assertIn('reflex_set_mode', menu)
        self.assertIn('reflex_status()', menu)

    def test_vendor_runtime_is_deployed(self):
        for path in ('cmake/copy_upscaler_dlls.cmake', 'scripts/copy_upscaler_dlls.sh'):
            text = read(path)
            self.assertIn('sl.reflex.dll', text)
            self.assertIn('sl.pcl.dll', text)
            self.assertIn('NvLowLatencyVk.dll', text)
        installer = read('tools/install_dlss.py')
        for name in ('sl.reflex.dll', 'sl.pcl.dll', 'NvLowLatencyVk.dll'):
            self.assertIn(name, installer)


if __name__ == '__main__':
    unittest.main()
