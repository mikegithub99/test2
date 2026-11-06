/**
 * @file RedisHeartbeatManager.h
 * @brief Declares worker heartbeat publication and distributed stale-worker cleanup.
 */

#ifndef REDIS_HEARTBEAT_MANAGER_H
#define REDIS_HEARTBEAT_MANAGER_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "RedisClient.h"

class RedisJobStatus;

/**
 * @class RedisHeartbeatManager
 * @brief Publishes worker heartbeats and removes workers whose leases expire.
 *
 * Start() creates one cleanup manager per process. HeartbeatStart() creates
 * one heartbeat publisher per process and retains the supplied status object
 * for the publisher thread's lifetime.
 *
 * Cleanup ownership is coordinated through a Redis lease. Per-server Lua
 * fencing remains authoritative if cleanup scans overlap.
 */
class RedisHeartbeatManager final
{
public:
  /**
   * @brief Timing configuration for heartbeat publication and stale-worker cleanup.
   *
   * All durations are independently configurable. Defaults preserve the
   * original manager behavior.
   */
  struct Config
  {
    std::int64_t heartbeatTtlSeconds = 30;
    std::int64_t heartbeatRefreshIntervalSeconds = 10;
    std::int64_t cleanupLeaseTtlMilliseconds = 30000;
    std::int64_t cleanupLeaseRenewIntervalSeconds = 5;
    std::int64_t cleanupScanIntervalSeconds = 5;
  };

  using ConfigProvider = std::function<Config()>;

  /**
   * @brief Starts the process-wide cleanup manager.
   *
   * The provider is evaluated only on the first call. An empty provider uses
   * default configuration. Repeated calls do not create additional threads.
   * Call Start(provider) before HeartbeatStart() to inject custom settings.
   *
   * @param provider Optional source of all manager timing settings.
   * @throws std::invalid_argument if a configured duration is not positive.
   */
  static void Start(ConfigProvider provider = {});

  /**
   * @brief Starts the process-wide worker heartbeat publisher.
   *
   * The first call starts the publisher; subsequent calls are ignored.
   * The publisher refreshes immediately and then periodically.
   *
   * @param status Shared worker status whose heartbeat is refreshed.
   * @throws std::invalid_argument if status is null.
   */
  static void HeartbeatStart(std::shared_ptr<RedisJobStatus> status);

  /** @brief Prevents copying the process manager. */
  RedisHeartbeatManager(const RedisHeartbeatManager&) = delete;

  /** @brief Prevents copy assignment of the process manager. */
  RedisHeartbeatManager& operator=(const RedisHeartbeatManager&) = delete;

  /** @brief Stops and joins the cleanup thread. */
  ~RedisHeartbeatManager();

  /**
   * @brief Performs one distributed stale-worker cleanup scan.
   *
   * The scan acquires the Redis cleanup lease, checks registered workers,
   * fences expired workers, and removes their associated job data.
   */
  void scanOnce();

  /** @brief Signals the cleanup loop to stop and joins its thread. */
  void stop();

private:
  /** @brief Constructs the manager and starts its cleanup thread. */
  explicit RedisHeartbeatManager(const Config& config);

  /** @brief Runs cleanup scans until shutdown is requested. */
  void cleanupLoop();

  Config m_config; ///< Injected heartbeat and cleanup timing configuration.
  std::shared_ptr<RedisClient> m_client; ///< Redis client used for cleanup.
  std::mutex m_mutex; ///< Protects cleanup-thread shutdown state.
  std::condition_variable m_wakeup; ///< Wakes the cleanup loop during shutdown.
  bool m_stopping = false; ///< True after cleanup shutdown is requested.
  std::thread m_cleanupThread; ///< Background distributed cleanup thread.
};

#endif // REDIS_HEARTBEAT_MANAGER_H
