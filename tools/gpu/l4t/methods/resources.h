static VkDeviceMemory switch_allocate_memory(VkDevice d, VkMemoryRequirements req)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = __builtin_ctz(req.memoryTypeBits)};
    VkDeviceMemory mem;
    SWITCH_VK_CHECK(vkAllocateMemory(d, &info, NULL, &mem));
    return mem;
}

static VkImageView switch_create_image(VkDevice d, VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                                       VkImageAspectFlags aspect, VkImage *raw)
{
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt,
                            .extent = {16, 16, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = samples,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage};
    VkImage im;
    SWITCH_VK_CHECK(vkCreateImage(d, &ci, NULL, &im));
    if (raw)
        *raw = im;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d, im, &req);
    SWITCH_VK_CHECK(vkBindImageMemory(d, im, switch_allocate_memory(d, req), 0));
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = im,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = fmt,
                                  .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkImageView v;
    SWITCH_VK_CHECK(vkCreateImageView(d, &view, NULL, &v));
    return v;
}

static VkBuffer switch_create_buffer(VkDevice d)
{
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = 4096,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer b;
    SWITCH_VK_CHECK(vkCreateBuffer(d, &ci, NULL, &b));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, b, &req);
    SWITCH_VK_CHECK(vkBindBufferMemory(d, b, switch_allocate_memory(d, req), 0));
    return b;
}

static VkShaderModule switch_load_module(VkDevice d, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END))
        exit(2);
    long n = ftell(f);
    if (n < 20 || n % 4 || fseek(f, 0, SEEK_SET))
        exit(2);
    uint32_t *words = malloc(n);
    if (!words || fread(words, 1, n, f) != (size_t)n || fclose(f))
        exit(2);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = words};
    VkShaderModule mod;
    SWITCH_VK_CHECK(vkCreateShaderModule(d, &ci, NULL, &mod));
    free(words);
    return mod;
}

static VkDescriptorSet switch_bind_resources(VkDevice d, VkDescriptorSetLayout set, VkBuffer b, VkImageView storage,
                                             const VkDescriptorSetLayoutCreateInfo *layout)
{
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = sizes};
    VkDescriptorPool p;
    SWITCH_VK_CHECK(vkCreateDescriptorPool(d, &pool, NULL, &p));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = p,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &set};
    VkDescriptorSet bound;
    SWITCH_VK_CHECK(vkAllocateDescriptorSets(d, &allocate, &bound));
    VkDescriptorBufferInfo buf = {b, 0, 4096};
    VkDescriptorImageInfo im = {.imageView = storage, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    for (uint32_t i = 0; i < layout->bindingCount; i++) {
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = bound,
                                  .dstBinding = layout->pBindings[i].binding,
                                  .descriptorCount = 1,
                                  .descriptorType = layout->pBindings[i].descriptorType,
                                  .pBufferInfo = &buf,
                                  .pImageInfo = &im};
        vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
    }
    return bound;
}
