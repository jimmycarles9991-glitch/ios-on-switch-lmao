// bb_worker on a build host: takes darwin actions from the scheduler (this host's, or the first host's over the
// private network), runs them through bb_runner in a native build directory and keeps their inputs in a bounded cache
// of APFS clones. Host values come from host.libsonnet and the LaunchDaemon's environment (switch-worker).
local host = import 'host.libsonnet';
local dir = std.extVar('SWITCH_BB_WORKER_DIR');
local gpu = std.extVar('SWITCH_WORKER_GPU');
local storage = { grpc: { client: { address: host.storageAddress } } };
// Bazel's exec platform //bazel/platforms:exec_darwin_arm64, property for property.
local platform = [
  { name: 'ISA', value: 'arm64' },
  { name: 'OSFamily', value: 'darwin' },
  { name: 'xcode', value: std.extVar('SWITCH_WORKER_XCODE_BUILD') },
];
local runner(concurrency, properties) = {
  endpoint: { address: 'unix://' + dir + '/runner.sock' },
  concurrency: concurrency,
  platform: { properties: properties },
  maximumFilePoolFileCount: 200000,
  maximumFilePoolSizeBytes: 8 * 1024 * 1024 * 1024,
  workerId: { host: host.name },
};

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
    // The scheduler queues an action by its exact platform, so the actions that ask for a GPU (a `gpu` property:
    // apps/hwtest's Core Animation goldens) run on a one-slot runner of their own, and every other action keeps all of
    // the general runner's slots. Slots are numbered across a build directory's runners, so the two share none.
    runners: [runner(host.concurrency, platform)] +
             (if gpu == '' then [] else [runner(1, platform + [{ name: 'gpu', value: gpu }])]),
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
