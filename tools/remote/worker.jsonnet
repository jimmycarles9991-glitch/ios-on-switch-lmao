// bb_worker on a Mac: takes darwin actions from the scheduler, runs them through bb_runner in a native build directory,
// and keeps their inputs in a bounded cache. A virtual (NFSv4) build directory would fetch only what an action reads,
// but on macOS 27 the runner's open(2) of an action's stdout there fails with EINTR (with GODEBUG=asyncpreemptoff=1
// too), which it does not retry. Every host value comes from the LaunchAgent's environment (install.sh);
// the two tokens sit beside this file, chmod 600.
local dir = std.extVar('SWITCH_WORKER_DIR');
local cacheToken = std.stripChars(importstr 'cache.token', '\n');
local workerToken = std.stripChars(importstr 'worker.token', '\n');
local frontend = {
  grpc: {
    client: {
      address: std.extVar('SWITCH_WORKER_FRONTEND'),
      addMetadata: [{ header: 'authorization', values: ['Bearer ' + cacheToken] }],
    },
    // On the LAN, zstd only costs the worker and the frontend CPU.
    enableCompression: false,
  },
};

{
  blobstore: {
    contentAddressableStorage: {
      chunkStorage: frontend,
      chunkMappingStorage: { chunkMappingValidating: { backend: frontend } },
      cdcParameterCache: { cacheSize: 1000, cacheDuration: '60s', cacheReplacementPolicy: 'LEAST_RECENTLY_USED' },
    },
    actionCache: frontend,
  },
  maximumMessageSizeBytes: 16 * 1024 * 1024,
  scheduler: {
    address: std.extVar('SWITCH_WORKER_SCHEDULER'),
    addMetadata: [{ header: 'authorization', values: ['Bearer ' + workerToken] }],
  },
  global: {
    diagnosticsHttpServer: {
      httpServers: [{ listenAddresses: ['127.0.0.1:9986'], authenticationPolicy: { allow: {} } }],
      enablePrometheus: true,
    },
  },
  buildDirectories: [{
    native: {
      buildDirectoryPath: dir + '/build',
      // Every input is hard-linked from here into an action's directory: the worker's only lasting disk use.
      cacheDirectoryPath: dir + '/cache',
      maximumCacheFileCount: 400000,
      maximumCacheSizeBytes: std.parseInt(std.extVar('SWITCH_WORKER_CACHE_GIB')) * 1024 * 1024 * 1024,
      cacheReplacementPolicy: 'LEAST_RECENTLY_USED',
    },
    runners: [{
      endpoint: { address: 'unix://' + dir + '/runner.sock' },
      concurrency: std.parseInt(std.extVar('SWITCH_WORKER_CONCURRENCY')),
      platform: {
        // Bazel's exec platform //bazel/platforms:exec_darwin_arm64, property for property.
        properties: [
          { name: 'ISA', value: 'arm64' },
          { name: 'OSFamily', value: 'darwin' },
          { name: 'xcode', value: std.extVar('SWITCH_WORKER_XCODE_BUILD') },
        ],
      },
      maximumFilePoolFileCount: 100000,
      maximumFilePoolSizeBytes: 2 * 1024 * 1024 * 1024,
      workerId: { host: std.extVar('SWITCH_WORKER_HOST') },
    }],
  }],
  filePool: { blockDevice: { file: { path: dir + '/filepool', sizeBytes: 2 * 1024 * 1024 * 1024 } } },
  // An action waits on its inputs' round trips far more than it runs (10 s fetching to 1 s running, cold): many at once.
  inputDownloadConcurrency: 128,
  outputUploadConcurrency: 64,
  // An input root is fetched directory by directory, one round trip each: XNU's tree and the toolchains alone have
  // more than the 10,000 directories this held.
  directoryCache: {
    maximumCount: 1000000,
    maximumSizeBytes: 1024 * 1024 * 1024,
    cacheReplacementPolicy: 'LEAST_RECENTLY_USED',
  },
}
