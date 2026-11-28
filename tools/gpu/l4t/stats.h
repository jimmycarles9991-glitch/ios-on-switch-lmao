static void report_stats(VkDevice d, VkPipeline pipeline)
{
    PFN_vkGetPipelineExecutablePropertiesKHR properties =
        (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(d, "vkGetPipelineExecutablePropertiesKHR");
    PFN_vkGetPipelineExecutableStatisticsKHR statistics =
        (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(d, "vkGetPipelineExecutableStatisticsKHR");
    VkPipelineInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipeline};
    uint32_t count = 0;
    CHECK(properties(d, &info, &count, NULL));
    VkPipelineExecutablePropertiesKHR *exec = calloc(count, sizeof(*exec));
    for (uint32_t i = 0; i < count; i++)
        exec[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    CHECK(properties(d, &info, &count, exec));
    for (uint32_t i = 0; i < count; i++) {
        printf("stage=%u executable=%s subgroup=%u\n", exec[i].stages, exec[i].name, exec[i].subgroupSize);
        VkPipelineExecutableInfoKHR ei = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pipeline, .executableIndex = i};
        uint32_t n = 0;
        CHECK(statistics(d, &ei, &n, NULL));
        VkPipelineExecutableStatisticKHR *stat = calloc(n, sizeof(*stat));
        for (uint32_t j = 0; j < n; j++)
            stat[j].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        CHECK(statistics(d, &ei, &n, stat));
        for (uint32_t j = 0; j < n; j++) {
            printf("  %s = ", stat[j].name);
            switch (stat[j].format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                printf("%u", stat[j].value.b32);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                printf("%lld", (long long)stat[j].value.i64);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                printf("%llu", (unsigned long long)stat[j].value.u64);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
                printf("%g", stat[j].value.f64);
                break;
            default:
                exit(1);
            }
            printf(" (%s)\n", stat[j].description);
        }
        free(stat);
    }
    free(exec);
}
