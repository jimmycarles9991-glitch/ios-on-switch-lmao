#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#define SWITCH_VK_CHECK(expr)                                \
    do {                                                     \
        VkResult err = (expr);                               \
        if (err != VK_SUCCESS) {                             \
            fprintf(stderr, "%s returned %d\n", #expr, err); \
            exit(2);                                         \
        }                                                    \
    } while (0)
#include "resources.h"
#include "glow.h"

static bool switch_supports_combination(VkInstance instance, VkPhysicalDevice gpu, VkCoverageReductionModeNV mode,
                                        uint32_t raster, uint32_t depth, uint32_t color)
{
    PFN_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV query =
        (PFN_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV");
    if (!query)
        exit(2);
    uint32_t n = 0;
    SWITCH_VK_CHECK(query(gpu, &n, NULL));
    VkFramebufferMixedSamplesCombinationNV *rows = calloc(n, sizeof *rows);
    for (uint32_t i = 0; i < n; i++)
        rows[i].sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_MIXED_SAMPLES_COMBINATION_NV;
    SWITCH_VK_CHECK(query(gpu, &n, rows));
    printf("combination count=%u\n", n);
    bool match = false;
    for (uint32_t i = 0; i < n; i++) {
        printf("combination mode=%d raster=%#x depthMask=%#x colorMask=%#x\n", rows[i].coverageReductionMode,
               rows[i].rasterizationSamples, rows[i].depthStencilSamples, rows[i].colorSamples);
        if (rows[i].coverageReductionMode == mode && rows[i].rasterizationSamples == raster &&
            (rows[i].depthStencilSamples & depth) && (rows[i].colorSamples & color))
            match = true;
    }
    free(rows);
    return match;
}

int main(int argc, char **argv)
{
    if (argc != 8) {
        fprintf(stderr, "usage: probe CASE RASTER COLOR SAMPLE_SHADING MASK VERT.spv FRAG.spv\n");
        return 2;
    }
    const char *name = argv[1];
    bool glow = !strcmp(name, "glow"), mixed = !strcmp(name, "merge") || !strcmp(name, "truncate");
    bool bbox = !strcmp(name, "bbox");
    VkSampleCountFlagBits raster = atoi(argv[2]), color = atoi(argv[3]);
    bool per_sample = atoi(argv[4]);
    VkSampleMask mask = strtoul(argv[5], NULL, 0);
    VkCoverageReductionModeNV reduction =
        !strcmp(name, "truncate") ? VK_COVERAGE_REDUCTION_MODE_TRUNCATE_NV : VK_COVERAGE_REDUCTION_MODE_MERGE_NV;
    setvbuf(stdout, NULL, _IONBF, 0);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo inst = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    SWITCH_VK_CHECK(vkCreateInstance(&inst, NULL, &instance));
    VkPhysicalDevice gpu;
    uint32_t n = 1;
    SWITCH_VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, &gpu));
    if (n != 1)
        return 2;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("device %s %x:%x driver %x\n", props.deviceName, props.vendorID, props.deviceID, props.driverVersion);
    if (props.vendorID != 0x10de || !strstr(props.deviceName, "Tegra X1"))
        return 2;
    const char *extensions[] = {VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME,
                                VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME,
                                VK_NV_COVERAGE_REDUCTION_MODE_EXTENSION_NAME, VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME,
                                VK_NV_FILL_RECTANGLE_EXTENSION_NAME};
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
        .fragmentShaderPixelInterlock = 1,
        .fragmentShaderSampleInterlock = 1};
    VkPhysicalDeviceCoverageReductionModeFeaturesNV coverage = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COVERAGE_REDUCTION_MODE_FEATURES_NV,
        .coverageReductionMode = 1,
        .pNext = &interlock};
    VkPhysicalDeviceCoverageReductionModeFeaturesNV supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COVERAGE_REDUCTION_MODE_FEATURES_NV};
    VkPhysicalDeviceFeatures2 supported_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                                    .pNext = &supported};
    vkGetPhysicalDeviceFeatures2(gpu, &supported_features);
    printf("coverageReductionMode=%u\n", supported.coverageReductionMode);
    if (mixed && !switch_supports_combination(instance, gpu, reduction, raster, raster, color)) {
        printf("unsupported combination mode=%d raster=%u depth=%u color=%u\n", reduction, raster, raster, color);
        _exit(3);
    }
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &coverage,
                                            .shaderFloat16 = 1,
                                            .uniformBufferStandardLayout = 1,
                                            .shaderInt8 = 1,
                                            .storageBuffer8BitAccess = 1};
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12, .storageBuffer16BitAccess = 1};
    VkPhysicalDeviceFeatures features = {.fragmentStoresAndAtomics = 1,
                                         .shaderStorageImageMultisample = 1,
                                         .shaderStorageImageReadWithoutFormat = 1,
                                         .shaderStorageImageWriteWithoutFormat = 1,
                                         .shaderInt16 = 1,
                                         .fillModeNonSolid = 1,
                                         .sampleRateShading = 1};
    VkDeviceCreateInfo dev = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &queue,
                              .pNext = &f11,
                              .pEnabledFeatures = &features,
                              .enabledExtensionCount = sizeof extensions / sizeof extensions[0],
                              .ppEnabledExtensionNames = extensions};
    VkDevice d;
    SWITCH_VK_CHECK(vkCreateDevice(gpu, &dev, NULL, &d));
    VkFormat cf = glow ? color_formats[0] : VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat df = glow ? DEPTH_FORMAT : VK_FORMAT_D32_SFLOAT;
    VkAttachmentDescription attachments[2] = {{.format = cf,
                                               .samples = color,
                                               .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                               .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                              {.format = df,
                                               .samples = raster,
                                               .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                               .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference cr = {glow ? VK_ATTACHMENT_UNUSED : 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                          zr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &cr,
                                .pDepthStencilAttachment = &zr};
    VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 2,
                                 .pAttachments = attachments,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub};
    VkRenderPass pass;
    SWITCH_VK_CHECK(vkCreateRenderPass(d, &rp, NULL, &pass));
    VkImageView views[] = {
        switch_create_image(d, cf, color, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, NULL),
        switch_create_image(d, df, raster, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT | (glow ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), NULL)};
    VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = pass,
                                  .attachmentCount = 2,
                                  .pAttachments = views,
                                  .width = 16,
                                  .height = 16,
                                  .layers = 1};
    VkFramebuffer framebuffer;
    SWITCH_VK_CHECK(vkCreateFramebuffer(d, &fb, NULL, &framebuffer));
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo set = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    VkDescriptorSetLayout setlayout;
    const VkDescriptorSetLayoutCreateInfo *descriptor_layout = glow ? &layouts[0] : &set;
    SWITCH_VK_CHECK(vkCreateDescriptorSetLayout(d, descriptor_layout, NULL, &setlayout));
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &setlayout,
                                     .pushConstantRangeCount = glow ? NUM_PUSH : 0,
                                     .pPushConstantRanges = ranges};
    VkPipelineLayout layout;
    SWITCH_VK_CHECK(vkCreatePipelineLayout(d, &li, NULL, &layout));
    VkPipelineShaderStageCreateInfo stages[] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                 .pName = "main",
                                                 .module = switch_load_module(d, argv[6])},
                                                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                 .pName = "main",
                                                 .module = switch_load_module(d, argv[7])}};
    for (unsigned i = 0; i < 2; i++)
        if (glow && specs[i].mapEntryCount)
            stages[i].pSpecializationInfo = &specs[i];
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = glow ? NUM_BINDINGS : 0,
                                               .pVertexBindingDescriptions = vertex_bindings,
                                               .vertexAttributeDescriptionCount = glow ? NUM_ATTRS : 0,
                                               .pVertexAttributeDescriptions = vertex_attrs};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, 16, 16, 0, 1};
    VkRect2D rect = {{0, 0}, {16, 16}};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1,
                                            .pViewports = &viewport,
                                            .scissorCount = 1,
                                            .pScissors = &rect};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode =
                                                     bbox ? VK_POLYGON_MODE_FILL_RECTANGLE_NV : VK_POLYGON_MODE_FILL,
                                                 .lineWidth = 1};
    VkPipelineCoverageReductionStateCreateInfoNV reduce = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_REDUCTION_STATE_CREATE_INFO_NV,
        .coverageReductionMode = reduction};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .pNext = mixed ? &reduce : NULL,
                                               .rasterizationSamples = raster,
                                               .sampleShadingEnable = per_sample,
                                               .minSampleShading = per_sample ? 1 : 0,
                                               .pSampleMask = &mask};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = !glow,
                                                .depthWriteEnable = !glow,
                                                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb,
                                       .layout = layout,
                                       .renderPass = pass};
    VkPipeline pipeline;
    SWITCH_VK_CHECK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline));
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool commands;
    SWITCH_VK_CHECK(vkCreateCommandPool(d, &pool, NULL, &commands));
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commands, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    SWITCH_VK_CHECK(vkAllocateCommandBuffers(d, &alloc, &cmd));
    VkCommandBufferBeginInfo start = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkBuffer buf = switch_create_buffer(d);
    VkImage raw_storage = VK_NULL_HANDLE;
    VkImageView storage = glow ? switch_create_image(d, VK_FORMAT_R32_UINT, raster, VK_IMAGE_USAGE_STORAGE_BIT,
                                                     VK_IMAGE_ASPECT_COLOR_BIT, &raw_storage)
                               : VK_NULL_HANDLE;
    VkDescriptorSet bound = switch_bind_resources(d, setlayout, buf, storage, descriptor_layout);
    SWITCH_VK_CHECK(vkBeginCommandBuffer(cmd, &start));
    if (glow) {
        VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = raw_storage,
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                             0, NULL, 1, &barrier);
    }
    VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   .renderPass = pass,
                                   .framebuffer = framebuffer,
                                   .renderArea = rect};
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &bound, 0, NULL);
    if (glow) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
        uint32_t push[32] = {};
        vkCmdPushConstants(cmd, layout, ranges[0].stageFlags, ranges[0].offset, ranges[0].size, push);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    SWITCH_VK_CHECK(vkEndCommandBuffer(cmd));
    VkQueue q;
    vkGetDeviceQueue(d, 0, 0, &q);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    printf("draw case=%s raster=%u color=%u shade=%d mask=%#x\n", name, raster, color, per_sample, mask);
    void (*mark_submit)(void) = dlsym(RTLD_DEFAULT, "switch_mark_draw_submit");
    if (!mark_submit)
        return 2;
    mark_submit();
    SWITCH_VK_CHECK(vkQueueSubmit(q, 1, &submit, VK_NULL_HANDLE));
    puts("vkQueueSubmit returned VK_SUCCESS");
    _exit(0);
}
