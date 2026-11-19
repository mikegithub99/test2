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
class HeartbeatRunner;

/**
 * @class RedisHeartbeatManager
 * @brief Publishes worker heartbeats and removes workers whose leases expire.
 *
 * Instance() creates one cleanup manager per process. HeartbeatStart() creates
 * one heartbeat publisher per process using RedisJobStatus::Instance().
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
    * All durations are independently configurable. The first-heartbeat
    * timeout is a startup grace period, not a substitute for a heartbeat.
    */
   struct Config
   {
      /**
       * TTL in seconds applied to each heartbeat.
       */
      std::int64_t heartbeatTtlSeconds = 30;
      /**
       * Interval in seconds between heartbeat refreshes.
       */
      std::int64_t heartbeatRefreshIntervalSeconds = 10;
      /**
       * TTL in milliseconds for the distributed cleanup lease.
       */
      std::int64_t cleanupLeaseTtlMilliseconds = 30000;
      /**
       * Interval in seconds between cleanup lease renewals.
       */
      std::int64_t cleanupLeaseRenewIntervalSeconds = 5;
      /**
       * Interval in seconds between cleanup scans.
       */
      std::int64_t cleanupScanIntervalSeconds = 5;
      /**
       * Maximum time in seconds allowed between registration and the first heartbeat.
       */
      std::int64_t firstHeartbeatTimeoutSeconds = 60;
      /**
       * Retention period in seconds for retired-server tombstones.
       */
      std::int64_t retiredTombstoneRetentionSeconds = 24 * 60 * 60;
   };

   /**
    * Callable that supplies heartbeat and cleanup timing configuration.
    */
   using ConfigProvider = std::function<Config()>;

   /**
    * @brief Gets the process-wide manager, initializing it lazily.
    * @param provider Optional timing configuration provider. A supplied
    *        provider must be passed on the first call only.
    */
   static std::shared_ptr<RedisHeartbeatManager> Instance(ConfigProvider provider = {});

   /**
    * @brief Starts the heartbeat publisher using the RedisJobStatus singleton.
    *
    * The first call starts the publisher; subsequent calls are ignored.
    * RedisJobStatus::Instance() must be configured before this call when
    * custom status configuration is required.
    */
   void HeartbeatStart();

   /**
    * @brief Prevents copying the process manager.
    */
   RedisHeartbeatManager(const RedisHeartbeatManager&) = delete;

   /**
    * @brief Prevents copy assignment of the process manager.
    */
   RedisHeartbeatManager& operator=(const RedisHeartbeatManager&) = delete;

   /**
    * @brief Stops and joins the cleanup thread.
    */
   ~RedisHeartbeatManager();

   /**
    * @brief Performs one distributed stale-worker cleanup scan.
    *
    * The scan acquires the Redis cleanup lease, checks registered workers,
    * fences expired workers, and removes their associated job data.
    */
   void scanOnce();

   /**
    * @brief Stops and joins cleanup; the heartbeat runner stops when the manager is destroyed.
    */
   void stop();

 private:
   /**
    * @brief Constructs the singleton, loads its configuration, and starts cleanup.
    */
   explicit RedisHeartbeatManager(ConfigProvider provider);

   /**
    * @brief Runs cleanup scans until shutdown is requested.
    */
   void cleanupLoop();

   /**
    * False when Redis status reporting is disabled.
    */
   bool m_enabled = false;
   /**
    * Injected heartbeat and cleanup timing configuration.
    */
   Config m_config;
   /**
    * Redis client used for cleanup.
    */
   std::shared_ptr<RedisClient> m_client;
   /**
    * Protects cleanup-thread shutdown state.
    */
   std::mutex m_mutex;
   /**
    * Wakes the cleanup loop during shutdown.
    */
   std::condition_variable m_wakeup;
   /**
    * True after cleanup shutdown is requested.
    */
   bool m_stopping = false;
   /**
    * Background distributed cleanup thread.
    */
   std::thread m_cleanupThread;
   /**
    * Protects heartbeat-runner initialization.
    */
   std::mutex m_heartbeatMutex;
   /**
    * Worker heartbeat publisher.
    */
   std::unique_ptr<HeartbeatRunner> m_heartbeatRunner;
};

#endif // REDIS_HEARTBEAT_MANAGER_H
