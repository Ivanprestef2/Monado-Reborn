// Present helpers and post-processing passes (SMAA 1x + AMD FSR 1 EASU/RCAS + screenshot capture).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
// [XCDE] T-LINUX-THREE: the post/present passes used to live in exec.c, whose vkq.h redirection sends every recording
// call through the ordered "vk-record" worker. Split into this unit they went straight to the driver from the render
// thread while that worker was replaying into the same command buffer: the present draw raced it (black game image) and
// the command stream collapsed (free(): invalid size inside vkBeginCommandBuffer). Include it after vk.h, as exec.c does.
#include "vkq.h"
// A unit that records after this one splitting again without the header compiles and renders black: fail at build time.
#if (defined(__linux__) || defined(_WIN32)) && !defined(vkCmdDraw)
#error "runtime/nvn/vulkan/post.c must include vkq.h after vk.h: its recording has to go through the ordered worker"
#endif
#include "temporal.h"
#include "../renderer.h"
#include "../../settings.h"
#include "../../env_cache.h"  // [XCDE] cached getenv

void vk_watch(VkImage dst, const char *site);
uint64_t renderer_present_count(void);

static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .flags = g_vk.cmd_push_descriptor_set ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                              .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].fs == fs) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .flags = g_vk.cmd_push_descriptor_set ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                              .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[7];  // (5: T213 the upscaled scene with the HUD; 6: the native upscalers' sRGB-encoded output)
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vk_watch(d->image, "post.c:smaa_lut vkCmdCopyBufferToImage"); vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// [XCDE] Perf: scoped barrier for post-processing passes instead of vk_full_barrier (ALL_COMMANDS -> ALL_COMMANDS).
static void post_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void post_bind_samplers(VkCommandBuffer cb, VkPipelineLayout layout, VkDescriptorSetLayout set_layout,
                               const VkImageView *srcs, int n) {
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    if (g_vk.cmd_push_descriptor_set) {
        q_vkCmdPushDescriptorSetKHR(g_vk.cmd_push_descriptor_set, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, (uint32_t)n, w);
        return;
    }
    VkDescriptorSet ds = vk_alloc_set(set_layout);
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vk_bind_graphics_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ds, 0, NULL);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    post_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    post_bind_samplers(cb, post_layout, post_set_layout, srcs, n);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView fg_scene;  // [XCDE] this frame's upscaled scene before the HUD composite (frame generation's HUD-less copy)
float vk_frame_clear[4];       // (device.c: the window image's clear colour this frame)
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    fg_scene = VK_NULL_HANDLE;
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool tmp_on = tmp_enabled();
    bool fsr = (g_settings.upscaler == UPSCALE_FSR1 || (!tmp_on && g_settings.upscaler == UPSCALE_FSR3)) &&
               (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only; FSR 3 falls back to FSR 1 when unavailable)
    bool upscaled = false;  // [XCDE] v12 #29: v is already at the output size (the SMAA below then runs at ow x oh)
    // [XCDE] v12 #29: XCDE_SMAA_AFTER_UPSCALE=0 restores the behaviour before the change (no SMAA under FSR 3 / DLSS,
    // SMAA at the render resolution under FSR 1), so a tester can A/B without rebuilding.
    static int smaa_after = -1;
    if (smaa_after < 0) { const char *e = getenv("XCDE_SMAA_AFTER_UPSCALE"); smaa_after = e ? atoi(e) != 0 : 1; }
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    if (tmp_on) {  // [XCDE] FSR 3 / DLSS (temporal.c); without this frame's camera: FSR 1 as the upscaler
        const TmpCamera *tmp_cam = NULL;
        uint64_t tmp_frame = ~0ull;
        TexBackend *tmp_depth = NULL;
        const float *tmp_jit = NULL;
        bool tmp_dm1 = true;
        vk_tmp_frame_state(&tmp_cam, &tmp_frame, &tmp_depth, &tmp_jit, &tmp_dm1);
        bool cam = tmp_cam->ok && tmp_frame == renderer_present_count() && tmp_depth;
        vk_frame_suspend();
        vk_tex_transition(cb, b, VK_IMAGE_LAYOUT_GENERAL);
        if (cam && tmp_depth) vk_tex_transition(cb, tmp_depth, VK_IMAGE_LAYOUT_GENERAL);
        VkImageView t = tmp_upscale(cb, b, v, cam ? tmp_depth : NULL, cam ? tmp_cam : NULL, tmp_jit, tmp_dm1, ow, oh);
        extern VkImageView tmp_fg_hudless_target(void);
        VkImageView scene = t ? tmp_hud_scene_view(b->width, b->height) : VK_NULL_HANDLE;
        bool fuse_srgb = false;
        bool need_srgb = t && tmp_uses_linear_fp16();
        fuse_srgb = need_srgb && scene && !tmp_fg_hudless_target();
        if (need_srgb && !fuse_srgb) {  // [XCDE] linear -> sRGB
            vk_gp_mark(cb, "post: tmp_srgb %ux%u", ow, oh);
            TexBackend *encoded = post_image(6, ow, oh);
            float pe[8] = {1.0f / ow, 1.0f / oh};
            post_pass(cb, "tmp_srgb.frag", &t, 1, encoded, pe, false);
            t = post_view(encoded);
        }
        if (scene) {  // [XCDE] T213: the upscaled scene + what the HUD passes added (render size, bilinear)
            vk_gp_mark(cb, "post: tmp_hud %ux%u", ow, oh);
            fg_scene = t;
            TexBackend *hud = post_image(5, ow, oh);
            // [XCDE] GitHub #103: b.x = XCDE_TMP_HUD_OLD (tmp_hud.frag: the composite before #103)
            static int hud_old = -1;
            if (hud_old < 0) { const char *e = getenv("XCDE_TMP_HUD_OLD"); hud_old = e && *e && *e != '0'; }
            float ph[8] = {1.0f / ow, 1.0f / oh, 1.0f / b->width, 1.0f / b->height, (float)hud_old, fuse_srgb ? 1.0f : 0.0f};
            VkImageView hi[3] = {t, v, scene};
            post_pass(cb, "tmp_hud.frag", hi, 3, hud, ph, false);
            t = post_view(hud);
        }
        if ((need_srgb && !fuse_srgb) || scene) post_barrier(cb);
        vk_frame_resume();
        // [XCDE] v12 #29: the SMAA runs AFTER the upscaler (with FSR 3 / DLSS the early return made the SMAA block dead
        // code: toggling it changed nothing). The upscaled view is kept and the SMAA blends it at the output size.
        // fg_scene was taken before the HUD composite and is untouched: frame generation keeps drawing the scaled
        // scene without SMAA (it interpolates, it does not re-antialias).
        if (t && smaa_after) {
            v = t;
            fsr = false;      // (already scaled)
            upscaled = true;
        } else if (t) {
            return t;         // (XCDE_SMAA_AFTER_UPSCALE=0: the behaviour before this change)
        } else {
            smaa = false;
            fsr = ow > b->width + 1 || oh > b->height + 1;
        }
    }
    if (!smaa && !fsr) {
        if (b->cur_layout != VK_IMAGE_LAYOUT_GENERAL) {
            vk_frame_suspend();
            vk_tex_transition(cb, b, VK_IMAGE_LAYOUT_GENERAL);
            vk_frame_resume();
        }
        return v;
    }
    vk_frame_suspend();
    vk_tex_transition(cb, b, VK_IMAGE_LAYOUT_GENERAL);
    if (fsr) {
        vk_gp_mark(cb, "post: fsr1_easu %ux%u -> %ux%u", b->width, b->height, ow, oh);
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        v = post_view(e);
        float sharp = xcde_sharpness(&g_settings);  // [XCDE] RCAS strength (default 0.9 = the previous 0.2 stops); 0 = off
        if (sharp > 0.0f) {
            vk_gp_mark(cb, "post: fsr1_rcas %ux%u", ow, oh);
            float pr[8] = {exp2f(-(2.0f - 2.0f * sharp)), 0, 0, 0, (float)ow, (float)oh};
            VkImageView ri_[3] = {v};
            post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
            v = post_view(r);
        }
        upscaled = true;  // [XCDE] v12 #29: the SMAA below then runs at the output size, after the RCAS sharpening
    }
    // [XCDE] v12 #29: the SMAA moved AFTER the upscaler: at the render resolution it only saw the stairs that exist
    // there and the RCAS / upscaler rebuilt the hard edge right after it. Over the upscaled image (issue #29) it
    // blends the edges at the size they are shown at.
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        uint32_t sw = upscaled ? ow : b->width, sh = upscaled ? oh : b->height;
        vk_gp_mark(cb, "post: smaa %ux%u", sw, sh);
        float w = (float)sw, h = (float)sh;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, sw, sh), *weights = post_image(1, sw, sh);
        TexBackend *out = post_image(2, sw, sh);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    post_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1, pup = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs || (upscaled ? 1 : 0) != pup)
            LOG("[post] %ux%u -> %s%s%ux%u\n", b->width, b->height, fsr ? "fsr1 " : upscaled ? "upscaled " : "",
                smaa ? "smaa " : "", ow, oh);  // [XCDE] v12 #29: the order is the chain: upscaler first, SMAA after it
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr; pup = upscaled ? 1 : 0;
    }
    return v;
}

