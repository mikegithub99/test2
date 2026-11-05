/**
 * @file RedisHeartbeatManager.h
 * @brief Central cleanup coordinator for all registered server processes.
 *
 * Run one dedicated instance in the deployment. Server processes publish
 * their own heartbeats independently through RedisJobStatus.
 */
#ifndef REDIS_HEARTBEAT_MANAGER_H
#define REDIS_HEARTBEAT_MANAGER_H

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "RedisClient.h"

class RedisHeartbeatManager final
{
public:
  static constexpr std::int64_t kCleanupScanIntervalSeconds = 5;
  // Explicitly start the single deployment-wide cleanup coordinator.
  static void Start();

  RedisHeartbeatManager(const RedisHeartbeatManager&) = delete;
  RedisHeartbeatManager& operator=(const RedisHeartbeatManager&) = delete;

  ~RedisHeartbeatManager();

  // Exposed for deterministic integration tests; the worker also calls it.
  void scanOnce();

  void stop();

private:
  RedisHeartbeatManager();
  void cleanupLoop();

  std::shared_ptr<RedisClient> m_client;
  std::chrono::seconds m_scanInterval{kCleanupScanIntervalSeconds};
  std::mutex m_mutex;
  std::condition_variable m_wakeup;
  bool m_stopping = false;
  std::thread m_cleanupThread;
};

#endif // REDIS_HEARTBEAT_MANAGER_H
