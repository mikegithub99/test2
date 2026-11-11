/**
 * @file WorkerHeartbeatPublisher.cc
 * @brief Implements the worker-only heartbeat publisher.
 */
#include "WorkerHeartbeatPublisher.h"

#include "HeartbeatRunner.h"
#include "RedisJobStatus.h"

#include <stdexcept>
#include <string>
#include <utility>

using std::invalid_argument;
using std::shared_ptr;
using std::string;

// The host application provides this registry lookup.
string get_reg_val(const string& key);

namespace
{
std::int64_t readPositiveSetting(const string& key, std::int64_t fallback)
{
  const auto value = get_reg_val(key);
  if (value.empty()) return fallback;
  std::size_t parsed = 0;
  const auto result = std::stoll(value, &parsed);
  if (parsed != value.size() || result <= 0)
    throw invalid_argument("WorkerHeartbeatPublisher: invalid " + key);
  return result;
}

WorkerHeartbeatPublisher::Config defaultConfigProvider()
{
  WorkerHeartbeatPublisher::Config config;
  config.ttlSeconds = readPositiveSetting("heartbeatTtlSeconds", config.ttlSeconds);
  config.refreshIntervalSeconds = readPositiveSetting(
      "heartbeatRefreshIntervalSeconds", config.refreshIntervalSeconds);
  return config;
}
} // namespace

WorkerHeartbeatPublisher::WorkerHeartbeatPublisher(ConfigProvider provider)
    : m_config(provider ? provider() : defaultConfigProvider())
{
  if (m_config.ttlSeconds <= 0 || m_config.refreshIntervalSeconds <= 0)
    throw invalid_argument("WorkerHeartbeatPublisher durations must be positive");
}

shared_ptr<WorkerHeartbeatPublisher> WorkerHeartbeatPublisher::Instance(
    ConfigProvider provider)
{
  static const shared_ptr<WorkerHeartbeatPublisher> instance(
      new WorkerHeartbeatPublisher(std::move(provider)));
  return instance;
}

WorkerHeartbeatPublisher::~WorkerHeartbeatPublisher()
{
  stop();
}

void WorkerHeartbeatPublisher::start()
{
  const auto status = RedisJobStatus::Instance();
  if (!status->isEnabled()) return;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_runner)
  {
    m_runner.reset(new HeartbeatRunner(
        status, m_config.ttlSeconds, m_config.refreshIntervalSeconds));
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
