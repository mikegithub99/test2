/**
 * @file WorkerHeartbeatPublisher.h
 * @brief Worker-only heartbeat publisher; it has no cleanup responsibilities.
 */
#ifndef DUAL_ROLE_WORKER_HEARTBEAT_PUBLISHER_H
#define DUAL_ROLE_WORKER_HEARTBEAT_PUBLISHER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

class HeartbeatRunner;

class WorkerHeartbeatPublisher final
{
public:
  struct Config
  {
    std::int64_t ttlSeconds = 30;
    std::int64_t refreshIntervalSeconds = 10;
  };

  using ConfigProvider = std::function<Config()>;

  /** Process-wide singleton; provider is evaluated only on first access. */
  static std::shared_ptr<WorkerHeartbeatPublisher> Instance(
      ConfigProvider provider = {});
  ~WorkerHeartbeatPublisher();

  WorkerHeartbeatPublisher(const WorkerHeartbeatPublisher&) = delete;
  WorkerHeartbeatPublisher& operator=(const WorkerHeartbeatPublisher&) = delete;

  /** Starts heartbeat publication using the configured timing values. */
  void start();
  void stop();

private:
  explicit WorkerHeartbeatPublisher(ConfigProvider provider);

  Config m_config;
  std::mutex m_mutex;
  std::unique_ptr<HeartbeatRunner> m_runner;
};

#endif
