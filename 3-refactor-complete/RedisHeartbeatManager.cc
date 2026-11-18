/**
 * @file RedisHeartbeatManager.cc
 * @brief Implements worker heartbeat publication and distributed cleanup.
 */

#include "RedisHeartbeatManager.h"

#include "HeartbeatRunner.h"
#include "RedisJobStatus.h"
#include "RedisJobScripts.inc"

#include <iostream>
#include <initializer_list>
#include <initializer_list>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using std::back_inserter;
using std::int64_t;
using std::string;
using std::vector;

namespace
{
   enum class LeaseAcquireResult : std::int64_t
   {
      NotAcquired = 0,
      Acquired = 1
   };

   enum class LeaseRenewResult : std::int64_t
   {
      Lost = 0,
      Renewed = 1
   };

   enum class LeaseReleaseResult : std::int64_t
   {
      NotOwned = 0,
      Released = 1
   };

   enum class FenceResult : std::int64_t
   {
      NotEligible = 0,
      Fenced = 1,
      AlreadyCleaned = 2
   };

   enum class CleanupResult : std::int64_t
   {
      NotCleaned = 0,
      Cleaned = 1,
      AlreadyCleaned = 2
   };

   enum class TombstoneCollectionResult : std::int64_t
   {
      NotCollected = 0,
      Collected = 1
   };

   /**
   * @brief Converts a numer/**
   * @brief Converts a numeric Lua result to its corresponding strongly typed outcome.
   * @tparam Enum Result enum type.
   * @tparam Redis Redis++ client type.
   * @tparam Redis Redis connection type providing the required Redis++ commands.
   * @tparam Renew Callable that renews the distributed cleanup lease when necessary.
   * @tparam Redis Redis connection type providing the required Re/**
   * @brief Provides the checkedResult helper operation.
   * @param value Parameter used by this helper.
   * @param values Parameter used by this helper.
   * @param operation Parameter used by this helper.
   * @return Result produced by this helper.
   */
   dis++ commands.
   * @tparam Renew /
   * @tparam Enum Result enum type.
   * @tparam Redis Redis++ client type.
   * @tparam Redis Redis connection type providing the required Redis++ commands.
   * @tparam Renew Callable that renews the distributed cleanup lease when necessary.
   * @tparam Redis Redis connection type providing the required Redis++ commands.
   * @tparam Renew Callable that renews the distributed cleanup lease when necessary.
   * @tparam Redis R/
   * @tparam Enum Result enum type.
   * /**
   * @brief Provides the registryTypesKey helper operation.
   * @return Result produced by this helper.
   */
   @tparam Redis Redis++ client type.
   * @tparam Redis Redis connection type providing the required Redis++ commands.
   * @tparam Renew Callable th/**
   * @brief Provides the registryServersKey helper operation.
   * @param type Parameter used by this helper.
   * @return Result produced by this helper.
   */
   at renews the distributed cleanup lease when necessary.
   * @tparam Redis Redis connection type providing the required Redis++ commands.
   * @tparam Renew Callable that rene/**
   * @brief Provides the cleanupLeaseKey helper operation.
   * @return Result produced by this helper.
   */
   ws the distributed cleanup lease when necessary.
   * @tparam Redis Redis connection type providing the required Redis++ commands.
   * @tpa/**
   * @brief Provides the liveKey helper operation.
   * @param type Parameter used by this helper.
   * @return Result produced by this helper.
   */
   ram Enum Result enum type.
   * @tparam Redis Redis++ client type.
   * @tparam Redis Redis connection type providing the required Redis++ commands.
   /**
   * @brief Provides the stateKey helper operation.
   * @param type Parameter used by this helper.
   * @return Result produced by this helper.
   */
   * @tparam Renew Callable that renews the distributed cleanup lease when necessary.
   * @tparam Redis Redis connection type providing the required Redis++ comm/**
   * @brief Provides the retiredServersKey helper operation.
   * @param type Parameter used by this helper.
   * @return Result produced by this helper.
   */
   ands.
   * @tparam Renew Callable that renews the distributed cleanup lease when necessary.
   * @tparam Redis Redis connection type providing the requir/**
   * @brief Provides the heartbeatKey helper operation.
   * @param type Parameter used by this helper.
   * @param id Parameter used by this helper.
   * @return Result produced by this helper.
   */
   ed Redis++ commands.
   * @tparam Enum Result enum type.
   * @tparam Redis Redis++ client type.
   * @tparam Redis Redis connection type providing the required Redis++ comm/**
   * @brief Provides the serverJobsKey helper operation.
   * @param type Parameter used by this helper.
   * @param id Parameter used by this helper.
   * @return Result produced by this helper.
   */
   ands.
   * @tparam Renew Callable that renews the distributed cleanup lease when necessary.
   * @tparam Redis Redis connection type providing the required Redis++ commands.
   */**
   * @brief Provides the typeJobsKey helper operation.
   * @param type Parameter used by this helper.
   * @return Result produced by this helper.
   */
   @tparam Renew Callable that renews the distributed cleanup lease when necessary.
   * @tparam Redis Redis connection type providing t/**
   * @brief Provides the serverSummaryKey helper operation.
   * @param type Parameter used by this helper.
   * @param id Parameter used by this helper.
   * @return Result produced by this helper.
   */
   he required Redis++ commands.
   * @param type Parameter used by this helper.
   * @return Result produced by this helper.
   */
} // namespace