static VkImageView rp_src;  // (vk_present_redraw) the last final image and its placement
static VkViewport rp_vp;
static float rp_pv[8];
// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh);
    vk_gp_mark(cb, "post: present %ux%u", g_vk.sc_extent.width, g_vk.sc_extent.height);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    {   // [XCDE] FSR 3 frame generation: the same frame without the HUD, drawn the same way (temporal_fg.inc). Without a
        // HUD-less scene this frame (menus, no snapshot) the full image: no HUD is told apart, as before.
        extern VkImageView tmp_fg_hudless_target(void);
        extern void vk_frame_suspend(void);
        extern void vk_frame_resume(void);
        VkImageView hl = tmp_fg_hudless_target();
        if (hl) {
            vk_frame_suspend();
            VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = hl, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                            .clearValue.color.float32 = {vk_frame_clear[0], vk_frame_clear[1], vk_frame_clear[2], vk_frame_clear[3]}};
            VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                                  .colorAttachmentCount = 1, .pColorAttachments = &ca};
            vkCmdBeginRendering(cb, &ri);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            VkImageView hsrc = fg_scene ? fg_scene : src;
            post_bind_samplers(cb, present_layout, present_set_layout, &hsrc, 1);
            VkViewport hvp = {vx, vy, vw, vh, 0, 1};
            VkRect2D hsc = {{0, 0}, g_vk.sc_extent};
            vkCmdSetViewport(cb, 0, 1, &hvp);
            vkCmdSetScissor(cb, 0, 1, &hsc);
            vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            post_barrier(cb);
            vk_frame_resume();
        }
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    post_bind_samplers(cb, present_layout, present_set_layout, &src, 1);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
    rp_src = src; rp_vp = vp; memcpy(rp_pv, pv, sizeof rp_pv);
}

