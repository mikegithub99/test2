#ifndef REDIS_HEARTBEAT_MANAGER_H
#define REDIS_HEARTBEAT_MANAGER_H
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include "RedisClient.h"

// One cleanup thread per worker process. Redis lease elects the normal scan
// owner; per-server Lua fencing remains authoritative if scans overlap.
class RedisHeartbeatManager final
{
public:
  static constexpr std::int64_t kCleanupScanIntervalSeconds = 5;
  static void Start();
  RedisHeartbeatManager(const RedisHeartbeatManager&) = delete;
  RedisHeartbeatManager& operator=(const RedisHeartbeatManager&) = delete;
  ~RedisHeartbeatManager();
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
