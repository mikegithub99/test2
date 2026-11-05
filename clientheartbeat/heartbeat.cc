/**
 * @file heartbeat.cc
 * @brief Worker-process integration example for RedisJobStatus heartbeats.
 *
 * The worker registers once, then this client owns the heartbeat schedule.
 * RedisJobStatus performs individual Redis operations and does not create
 * threads or manage a heartbeat lifecycle.
 *
 * Application work runs on the application's own threads. This file has no
 * work-processing loop.
 */

#include "RedisJobStatus.h"

#include <csignal>
#include <iostream>
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

// Initialize application configuration and RedisClient settings before
// constructing RedisJobStatus. The host application supplies this function.
void initializeConfiguration();

int main()
{
  std::signal(SIGINT, requestShutdown);
  std::signal(SIGTERM, requestShutdown);

  initializeConfiguration();

  RedisJobStatus status;
  status.registerServer(
      std::map<std::string, std::string>{}, // server-type metadata
      std::map<std::string, std::string>{}  // server-instance metadata
  );

  // Publish immediately, then refresh every 10 seconds with a 30-second TTL.
  // The short sleep intervals make shutdown responsive to SIGINT/SIGTERM.
  while (g_running)
  {
    try
    {
      status.refreshHeartbeat(30);
    }
    catch (const std::exception& error)
    {
      std::cerr << "heartbeat: refresh failed: " << error.what() << '\n';
    }
    catch (...)
    {
      std::cerr << "heartbeat: refresh failed: unknown exception\n";
    }

    for (int second = 0; second < 10 && g_running; ++second)
    {
      sleep(1);
    }
  }

  return 0;
}
