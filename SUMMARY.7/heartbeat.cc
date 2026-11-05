/**
 * @file heartbeat.cc
 * @brief C++17 example that refreshes a RedisJobStatus heartbeat on a worker thread.
 *
 * The RedisJobStatus object must outlive HeartbeatRunner. Link this example with
 * RedisJobStatus.cc, RedisClient.cc, redis-plus-plus, hiredis, and the host
 * application's get_reg_val() implementation.
 */

#include "RedisJobStatus.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

class HeartbeatRunner final
{
public:
  HeartbeatRunner(
      RedisJobStatus& status,
      std::chrono::seconds period,
      std::int64_t ttlSeconds)
      : m_status(status),
        m_period(period),
        m_ttlSeconds(ttlSeconds),
        m_thread(&HeartbeatRunner::run, this)
  {
    if (m_period.count() <= 0 || m_ttlSeconds <= 0)
    {
      stop();
      throw std::invalid_argument(
          "Heartbeat period and TTL must both be positive");
    }
  }

  HeartbeatRunner(const HeartbeatRunner&) = delete;
  HeartbeatRunner& operator=(const HeartbeatRunner&) = delete;

  ~HeartbeatRunner()
  {
    stop();
  }

  // Safe to call more than once. Wakes the worker and waits for it to exit.
  void stop()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_wakeup.notify_all();

    if (m_thread.joinable())
    {
      m_thread.join();
    }
  }

private:
  void run()
  {
    std::unique_lock<std::mutex> lock(m_mutex);

    while (!m_stopping)
    {
      lock.unlock();
      try
      {
        // Refresh immediately, then again after each configured period.
        m_status.refreshHeartbeat(m_ttlSeconds);
      }
      catch (const std::exception& error)
      {
        // Keep the worker thread alive; a later refresh may recover.
        std::cerr << "Heartbeat refresh failed: " << error.what() << '\n';
      }
      catch (...)
      {
        std::cerr << "Heartbeat refresh failed: unknown exception\n";
      }
      lock.lock();

      m_wakeup.wait_for(
          lock,
          m_period,
          [this] { return m_stopping; });
    }
  }

  RedisJobStatus& m_status;
  std::chrono::seconds m_period;
  std::int64_t m_ttlSeconds;

  std::mutex m_mutex;
  std::condition_variable m_wakeup;
  bool m_stopping = false;
  std::thread m_thread;
};

/*
 * Example integration in the host worker:
 *
 * int main()
 * {
 *   // Initialize the host configuration registry first. It must provide
 *   // redisJobStatusEnabled, redisJobStatusServerType, and
 *   // redisJobStatusServerId (plus RedisClient's Redis settings).
 *   RedisJobStatus status;
 *   status.registerServer();
 *
 *   // Refresh every 10 seconds; Redis expires the key after 30 seconds
 *   // without a successful refresh.
 *   HeartbeatRunner heartbeat(status, std::chrono::seconds(10), 30);
 *
 *   // Run the worker's normal processing here. When this scope exits,
 *   // HeartbeatRunner stops and joins its thread before status is destroyed.
 *   runWorker();
 * }
 *
 * Ensure runWorker() returns during orderly shutdown. If the worker is
 * disabled by configuration, refreshHeartbeat() is a no-op.
 */
