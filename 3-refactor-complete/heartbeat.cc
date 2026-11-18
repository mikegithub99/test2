/**
 * @file heartbeat.cc
 * @brief Worker startup example using the process-wide status and heartbeat services.
 *
 * The host application supplies configuration initialization and its normal
 * worker event loop. RedisJobStatus::Instance() reads the configured identity;
 * registration must complete before the heartbeat publisher starts.
 */

#include "RedisJobStatus.h"
#include "RedisHeartbeatManager.h"

#include <map>

void initializeConfiguration();
int runWorkerApplication();

int main()
{
  initializeConfiguration();

  // The singleton is the public construction path; use this same instance
  // for registration and all job lifecycle operations in this process.
  const auto status = RedisJobStatus::Instance();
  status->registerServer(
      std::map<std::string, std::string>{}, // server-type metadata
      std::map<std::string, std::string>{}  // server-instance metadata
  );

  // Instance() starts the process-local cleanup thread. HeartbeatStart()
  // starts the publisher using RedisJobStatus::Instance().
  const auto heartbeatManager = RedisHeartbeatManager::Instance();
  heartbeatManager->HeartbeatStart();

  // Keep the process alive while it serves work. Static process-wide
  // services are destroyed during orderly process shutdown.
  return runWorkerApplication();
}
