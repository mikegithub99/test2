/**
 * @file heartbeat.cc
 * @brief Worker-process integration example for RedisJobStatus heartbeats.
 *
 * Each worker process registers its configured (serverType, serverId) and
 * keeps one RedisJobStatus instance alive for the lifetime of the process.
 * RedisJobStatus owns the periodic heartbeat publisher; do not create a
 * second heartbeat thread here.
 *
 * Application work runs on the application's own threads. This file has no
 * work-processing loop.
 */

#include "RedisJobStatus.h"

#include <csignal>
#include <map>
#include <string>
#include <unistd.h>

namespace
{
volatile std::sig_atomic_t g_running = 1;

void requestShutdown(int)
{
  g_running = 0;
}
} // namespace

// Initialize configuration and RedisClient settings before constructing
// RedisJobStatus.
void initializeConfiguration();

int main()
{
  std::signal(SIGINT, requestShutdown);
  std::signal(SIGTERM, requestShutdown);

  initializeConfiguration();

  // Keep this object alive for the worker process lifetime. Its internal
  // heartbeat thread starts when registration succeeds.
  RedisJobStatus status;
  status.registerServer(
      std::map<std::string, std::string>{}, // server-type metadata
      std::map<std::string, std::string>{}  // server-instance metadata
  );

  // Work is handled by the application's other threads. pause() blocks
  // without polling until a shutdown signal arrives.
  while (g_running)
  {
    pause();
  }

  // RedisJobStatus stops and joins its heartbeat thread during destruction.
  return 0;
}
