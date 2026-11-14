// bb_scheduler on the build host: takes Execute from bb_storage and hands actions to the workers, this host's own and
// any other build host's over the private network (host.libsonnet's listenAddresses).
local host = import 'host.libsonnet';
local allow = { allow: {} };
local storage = { grpc: { client: { address: '127.0.0.1:8980' } } };

{
  clientGrpcServers: [{ listenAddresses: ['127.0.0.1:8982'], authenticationPolicy: allow }],
  workerGrpcServers: [{
    listenAddresses: [a + ':8983' for a in host.listenAddresses],
    authenticationPolicy: allow,
  }],
  maximumMessageSizeBytes: 16 * 1024 * 1024,
  global: {
    diagnosticsHttpServer: {
      httpServers: [{ listenAddresses: ['127.0.0.1:9982'], authenticationPolicy: allow }],
      enablePrometheus: true,
    },
  },
  // Reads each Action message for its platform and timeout.
  contentAddressableStorage: {
    chunkStorage: storage,
    chunkMappingStorage: { chunkMappingValidating: { backend: storage } },
    cdcParameterCache: { cacheSize: 1000, cacheDuration: '60s', cacheReplacementPolicy: 'LEAST_RECENTLY_USED' },
  },
  executeAuthorizer: allow,
  modifyDrainsAuthorizer: allow,
  killOperationsAuthorizer: allow,
  synchronizeAuthorizer: allow,
  actionRouter: {
    simple: {
      platformKeyExtractor: { action: {} },
      invocationKeyExtractors: [{ correlatedInvocationsId: {} }, { toolInvocationId: {} }],
      initialSizeClassAnalyzer: { defaultExecutionTimeout: '1800s', maximumExecutionTimeout: '7200s' },
    },
  },
  // With no worker of a platform left, Execute fails soon and Bazel builds those actions locally
  // (--remote_local_fallback) instead of waiting for one.
  platformQueueWithNoWorkersTimeout: '120s',
}
