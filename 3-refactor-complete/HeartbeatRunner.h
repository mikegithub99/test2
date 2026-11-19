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

   /**
    * @brief Requests shutdown and joins the heartbeat thread.
    */
   ~HeartbeatRunner();

   /**
    * @brief Prevents copying the heartbeat runner.
    */
   HeartbeatRunner(const HeartbeatRunner&) = delete;

   /**
    * @brief Prevents copy assignment of the heartbeat runner.
    */
   HeartbeatRunner& operator=(const HeartbeatRunner&) = delete;

 private:
   /**
    * @brief Refreshes the heartbeat immediately and on each configured interval.
    */
   void run();

   /**
    * Retained worker status object.
    */
   std::shared_ptr<RedisJobStatus> m_status;
   /**
    * TTL supplied to heartbeat refreshes.
    */
   std::int64_t m_heartbeatTtlSeconds;
   /**
    * Delay between heartbeat refreshes.
    */
   std::int64_t m_heartbeatRefreshIntervalSeconds;
   /**
    * Protects the runner shutdown flag.
    */
   std::mutex m_mutex;
   /**
    * Wakes the runner during shutdown.
    */
   std::condition_variable m_wakeup;
   /**
    * True after shutdown is requested.
    */
   bool m_stopping = false;
   /**
    * Background heartbeat publication thread.
    */
   std::thread m_thread;
};

#endif // HEARTBEAT_RUNNER_H
