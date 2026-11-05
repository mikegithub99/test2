/**
 * @file RedisHeartbeatManager.h
 * @brief One process-wide heartbeat publisher and expired-server cleanup worker.
 *
 * One instance runs per application process. Its identity is the configured,
 * process-unique serverId. The manager refreshes its own heartbeat and scans
 * registered server IDs for expired heartbeats.
 */
#ifndef REDIS_HEARTBEAT_MANAGER_H
#define REDIS_HEARTBEAT_MANAGER_H

#include <atomic>
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
  static constexpr std::int64_t kHeartbeatIntervalSeconds = 10;
  static constexpr std::int64_t kHeartbeatTtlSeconds = 30;
  static constexpr std::int64_t kCleanupScanIntervalSeconds = 5;

  // Starts exactly one manager per process. Repeated calls for the same
  // identity are harmless; a different identity in the same process is an error.
  static void Start(const std::string& serverType, const std::string& serverId);

  RedisHeartbeatManager(const RedisHeartbeatManager&) = delete;
  RedisHeartbeatManager& operator=(const RedisHeartbeatManager&) = delete;

  ~RedisHeartbeatManager();

  // Exposed for deterministic integration tests; the worker also calls it.
  void scanOnce();

  void stop();

private:
  RedisHeartbeatManager(std::string serverType, std::string serverId);
  void heartbeatLoop();
  void cleanupLoop();
  void refreshOwnHeartbeat();

  std::shared_ptr<RedisClient> m_client;
  std::string m_serverType;
  std::string m_serverId;
  std::chrono::seconds m_scanInterval{kCleanupScanIntervalSeconds};
  std::mutex m_mutex;
  std::condition_variable m_wakeup;
  bool m_stopping = false;
  std::thread m_heartbeatThread;
  std::thread m_cleanupThread;
};

#endif // REDIS_HEARTBEAT_MANAGER_H
