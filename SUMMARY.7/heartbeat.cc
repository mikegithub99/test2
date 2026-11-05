/**
 * @file heartbeat.cc
 * @brief Host-application integration example for a worker/server process.
 *
 * registerServer() starts this process's own heartbeat publisher. The central
 * RedisHeartbeatManager cleanup service runs separately.
 */

#include "RedisJobStatus.h"

// Supplied by the host application. Configuration and RedisClient settings
// must be initialized before constructing RedisJobStatus.
void initializeConfiguration();
void runWorker();

int main()
{
  initializeConfiguration();

  RedisJobStatus status;
  status.registerServer();

  // This process now publishes its own heartbeat every 10 seconds with a
  // 30-second TTL. The separate central service performs stale-server cleanup.
  runWorker();

  // Destroying status stops this process's heartbeat publisher. The central
  // cleanup service observes the heartbeat expiry and removes stale artifacts.
  return 0;
}