/**
 * @brief Constructs the cleanup manager, loads its configuration, and starts cleanup.
 * @param provider Source of manager timing settings; an empty provider selects defaults.
 * @throws std::invalid_argument if a configured duration is not positive.
 */
RedisHeartbeatManager::RedisHeartbeatManager(ConfigProvider provider)
{
   // Resolve the status configuration first. When disabled, do not initialize
   // Redis, validate heartbeat timing, or start the cleanup thread.
   const auto status = RedisJobStatus::Instance();
   if (!status->isEnabled())
   {
      return;
   }

   m_config = provider ? provider() : Config{};
   if (m_config.heartbeatTtlSeconds <= 0 ||
         m_config.heartbeatRefreshIntervalSeconds <= 0 ||
         m_config.cleanupLeaseTtlMilliseconds <= 0 ||
         m_config.cleanupLeaseRenewIntervalSeconds <= 0 ||
         m_config.cleanupScanIntervalSeconds <= 0 ||
         m_config.firstHeartbeatTimeoutSeconds <= 0 ||
         m_config.retiredTombstoneRetentionSeconds <= 0)
   {
      throw std::invalid_argument(
            "RedisHeartbeatManager: all configured durations must be positive");
   }

   m_client = RedisClient::Instance();
   m_enabled = true;
   m_cleanupThread = std::thread(&RedisHeartbeatManager::cleanupLoop, this);
}

/** @brief Stops the cleanup thread before destroying the manager. */
RedisHeartbeatManager::~RedisHeartbeatManager()
{
   stop();
}

/**
 * @brief Starts the process-wide cleanup manager exactly once.
 * @param provider Optional source of manager timing settings.
 */
std::shared_ptr<RedisHeartbeatManager> RedisHeartbeatManager::Instance(
      ConfigProvider provider)
{
   static const std::shared_ptr<RedisHeartbeatManager> instance(
         new RedisHeartbeatManager(std::move(provider)));
   return instance;
}

/**
 * @brief Starts the process-wide heartbeat publisher once.
 *
 * Uses the configured RedisJobStatus singleton and manager heartbeat timings.
 */
void RedisHeartbeatManager::HeartbeatStart()
{
   const auto status = RedisJobStatus::Instance();
   if (!m_enabled || !status->isEnabled())
   {
      return;
   }

   std::lock_guard<std::mutex> lock(m_heartbeatMutex);

   if (!m_heartbeatRunner)
   {
      m_heartbeatRunner.reset(new HeartbeatRunner(
            status,
            m_config.heartbeatTtlSeconds,
            m_config.heartbeatRefreshIntervalSeconds));
   }
}

/**
 * @brief Stops and joins the cleanup thread.
 *
 * The heartbeat publisher has an independent lifetime and is stopped when
 * the manager is destroyed.
 */
void RedisHeartbeatManager::stop()
{
   {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
   }

   m_wakeup.notify_all();

   if (m_cleanupThread.joinable())
   {
      m_cleanupThread.join();
   }
}

/**
 * @brief Runs cleanup scans until shutdown is requested.
 *
 * Scan failures are logged; the loop remains alive and retries after the
 * configured scan interval.
 */
void RedisHeartbeatManager::cleanupLoop()
{
   std::unique_lock<std::mutex> lock(m_mutex);

   while (!m_stopping)
   {
      lock.unlock();

      try
      {
         scanOnce();
      }
      catch (const std::exception& e)
      {
         std::cerr << "RedisHeartbeatManager: cleanup scan failed: "
                        << e.what() << '\n';
      }
      catch (...)
      {
         std::cerr
               << "RedisHeartbeatManager: cleanup scan failed: unknown exception\n";
      }

      lock.lock();
      m_wakeup.wait_for(
            lock,
            std::chrono::seconds(m_config.cleanupScanIntervalSeconds),
            [this]
            {
               return m_stopping;
            });
   }
}

/**
 * @brief Performs one distributed scan for workers with expired heartbeats.
 *
 * The process first acquires the registry-slot cleanup lease. For each
 * registered server, a per-type Lua script performs authoritative fencing.
 * Once fenced, a second Lua script removes the server's jobs and summaries.
 * The lease is renewed during long scans and released on success or failure.
 */
void RedisHeartbeatManager::scanOnce()
{
   if (!m_enabled)
   {
      return;
   }

   const string token = ownerToken();
   m_client->execute([&](auto& redis)
   {
      runCleanupScan(redis, m_config, token);
   });
}
