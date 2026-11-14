// bb_storage on the build host: the CAS and the action cache on this Mac's own disk, and the frontend Bazel talks to
// (through an SSH forward to 127.0.0.1). Execute goes to bb_scheduler on the same host. Every size below is fixed at
// first start: changing one discards that store.
local host = import 'host.libsonnet';
local dir = host.dataDir + '/storage';
local allow = { allow: {} };

local localStore(name, blocksGiB, mapMiB, newBlocks) = {
  local root = dir + '/' + name,
  keyLocationMap: {
    onBlockDevice: { file: { path: root + '/key_location_map', sizeBytes: mapMiB * 1024 * 1024 } },
    maximumGetAttempts: 16,
    maximumPutAttempts: 64,
  },
  oldBlocks: 8,
  currentBlocks: 24,
  newBlocks: newBlocks,
  blocksOnBlockDevice: {
    source: { file: { path: root + '/blocks', sizeBytes: blocksGiB * 1024 * 1024 * 1024 } },
    spareBlocks: 3,
  },
  persistent: { stateDirectoryPath: root + '/persistent_state', minimumEpochInterval: '300s' },
};

{
  grpcServers: [{
    listenAddresses: [a + ':8980' for a in host.listenAddresses],
    authenticationPolicy: allow,
  }],
  maximumMessageSizeBytes: 16 * 1024 * 1024,
  global: {
    diagnosticsHttpServer: {
      httpServers: [{ listenAddresses: ['127.0.0.1:9980'], authenticationPolicy: allow }],
      enablePrometheus: true,
    },
  },
  supportedCompressors: ['ZSTD'],
  contentAddressableStorageServer: {
    contentAddressableStorage: {
      chunkStorage: {
        'local': localStore('cs', host.casGiB, host.casGiB * 10, 3) + {
          chunkingParameters: { minChunkSizeBytes: 256 * 1024, horizonSizeBytes: 2 * 1024 * 1024 },
        },
      },
      chunkMappingStorage: { 'local': localStore('cms', 4, 128, 1) },
    },
    getAuthorizer: allow,
    putAuthorizer: allow,
    findMissingAuthorizer: allow,
    maximumChunkCount: 100000,
  },
  actionCache: {
    // An ActionResult whose outputs were evicted is a miss, not a hit Bazel later fails to download.
    backend: {
      completenessChecking: {
        backend: { 'local': localStore('ac', 16, 1024, 1) },
        maximumTotalTreeSizeBytes: 64 * 1024 * 1024,
      },
    },
    getAuthorizer: allow,
    putAuthorizer: allow,
  },
  schedulers: {
    '': {
      endpoint: {
        address: '127.0.0.1:8982',
        addMetadataJmespathExpression: {
          expression: '{"build.bazel.remote.execution.v2.requestmetadata-bin": incomingGRPCMetadata."build.bazel.remote.execution.v2.requestmetadata-bin"}',
        },
      },
    },
  },
  executeAuthorizer: allow,
}
