// bb_worker on a build host: takes darwin actions from the scheduler (this host's, or the first host's over the
// private network), runs them through bb_runner in a native build directory and keeps their inputs in a bounded cache
// of APFS clones. Host values come from host.libsonnet and the LaunchDaemon's environment (switch-worker).
local host = import 'host.libsonnet';
local dir = std.extVar('SWITCH_BB_WORKER_DIR');
local storage = { grpc: { client: { address: host.storageAddress } } };

{
  blobstore: {
    contentAddressableStorage: {
      chunkStorage: storage,
      chunkMappingStorage: { chunkMappingValidating: { backend: storage } },
      cdcParameterCache: { cacheSize: 1000, cacheDuration: '60s', cacheReplacementPolicy: 'LEAST_RECENTLY_USED' },
    },
    actionCache: storage,
  },
  maximumMessageSizeBytes: 16 * 1024 * 1024,
  scheduler: { address: host.schedulerAddress },
  global: {
    diagnosticsHttpServer: {
      httpServers: [{ listenAddresses: ['127.0.0.1:9986'], authenticationPolicy: { allow: {} } }],
      enablePrometheus: true,
    },
  },
  buildDirectories: [{
    native: {
      buildDirectoryPath: dir + '/build',
      cacheDirectoryPath: dir + '/cache',
      maximumCacheFileCount: 1000000,
      maximumCacheSizeBytes: host.workerCacheGiB * 1024 * 1024 * 1024,
      cacheReplacementPolicy: 'LEAST_RECENTLY_USED',
    },
    runners: [{
      endpoint: { address: 'unix://' + dir + '/runner.sock' },
      concurrency: host.concurrency,
      platform: {
        // Bazel's exec platform //bazel/platforms:exec_darwin_arm64, property for property.
        properties: [
          { name: 'ISA', value: 'arm64' },
          { name: 'OSFamily', value: 'darwin' },
          { name: 'xcode', value: std.extVar('SWITCH_WORKER_XCODE_BUILD') },
        ],
      },
      maximumFilePoolFileCount: 200000,
      maximumFilePoolSizeBytes: 8 * 1024 * 1024 * 1024,
      workerId: { host: host.name },
    }],
  }],
  filePool: { blockDevice: { file: { path: dir + '/filepool', sizeBytes: 8 * 1024 * 1024 * 1024 } } },
  inputDownloadConcurrency: 128,
  outputUploadConcurrency: 64,
  // An input root is fetched directory by directory: XNU's tree and the toolchains alone have more than 10,000.
  directoryCache: {
    maximumCount: 1000000,
    maximumSizeBytes: 1024 * 1024 * 1024,
    cacheReplacementPolicy: 'LEAST_RECENTLY_USED',
  },
}
