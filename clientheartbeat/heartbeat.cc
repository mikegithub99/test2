/**
 * @file heartbeat.cc
 * @brief Worker-process integration example for RedisJobStatus heartbeats.
 *
 * The worker registers once, then this client owns the heartbeat schedule.
 * RedisJobStatus performs individual Redis operations and does not create
 * threads or manage a heartbeat lifecycle.
 *
 * Application work runs on the application's own threads. This file has no
 * work-processing loop or shutdown mechanism.
 */

#include "RedisJobStatus.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

// Initialize application configuration and RedisClient settings before
// constructing RedisJobStatus. The host application supplies this function.
void initializeConfiguration();

int main()
{
  initializeConfiguration();

  RedisJobStatus status;
  status.registerServer(
      std::map<std::string, std::string>{}, // server-type metadata
      std::map<std::string, std::string>{}  // server-instance metadata
  );

  // This client owns the heartbeat schedule. Publish immediately, then
  // refresh every 10 seconds with a 30-second Redis TTL.
  std::mutex waitMutex;
  std::condition_variable heartbeatWakeup;
  std::unique_lock<std::mutex> waitLock(waitMutex);

  for (;;)
  {
    try
    {
      status.refreshHeartbeat(30);
    }
    catch (const std::exception& error)
    {
      std::cerr << "heartbeat: refresh failed: " << error.what() << '\\n';
    }
    catch (...)
    {
      std::cerr << "heartbeat: refresh failed: unknown exception\\n";
    }

    // No stop condition or notification is used: the worker process owns
    // this loop for its lifetime. The predicate overload handles spurious
    // wakeups while preserving the full 10-second interval.
    heartbeatWakeup.wait_for(
        waitLock, std::chrono::seconds(10), [] { return false; });
  }

  return 0;
}
