// bb_runner on a build host: runs each action bb_worker hands it, in its directory below the build directory.
local host = import 'host.libsonnet';
local dir = std.extVar('SWITCH_BB_WORKER_DIR');

{
  buildDirectoryPath: dir + '/build',
  grpcServers: [{ listenPaths: [dir + '/runner.sock'], authenticationPolicy: { allow: {} } }],
  global: {},
  // TMPDIR inside the action's directory, removed with it (xcrun and clang keep caches there).
  setTmpdirEnvironmentVariable: true,
  // Bazel sends XCODE_VERSION_OVERRIDE, not a path, for rules_apple's and apple_support's actions; the runner sets
  // DEVELOPER_DIR and SDKROOT from it, as Bazel's local runner does.
  appleXcodeDeveloperDirectories: {
    [std.extVar('SWITCH_WORKER_XCODE_VERSION')]: '/Applications/Xcode.app/Contents/Developer',
  },
}
