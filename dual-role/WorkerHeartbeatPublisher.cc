/**
 * @file WorkerHeartbeatPublisher.cc
 * @brief Implements the worker-only heartbeat publisher.
 */
#include "WorkerHeartbeatPublisher.h"

#include "HeartbeatRunner.h"
#include "RedisJobStatus.h"

#include <stdexcept>

WorkerHeartbeatPublisher::~WorkerHeartbeatPublisher()
{
  stop();
}

void WorkerHeartbeatPublisher::start(
    std::int64_t ttlSeconds,
    std::int64_t refreshIntervalSeconds)
{
  const auto status = RedisJobStatus::Instance();
  if (!status->isEnabled())
  {
    return;
  }
  if (ttlSeconds <= 0 || refreshIntervalSeconds <= 0)
  {
    throw std::invalid_argument("heartbeat durations must be positive");
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_runner)
  {
    m_runner.reset(new HeartbeatRunner(
        status, ttlSeconds, refreshIntervalSeconds));
  }
}

void WorkerHeartbeatPublisher::stop()
{
  std::unique_ptr<HeartbeatRunner> runner;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    runner = std::move(m_runner);
  }
  runner.reset();
}
