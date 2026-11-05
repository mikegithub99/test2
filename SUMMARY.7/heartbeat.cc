/**
 * @file heartbeat.cc
 * @brief Host-application integration example for RedisHeartbeatManager.
 *
 * No separate heartbeat thread or HeartbeatRunner is required here.
 * RedisHeartbeatManager starts once per process after registerServer() and
 * refreshes the configured server ID every 10 seconds with a 30-second TTL.
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

  // The process-wide RedisHeartbeatManager is now running. It refreshes this
  // process's heartbeat and independently scans for expired registered IDs.
  runWorker();

  // On orderly shutdown, process exit stops the manager thread. If the worker
  // exits while the process remains alive, the heartbeat continues; callers
  // should stop the application process when this server instance is retired.
  return 0;
}
