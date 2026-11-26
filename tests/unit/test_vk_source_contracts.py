"""Source-level regression checks for Vulkan capability selection."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
DEVICE = (ROOT / "runtime/nvn/vulkan/device.c").read_text(encoding="utf-8")


class VulkanSourceContracts(unittest.TestCase):
    def test_validation_fallback_removes_debug_extension_by_name(self):
        self.assertIn("strcmp(ext[i], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != 0", DEVICE)
        self.assertNotIn("ci.enabledExtensionCount--;", DEVICE)

    def test_optional_feature_bits_are_copied_from_physical_device_support(self):
        self.assertIn(".shaderInt8 = h12.shaderInt8", DEVICE)
        self.assertIn(".storageBuffer8BitAccess = h12.storageBuffer8BitAccess", DEVICE)
        self.assertIn(".scalarBlockLayout = h12.scalarBlockLayout", DEVICE)
        self.assertIn(".storageBuffer16BitAccess = h11.storageBuffer16BitAccess", DEVICE)
        self.assertNotIn(".shaderInt8 = VK_TRUE", DEVICE)
        self.assertNotIn(".storageBuffer16BitAccess = VK_TRUE", DEVICE)

    def test_backend_required_features_fail_with_explicit_diagnostic(self):
        self.assertIn("!h13.dynamicRendering || !h13.synchronization2", DEVICE)
        self.assertIn("!h12.timelineSemaphore", DEVICE)

    def test_automatic_selection_filters_devices_by_queue_capabilities(self):
        probe = (ROOT / "runtime/nvn/vulkan/gpu_probe.h").read_text(encoding="utf-8")
        self.assertIn("xcde_gpu_enumerate", DEVICE)
        self.assertIn("VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT", probe)
        self.assertIn("vkGetPhysicalDeviceSurfaceSupportKHR", probe)
        self.assertIn("LARECOMP_GPU", DEVICE)

    def test_pipeline_cache_input_is_bounded_and_allocation_checked(self):
        self.assertIn("end <= PC_CACHE_MAX", DEVICE)
        self.assertIn("#define PC_CACHE_MAX (192L * 1024L * 1024L)", DEVICE)
        self.assertIn("if (!data || fread(data, 1, sz, fp) != sz)", DEVICE)
        self.assertIn("pipeline cache ignored: invalid size", DEVICE)

    def test_pipeline_cache_save_checks_size_and_allocation(self):
        self.assertIn("if (sz > (size_t)PC_CACHE_MAX)", DEVICE)
        self.assertIn("pipeline cache not saved: out of memory", DEVICE)

    def test_vulkan_memory_budget_extension_is_opt_in_setting(self):
        self.assertIn("g_settings.gpu_memory_budget && has_ext", DEVICE)
        menu = (ROOT / "runtime/ui/menu_vk.cpp").read_text(encoding="utf-8")
        self.assertIn("vmaGetHeapBudgets(g_vk.vma, budgets)", menu)
        self.assertIn("VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU", menu)


    def test_optional_feature_log_is_after_feature_struct_initialization(self):
        self.assertLess(DEVICE.index("VkPhysicalDeviceVulkan11Features f11 ="),
                        DEVICE.index("[vk] optional features:"))

    def test_integrated_gpu_memory_is_not_assumed_to_be_dedicated_vram(self):
        self.assertIn("VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT", DEVICE)
        self.assertIn("shared_bytes", DEVICE)
        self.assertIn("DEVICE_LOCAL alone does not mean dedicated VRAM", DEVICE)

    def test_extension_feature_bits_are_queried_before_enable(self):
        self.assertIn("cbc_have.customBorderColors", DEVICE)
        self.assertIn("div_have.vertexAttributeInstanceRateDivisor", DEVICE)
        self.assertIn("ffhave.deviceFault", DEVICE)

    def test_manual_gpu_policy_is_persisted_and_used(self):
        settings = (ROOT / "runtime/host/settings_ini.c").read_text(encoding="utf-8")
        header = (ROOT / "runtime/settings.h").read_text(encoding="utf-8")
        self.assertIn('"gpu_index"', settings)
        self.assertIn('"gpu_profile"', settings)
        self.assertIn('"gpu_gpl"', settings)
        self.assertIn('"gpu_memory_budget"', settings)
        self.assertIn('"amd_fix"', settings)
        self.assertIn('"session_fix"', settings)
        self.assertIn("AMD_FIX_AUTO", header)
        self.assertIn("g_settings.gpu_index", DEVICE)
        self.assertIn("GPU_PROFILE_COMPATIBILITY", header)

    def test_gpu_menu_exposes_restart_scoped_controls_and_diagnostics(self):
        menu = (ROOT / "runtime/ui/menu_vk.cpp").read_text(encoding="utf-8")
        self.assertIn('BeginTabItem(T("GPU / Vulkan"))', menu)
        self.assertIn("g_vk.props.deviceName", menu)
        self.assertIn("g_settings.gpu_memory_budget", menu)
        self.assertIn("g_vk.driver_props.driverName", menu)
        self.assertIn("These changes require restarting the game", menu)
        temporal = (ROOT / "runtime/nvn/vulkan/temporal_fg.inc").read_text(encoding="utf-8")
        self.assertIn("g_settings.gpu_profile != GPU_PROFILE_COMPATIBILITY", temporal)
        self.assertIn("g_settings.gpu_profile != GPU_PROFILE_COMPATIBILITY", DEVICE)

    def test_scalable_resize_preserves_gpu_contents_and_clears_new_color_rts(self):
        textures = (ROOT / "runtime/nvn/vulkan/textures.c").read_text(encoding="utf-8")
        fn_start = textures.index("static void resize_backend_if_needed(")
        fn_end = textures.index("static TexBackend *tex_backend(", fn_start)
        resize_fn = textures[fn_start:fn_end]
        blit_idx = resize_fn.index("vkCmdBlitImage(cb, old.image, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL,")
        defer_idx = resize_fn.index("defer((Dead){.img = old.image, .mem = old.mem});")
        self.assertLess(blit_idx, defer_idx)
        self.assertIn("b->uploaded = false;", resize_fn)
        self.assertIn("xcde_tex_should_clear_new_color", textures)
        self.assertIn("vkCmdClearColorImage", textures)
        self.assertIn("XCDE_POISON_NEW_IMAGES", textures)

    def test_temporal_lod_bias_is_material_only_and_bucketed(self):
        textures = (ROOT / "runtime/nvn/vulkan/textures.c").read_text(encoding="utf-8")
        exec_c = (ROOT / "runtime/nvn/vulkan/exec.c").read_text(encoding="utf-8")
        self.assertIn("VkSampler variant[5][XCDE_SAMPLER_BIAS_SLOTS];", textures)
        self.assertIn("xcde_sampler_wants_lod_bias", exec_c)
        self.assertLess(exec_c.index("xcde_sampler_wants_lod_bias"), exec_c.index("vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc], lod_bias)"))

    def test_temporal_hud_snapshot_waits_for_scene_provenance_on_presented_rt(self):
        exec_c = (ROOT / "runtime/nvn/vulkan/exec.c").read_text(encoding="utf-8")
        vk_h = (ROOT / "runtime/nvn/vulkan/vk.h").read_text(encoding="utf-8")
        self.assertIn("uint64_t tmp_scene_frame;", vk_h)
        self.assertIn("tmp_presented_scene_frame", exec_c)
        self.assertIn("xcde_tmp_hud_snap_ready", exec_c)
        self.assertIn("xcde_tmp_should_split_before_hud_draw", exec_c)
        self.assertIn("xcde_tmp_tex_carries_scene", exec_c)

    def test_vendor_upscaler_sdks_are_not_unloaded_by_an_exit_handler(self):
        """GitHub #216: exit() runs the atexit handlers (reverse order) while the game's threads still render.

        dlss.cpp used to atexit its Streamline/NGX shutdown. That ran BEFORE the renderer's quick exit (registered
        later, so earlier by LIFO), released the plugins while a render thread was inside dlss_evaluate, and the next
        Streamline call jumped into the unloaded plugin: 0xc0000005 at an address inside no loaded module. The window
        close path ends the process through quick_exit_handler + plat_terminate (no DLL teardown), so the SDKs stay
        loaded; nothing may register a handler that unloads them.
        """
        dlss = (ROOT / "runtime/nvn/vulkan/dlss.cpp").read_text(encoding="utf-8")
        # (line comments stripped: the fix's own notes quote the removed calls)
        code = "\n".join(line.split("//")[0] for line in dlss.splitlines())
        self.assertNotIn("atexit(dlss_shutdown", code)        # Windows: Streamline (sl.interposer + plugins, NGX)
        self.assertNotIn("atexit(dlss_shutdown_linux", code)  # Linux: NGX component shared library
        self.assertIn("GitHub #216", dlss)
        self.assertIn("GitHub #216", DEVICE)
        self.assertIn("plat_terminate();", DEVICE)  # the process ends without the DLL teardown

    def test_fg_present_semaphores_hudless_ring_and_bounded_exit(self):
        """GitHub #265: FSR 3 FG present sync, ring-buffered HUDLessColor, and bounded exit wait."""
        temporal = (ROOT / "runtime/nvn/vulkan/temporal_fg.inc").read_text(encoding="utf-8")
        exec_c = (ROOT / "runtime/nvn/vulkan/exec.c").read_text(encoding="utf-8")
        self.assertIn("#define DONE_SEMS 16", DEVICE)
        self.assertIn("done_sem[DONE_SEMS]", DEVICE)
        self.assertIn("tmp_fg_wait_presents();", DEVICE)
        self.assertIn("backend_is_stalled()", DEVICE)
        self.assertIn("exit_watchdog_thread", DEVICE)
        self.assertIn("bool backend_is_stalled(void)", exec_c)
        self.assertIn("#define FG_HL_RING 3", temporal)
        self.assertIn("VK_IMAGE_USAGE_TRANSFER_SRC_BIT", temporal)
        self.assertIn("FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK", temporal)

    def test_char_light_patch_survives_structured_cfg(self):
        """GitHub #58 / #268: try_emit_structured() removes `switch (pc)` / `case 5400u:` labels.

        char_light_src() must anchor on the emitted epilogue instruction (inserting before it) so the
        local-light scaling applies in structured CFG, switch CFG, and annotated dumps alike.
        """
        exec_c = (ROOT / "runtime/nvn/vulkan/exec.c").read_text(encoding="utf-8")
        start = exec_c.index("static const char *char_light_src(")
        end = exec_c.index("static const char *tmaa_texel_src(", start)
        fn = exec_c[start:end]
        code = "\n".join(line.split("//")[0] for line in fn.splitlines())
        self.assertNotIn("case 5400u:", code)
        self.assertIn('static const char anchor[] = "    r2 = as_u((as_f(r40) * as_f(cb3[13][0])));\\n";', code)
        self.assertNotIn("at += sizeof anchor - 1;", code)
        self.assertIn("r41 = as_u(as_f(r41) * %sf);", code)


if __name__ == "__main__":
    unittest.main()
