/**
 * @file HeartbeatRunner.h
 * @brief Declares the background worker heartbeat publisher.
 */

#ifndef HEARTBEAT_RUNNER_H
#define HEARTBEAT_RUNNER_H

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

class RedisJobStatus;

/**
 * @class HeartbeatRunner
 * @brief Owns the background thread that periodically refreshes one worker heartbeat.
 *
 * The runner retains shared ownership of RedisJobStatus and refreshes its
 * heartbeat immediately, then at the configured interval.
 */
class HeartbeatRunner final
{
public:
  /**
   * @brief Creates the runner and starts its background thread.
   * @param status Worker status whose heartbeat is published.
   * @param heartbeatTtlSeconds TTL applied to each heartbeat refresh.
   * @param heartbeatRefreshIntervalSeconds Delay between refresh attempts.
   */
  HeartbeatRunner(
      std::shared_ptr<RedisJobStatus> status,
      std::int64_t heartbeatTtlSeconds,
      std::int64_t heartbeatRefreshIntervalSeconds);

  /** @brief Requests shutdown and joins the heartbeat thread. */
  ~HeartbeatRunner();

  /** @brief Prevents copying the heartbeat runner. */
  HeartbeatRunner(const HeartbeatRunner&) = delete;

  /** @brief Prevents copy assignment of the heartbeat runner. */
  HeartbeatRunner& operator=(const HeartbeatRunner&) = delete;

private:
  /** @brief Refreshes the heartbeat immediately and on each configured interval. */
  void run();

  std::shared_ptr<RedisJobStatus> m_status; ///< Retained worker status object.
  std::int64_t m_heartbeatTtlSeconds; ///< TTL supplied to heartbeat refreshes.
  std::int64_t m_heartbeatRefreshIntervalSeconds; ///< Delay between heartbeat refreshes.
  std::mutex m_mutex; ///< Protects the runner shutdown flag.
  std::condition_variable m_wakeup; ///< Wakes the runner during shutdown.
  bool m_stopping = false; ///< True after shutdown is requested.
  std::thread m_thread; ///< Background heartbeat publication thread.
};

#endif // HEARTBEAT_RUNNER_H
