/**
 * @file HeartbeatRunner.cc
 * @brief Implements the background worker heartbeat publisher.
 */

#include "HeartbeatRunner.h"

#include "RedisJobStatus.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

/**
 * @brief Creates the runner and starts its background thread.
 * @param status Worker status whose heartbeat is published.
 * @param heartbeatTtlSeconds TTL applied to each heartbeat refresh.
 * @param heartbeatRefreshIntervalSeconds Delay between refresh attempts.
 */
HeartbeatRunner::HeartbeatRunner(
    std::shared_ptr<RedisJobStatus> status,
    std::int64_t heartbeatTtlSeconds,
    std::int64_t heartbeatRefreshIntervalSeconds)
    : m_status(std::move(status)),
      m_heartbeatTtlSeconds(heartbeatTtlSeconds),
      m_heartbeatRefreshIntervalSeconds(heartbeatRefreshIntervalSeconds)
{
  if (!m_status)
  {
    throw std::invalid_argument("HeartbeatRunner requires a non-null RedisJobStatus");
  }

  if (m_heartbeatTtlSeconds <= 0 || m_heartbeatRefreshIntervalSeconds <= 0)
  {
    throw std::invalid_argument("HeartbeatRunner durations must be positive");
  }

  m_thread = std::thread(&HeartbeatRunner::run, this);
}

/** @brief Requests shutdown and joins the heartbeat thread. */
HeartbeatRunner::~HeartbeatRunner()
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

/** @brief Refreshes the heartbeat immediately and on each configured interval. */
void HeartbeatRunner::run()
{
  std::unique_lock<std::mutex> lock(m_mutex);

  while (!m_stopping)
  {
    lock.unlock();

    try
    {
      m_status->refreshHeartbeat(m_heartbeatTtlSeconds);
    }
    catch (const std::exception& e)
    {
      std::cerr << "HeartbeatStart: heartbeat refresh failed: "
                << e.what() << '\n';
    }
    catch (...)
    {
      std::cerr
          << "HeartbeatStart: heartbeat refresh failed: unknown exception\n";
    }

    lock.lock();
    m_wakeup.wait_for(
        lock,
        std::chrono::seconds(m_heartbeatRefreshIntervalSeconds),
        [this]
        {
          return m_stopping;
        });
  }
}
