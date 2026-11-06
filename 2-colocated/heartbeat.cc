/**
 * @file heartbeat.cc
 * @brief Worker startup example using the colocated cleanup and heartbeat services.
 *
 * The worker owns one long-lived RedisJobStatus. HeartbeatStart() retains a
 * shared reference and publishes immediately, then every 10 seconds with a
 * 30-second Redis TTL. RedisHeartbeatManager::Start() independently starts
 * this process's distributed cleanup scanner.
 */

#include "RedisJobStatus.h"
#include "RedisHeartbeatManager.h"

#include <map>
#include <memory>

// The host application supplies configuration initialization.
void initializeConfiguration();

int runWorkerApplication();

int main()
{
  initializeConfiguration();

  auto status = std::make_shared<RedisJobStatus>();
  status->registerServer(
      std::map<std::string, std::string>{}, // server-type metadata
      std::map<std::string, std::string>{}  // server-instance metadata
  );

  // Both services are now encapsulated behind process-level start methods.
  RedisHeartbeatManager::Start();
  RedisHeartbeatManager::HeartbeatStart(status);

  // Enter the host worker's normal event/work loop here. Keep this process
  // alive while it is serving work; the heartbeat and cleanup threads run
  // independently. Returning from main triggers orderly thread shutdown.
  return runWorkerApplication();
}
