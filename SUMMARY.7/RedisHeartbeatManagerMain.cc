/**
 * @file RedisHeartbeatManagerMain.cc
 * @brief Entry point for the single deployment-wide heartbeat cleanup service.
 *
 * Build this as a separate executable from the worker/server application.
 * The host must initialize its configuration registry before Start(), so
 * RedisClient::Instance() can obtain its Redis connection settings.
 */

#include "RedisHeartbeatManager.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

// Supplied by the host application; initializes get_reg_val()/Redis settings.
void initializeConfiguration();

namespace
{
std::atomic<bool> running{true};

void requestStop(int)
{
  running.store(false);
}
} // namespace

int main()
{
  std::signal(SIGINT, requestStop);
  std::signal(SIGTERM, requestStop);

  initializeConfiguration();
  RedisHeartbeatManager::Start();

  // The manager's cleanup loop scans all registered server types and IDs.
  // Keep this dedicated service process alive until it receives a stop signal.
  while (running.load())
    std::this_thread::sleep_for(std::chrono::seconds(1));

  return 0;
}