// [XCDE] device.c repeat presents (FPS 30 with V-Sync on a 60 Hz monitor): the last frame's final image drawn again the
// same way - no post-processing / upscaling run again (the temporal upscalers' history would take it as a frame).
void vk_present_redraw(VkCommandBuffer cb) {
    if (!rp_src) return;
    VkPipeline p = present_pipeline(g_vk.sc_format);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    post_bind_samplers(cb, present_layout, present_set_layout, &rp_src, 1);
    vkCmdSetViewport(cb, 0, 1, &rp_vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, rp_pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) {  // [XCDE] say why nothing is saved
        LOG("[vk] screenshot %s skipped: %s\n", path, !b ? "no texture" : !b->image ? "no image" : "not a color image");
        return;
    }
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    vk_tex_transition(vk_cmd(), b, VK_IMAGE_LAYOUT_GENERAL);
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkImageView sv = raw_view(b);
    post_bind_samplers(cb, present_layout, present_set_layout, &sv, 1);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    if (path[0] == '#') {  // [XCDE] T119: "#<label>": log a hash of the image instead of saving it (XCDE_PASS_HASH)
        uint64_t hv = 0xcbf29ce484222325ull;
        for (uint64_t i = 0; i < (uint64_t)w * h * 4; i++) hv = (hv ^ ((const uint8_t *)out.ptr)[i]) * 0x100000001b3ull;
        LOG("[passhash] %s %016llx\n", path + 1, (unsigned long long)hv);
    } else
        vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}
