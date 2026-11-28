#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call)                                             \
    do {                                                        \
        VkResult status = (call);                               \
        if (status != VK_SUCCESS) {                             \
            fprintf(stderr, "%s returned %d\n", #call, status); \
            exit(1);                                            \
        }                                                       \
    } while (0)

static VkShaderModule load(VkDevice dev, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp || fseek(fp, 0, SEEK_END))
        exit(1);
    long size = ftell(fp);
    if (size < 20 || size % 4 || fseek(fp, 0, SEEK_SET))
        exit(1);
    uint32_t *code = malloc(size);
    if (!code || fread(code, 1, size, fp) != (size_t)size)
        exit(1);
    fclose(fp);
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(dev, &info, NULL, &module));
    free(code);
    return module;
}

static void save(const char *name, const void *buf, size_t len)
{
    FILE *fp = fopen(name, "wb");
    if (!fp || fwrite(buf, 1, len, fp) != len || fclose(fp))
        exit(1);
    printf("%s %zu bytes\n", name, len);
}

#include "stats.h"
#include <case.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: pipeline vertex.spv fragment.spv\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo inst = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&inst, NULL, &instance));
    VkPhysicalDevice gpu;
    uint32_t count = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &gpu));
    if (!count)
        return 1;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("device %s %x:%x driver %x\n", props.deviceName, props.vendorID, props.deviceID, props.driverVersion);
    if (props.vendorID != 0x10de || !strstr(props.deviceName, "Tegra X1"))
        return 1;
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
    const char *ext[] = {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
                         VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT lock = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
        .fragmentShaderPixelInterlock = INTERLOCK};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = INTERLOCK ? &lock : NULL,
                                            .shaderFloat16 = 1,
                                            .uniformBufferStandardLayout = 1,
                                            .shaderInt8 = 1,
                                            .storageBuffer8BitAccess = 1};
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12, .storageBuffer16BitAccess = 1};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
        .pNext = &f11,
        .pipelineExecutableInfo = VK_TRUE};
    VkPhysicalDeviceFeatures need = {.shaderInt16 = 1,
                                     .independentBlend = 1,
                                     .fragmentStoresAndAtomics = 1,
                                     .shaderStorageImageMultisample = 1,
                                     .shaderStorageImageReadWithoutFormat = 1,
                                     .shaderStorageImageWriteWithoutFormat = 1,
                                     .sampleRateShading = 1};
    device_info.pEnabledFeatures = &need;
    device_info.pNext = &feature;
    device_info.enabledExtensionCount = INTERLOCK ? 2 : 1;
    device_info.ppEnabledExtensionNames = ext;
    VkDevice d;
    CHECK(vkCreateDevice(gpu, &device_info, NULL, &d));
    VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                  .module = load(d, argv[1]),
                                                  .pName = "main"},
                                                 {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                  .module = load(d, argv[2]),
                                                  .pName = "main"}};
    for (unsigned i = 0; i < 2; i++)
        if (specs[i].mapEntryCount)
            stages[i].pSpecializationInfo = &specs[i];
    VkAttachmentDescription attachments[9] = {0};
    VkAttachmentReference refs[8];
    for (unsigned i = 0; i < NUM_COLORS; i++) {
        attachments[i] = (VkAttachmentDescription){.format = color_formats[i],
                                                   .samples = SAMPLES,
                                                   .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                   .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                   .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                   .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                   .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        refs[i] = (VkAttachmentReference){.attachment = (STORAGE_MASK & (1u << i)) ? VK_ATTACHMENT_UNUSED : i,
                                          .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkAttachmentReference depth = {.attachment = NUM_COLORS,
                                   .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if (DEPTH_FORMAT) {
        attachments[NUM_COLORS] = attachments[0];
        attachments[NUM_COLORS].format = DEPTH_FORMAT;
        attachments[NUM_COLORS].finalLayout = depth.layout;
    }
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = NUM_COLORS,
                                .pColorAttachments = refs,
                                .pDepthStencilAttachment = DEPTH_FORMAT ? &depth : NULL};
    VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = NUM_COLORS + !!DEPTH_FORMAT,
                                 .pAttachments = attachments,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub};
    VkRenderPass pass;
    CHECK(vkCreateRenderPass(d, &rp, NULL, &pass));
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    for (unsigned i = 0; i < NUM_SAMPLERS; i++)
        CHECK(vkCreateSampler(d, &sampler_states[i], NULL, &immutable_handles[i]));
    VkDescriptorSetLayout sets[32] = {0};
    for (unsigned i = 0; i < NUM_SETS; i++)
        CHECK(vkCreateDescriptorSetLayout(d, &layouts[i], NULL, &sets[i]));
    li.setLayoutCount = NUM_SETS;
    li.pSetLayouts = sets;
    li.pushConstantRangeCount = NUM_PUSH;
    li.pPushConstantRanges = ranges;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(d, &li, NULL, &layout));
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = NUM_BINDINGS,
                                               .pVertexBindingDescriptions = vertex_bindings,
                                               .vertexAttributeDescriptionCount = NUM_ATTRS,
                                               .pVertexAttributeDescriptions = vertex_attrs};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, 16, 16, 0, 1};
    VkRect2D scissor = {{0, 0}, {16, 16}};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1,
                                            .pViewports = &vp,
                                            .scissorCount = 1,
                                            .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = SAMPLES,
                                               .sampleShadingEnable = SAMPLE_SHADING,
                                               .minSampleShading = SAMPLE_SHADING};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = NUM_COLORS,
                                              .pAttachments = color_blends};
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache;
    CHECK(vkCreatePipelineCache(d, &ci, NULL, &cache));
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vs,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb,
                                       .layout = layout,
                                       .renderPass = pass};
    pi.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    VkPipeline pipeline;
    CHECK(vkCreateGraphicsPipelines(d, cache, 1, &pi, NULL, &pipeline));
    puts("vkCreateGraphicsPipelines success");
    report_stats(d, pipeline);
    size_t n = 0;
    CHECK(vkGetPipelineCacheData(d, cache, &n, NULL));
    void *data = malloc(n);
    CHECK(vkGetPipelineCacheData(d, cache, &n, data));
    save("pipeline.cache", data, n);
    free(data);
    vkDestroyPipeline(d, pipeline, NULL);
    vkDestroyPipelineCache(d, cache, NULL);
    vkDestroyPipelineLayout(d, layout, NULL);
    vkDestroyRenderPass(d, pass, NULL);
    for (unsigned i = 0; i < 2; i++)
        vkDestroyShaderModule(d, stages[i].module, NULL);
    for (unsigned i = 0; i < NUM_SETS; i++)
        vkDestroyDescriptorSetLayout(d, sets[i], NULL);
    for (unsigned i = 0; i < NUM_SAMPLERS; i++)
        vkDestroySampler(d, immutable_handles[i], NULL);
    vkDestroyDevice(d, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
