// bb_storage on an extra host: the worker's CAS and action cache on loopback, passed through to the first host's
// storage over the private network. It runs as root, which Local Network privacy lets through.
local host = import 'host.libsonnet';
local allow = { allow: {} };
local upstream = { grpc: { client: { address: host.upstreamStorageAddress } } };

{
  grpcServers: [{ listenAddresses: [host.storageAddress], authenticationPolicy: allow }],
  maximumMessageSizeBytes: 16 * 1024 * 1024,
  global: {
    diagnosticsHttpServer: {
      httpServers: [{ listenAddresses: ['127.0.0.1:9981'], authenticationPolicy: allow }],
      enablePrometheus: true,
    },
  },
  contentAddressableStorageServer: {
    contentAddressableStorage: {
      chunkStorage: upstream,
      chunkMappingStorage: { chunkMappingValidating: { backend: upstream } },
      cdcParameterCache: { cacheSize: 1000, cacheDuration: '60s', cacheReplacementPolicy: 'LEAST_RECENTLY_USED' },
    },
    getAuthorizer: allow,
    putAuthorizer: allow,
    findMissingAuthorizer: allow,
    maximumChunkCount: 100000,
  },
  actionCache: { backend: upstream, getAuthorizer: allow, putAuthorizer: allow },
}
