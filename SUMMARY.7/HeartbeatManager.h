/**
 * @file HeartbeatManager.h
 * @brief Background cleanup of server artifacts after heartbeat expiry.
 *
 * The manager is separate from RedisJobStatus. It owns its worker thread and
 * uses the shared RedisClient. Server IDs must be unique per process and
 * must never be reused.
 */
#ifndef HEARTBEAT_MANAGER_H
#define HEARTBEAT_MANAGER_H

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "RedisClient.h"

class HeartbeatManager final
{
public:
  explicit HeartbeatManager(
      std::chrono::seconds scanInterval = std::chrono::seconds(5));

  HeartbeatManager(const HeartbeatManager&) = delete;
  HeartbeatManager& operator=(const HeartbeatManager&) = delete;

  ~HeartbeatManager();

  // Performs one synchronous discovery/cleanup pass. Useful for tests and
  // controlled service integration; the background thread calls the same pass.
  void scanOnce();

  // Stops the worker and joins it. Safe to call more than once.
  void stop();

private:
  void run();

  std::shared_ptr<RedisClient> m_client;
  std::chrono::seconds m_scanInterval;
  std::mutex m_mutex;
  std::condition_variable m_wakeup;
  bool m_stopping = false;
  std::thread m_thread;
};

#endif // HEARTBEAT_MANAGER_H
