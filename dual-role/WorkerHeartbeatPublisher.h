/**
 * @file WorkerHeartbeatPublisher.h
 * @brief Worker-only heartbeat publisher; it has no cleanup responsibilities.
 */
#ifndef DUAL_ROLE_WORKER_HEARTBEAT_PUBLISHER_H
#define DUAL_ROLE_WORKER_HEARTBEAT_PUBLISHER_H

#include <cstdint>
#include <memory>
#include <mutex>

class RedisJobStatus;
class HeartbeatRunner;

class WorkerHeartbeatPublisher final
{
public:
  WorkerHeartbeatPublisher() = default;
  ~WorkerHeartbeatPublisher();

  WorkerHeartbeatPublisher(const WorkerHeartbeatPublisher&) = delete;
  WorkerHeartbeatPublisher& operator=(const WorkerHeartbeatPublisher&) = delete;

  void start(std::int64_t ttlSeconds, std::int64_t refreshIntervalSeconds);
  void stop();

private:
  std::mutex m_mutex;
  std::unique_ptr<HeartbeatRunner> m_runner;
};

#endif
