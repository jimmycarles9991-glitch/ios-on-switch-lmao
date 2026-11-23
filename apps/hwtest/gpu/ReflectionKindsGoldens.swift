/// Metal's answers for gpu.gm20b.reflection_kinds's bindings (ReflectionKinds.answers), measured on an M4 Max under macOS
/// 27.2.1 with the test's own shaders and 24B119's QuartzCore library
public enum ReflectionKindsGoldens {
    public static let metal: [String: [String: Int]] = [
        "reflection_kinds.b4": [
            "access": 1, "arrayLength": 1, "bufferAlignment": 16, "bufferDataSize": 16, "bufferDataType": 6,
            "bufferPointerType": 1, "bufferStructType": 0, "index": 0, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 0, "type": 0,
        ],
        "reflection_kinds.bs": [
            "access": 1, "arrayLength": 1, "bufferAlignment": 16, "bufferDataSize": 32, "bufferDataType": 1,
            "bufferPointerType": 1, "bufferStructType": 1, "index": 1, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 1, "type": 0,
        ],
        "reflection_kinds.cf": [
            "access": 0, "arrayLength": 1, "bufferAlignment": 4, "bufferDataSize": 4, "bufferDataType": 3,
            "bufferPointerType": 1, "bufferStructType": 0, "index": 2, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 0, "type": 0,
        ],
        "reflection_kinds.bu": [
            "access": 1, "arrayLength": 1, "bufferAlignment": 4, "bufferDataSize": 4, "bufferDataType": 33,
            "bufferPointerType": 1, "bufferStructType": 0, "index": 3, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 0, "type": 0,
        ],
        "reflection_kinds.unused": [
            "access": 1, "arrayLength": 1, "bufferAlignment": 2, "bufferDataSize": 2, "bufferDataType": 16,
            "bufferPointerType": 1, "bufferStructType": 0, "index": 4, "isActive": 0, "isArgument": 1, "isUsed": 0,
            "structType": 0, "type": 0,
        ],
        "reflection_kinds.t": [
            "access": 0, "arrayLength": 1, "bufferPointerType": 0, "index": 0, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 0, "isUsed": 1, "structType": 0, "textureDataType": 3, "textureType": 2, "type": 2,
        ],
        "reflection_kinds.ta": [
            "access": 0, "arrayLength": 4, "bufferPointerType": 0, "index": 1, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 0, "isUsed": 1, "structType": 0, "textureDataType": 3, "textureType": 2, "type": 2,
        ],
        "reflection_kinds.tarr": [
            "access": 0, "arrayLength": 1, "bufferPointerType": 0, "index": 5, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 0, "isUsed": 1, "structType": 0, "textureDataType": 3, "textureType": 3, "type": 2,
        ],
        "reflection_kinds.d": [
            "access": 0, "arrayLength": 1, "bufferPointerType": 0, "index": 6, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 1, "isUsed": 1, "structType": 0, "textureDataType": 3, "textureType": 2, "type": 2,
        ],
        "reflection_kinds.tc": [
            "access": 0, "arrayLength": 3, "bufferPointerType": 0, "index": 7, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 1, "isUsed": 1, "structType": 0, "textureDataType": 3, "textureType": 2, "type": 2,
        ],
        "reflection_kinds.w": [
            "access": 2, "arrayLength": 1, "bufferPointerType": 0, "index": 10, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 0, "isUsed": 1, "structType": 0, "textureDataType": 16, "textureType": 2, "type": 2,
        ],
        "reflection_kinds.s": [
            "access": 0, "arrayLength": 1, "bufferPointerType": 0, "index": 0, "isActive": 1, "isArgument": 1,
            "isUsed": 1, "structType": 0, "type": 3,
        ],
        "reflection_kinds.sa": [
            "access": 0, "arrayLength": 3, "bufferPointerType": 0, "index": 1, "isActive": 1, "isArgument": 1,
            "isUsed": 1, "structType": 0, "type": 3,
        ],
        "reflection_kinds.sc": [
            "access": 0, "arrayLength": 2, "bufferPointerType": 0, "index": 4, "isActive": 1, "isArgument": 1,
            "isUsed": 1, "structType": 0, "type": 3,
        ],
        "reflection_kinds.tg": [
            "access": 1, "arrayLength": 1, "bufferPointerType": 0, "index": 0, "isActive": 1, "isArgument": 1,
            "isUsed": 1, "structType": 0, "threadgroupMemoryAlignment": 4, "threadgroupMemoryDataSize": 4, "type": 1,
        ],
        "reflection_kinds.tg4": [
            "access": 1, "arrayLength": 1, "bufferPointerType": 0, "index": 1, "isActive": 1, "isArgument": 1,
            "isUsed": 1, "structType": 0, "threadgroupMemoryAlignment": 16, "threadgroupMemoryDataSize": 16, "type": 1,
        ],
        "reflection_vs.pos": [
            "access": 0, "arrayLength": 1, "bufferAlignment": 16, "bufferDataSize": 16, "bufferDataType": 6,
            "bufferPointerType": 1, "bufferStructType": 0, "index": 0, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 0, "type": 0,
        ],
        "reflection_vs.u": [
            "access": 0, "arrayLength": 1, "bufferAlignment": 16, "bufferDataSize": 32, "bufferDataType": 1,
            "bufferPointerType": 1, "bufferStructType": 1, "index": 1, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 1, "type": 0,
        ],
        "reflection_vs.vt": [
            "access": 0, "arrayLength": 1, "bufferPointerType": 0, "index": 0, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 0, "isUsed": 1, "structType": 0, "textureDataType": 3, "textureType": 2, "type": 2,
        ],
        "reflection_fs.ft": [
            "access": 0, "arrayLength": 1, "bufferPointerType": 0, "index": 0, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 0, "isUsed": 1, "structType": 0, "textureDataType": 3, "textureType": 2, "type": 2,
        ],
        "reflection_fs.fa": [
            "access": 0, "arrayLength": 2, "bufferPointerType": 0, "index": 1, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 0, "isUsed": 1, "structType": 0, "textureDataType": 3, "textureType": 2, "type": 2,
        ],
        "reflection_fs.fsm": [
            "access": 0, "arrayLength": 1, "bufferPointerType": 0, "index": 0, "isActive": 1, "isArgument": 1,
            "isUsed": 1, "structType": 0, "type": 3,
        ],
        "reflection_fs.fsa": [
            "access": 0, "arrayLength": 2, "bufferPointerType": 0, "index": 1, "isActive": 1, "isArgument": 1,
            "isUsed": 1, "structType": 0, "type": 3,
        ],
        "reflection_fs.tint": [
            "access": 0, "arrayLength": 1, "bufferAlignment": 16, "bufferDataSize": 16, "bufferDataType": 6,
            "bufferPointerType": 1, "bufferStructType": 0, "index": 0, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 0, "type": 0,
        ],
        "compute_average_luma.tex": [
            "access": 0, "arrayLength": 1, "bufferPointerType": 0, "index": 0, "isActive": 1, "isArgument": 1,
            "isDepthTexture": 0, "isUsed": 1, "structType": 0, "textureDataType": 16, "textureType": 2, "type": 2,
        ],
        "compute_average_luma.u": [
            "access": 0, "arrayLength": 1, "bufferAlignment": 4, "bufferDataSize": 20, "bufferDataType": 1,
            "bufferPointerType": 1, "bufferStructType": 1, "index": 0, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 1, "type": 0,
        ],
        "compute_average_luma.destination": [
            "access": 1, "arrayLength": 1, "bufferAlignment": 16, "bufferDataSize": 16, "bufferDataType": 6,
            "bufferPointerType": 1, "bufferStructType": 0, "index": 2, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 0, "type": 0,
        ],
        "compute_average_luma.tg_mem": [
            "access": 1, "arrayLength": 1, "bufferPointerType": 0, "index": 0, "isActive": 1, "isArgument": 1,
            "isUsed": 1, "structType": 0, "threadgroupMemoryAlignment": 16, "threadgroupMemoryDataSize": 16, "type": 1,
        ],
        "compute_sum_luma.u": [
            "access": 0, "arrayLength": 1, "bufferAlignment": 4, "bufferDataSize": 20, "bufferDataType": 1,
            "bufferPointerType": 1, "bufferStructType": 1, "index": 0, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 1, "type": 0,
        ],
        "compute_sum_luma.thread_group_sums": [
            "access": 0, "arrayLength": 1, "bufferAlignment": 16, "bufferDataSize": 16, "bufferDataType": 6,
            "bufferPointerType": 1, "bufferStructType": 0, "index": 1, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 0, "type": 0,
        ],
        "compute_sum_luma.destination": [
            "access": 1, "arrayLength": 1, "bufferAlignment": 16, "bufferDataSize": 16, "bufferDataType": 6,
            "bufferPointerType": 1, "bufferStructType": 0, "index": 2, "isActive": 1, "isArgument": 1, "isUsed": 1,
            "structType": 0, "type": 0,
        ],
        "compute_sum_luma.tg_mem": [
            "access": 1, "arrayLength": 1, "bufferPointerType": 0, "index": 0, "isActive": 1, "isArgument": 1,
            "isUsed": 1, "structType": 0, "threadgroupMemoryAlignment": 16, "threadgroupMemoryDataSize": 16, "type": 1,
        ],
    ]
}
