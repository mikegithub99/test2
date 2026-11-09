/**
 * @file RedisHeartbeatManager.cc
 * @brief Implements worker heartbeat publication and distributed cleanup.
 */

#include "RedisHeartbeatManager.h"

#include "HeartbeatRunner.h"
#include "RedisJobStatus.h"
#include "RedisJobScripts.inc"

#include <iostream>
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
/** @brief Returns the global Redis set containing registered server types. */
string registryTypesKey()
{
  return "servers:{registry}:types";
}

/** @brief Returns the registry set containing server IDs for one type. */
string registryServersKey(const string& type)
{
  return "servers:{registry}:type:" + type;
}

/** @brief Returns the Redis key used for the distributed cleanup lease. */
string cleanupLeaseKey()
{
  return "servers:{registry}:cleanup-lease";
}

/** @brief Returns the live-server set for a server type. */
string liveKey(const string& type)
{
  return "servers:live:{" + type + "}";
}

/** @brief Returns the lifecycle-state hash for a server type. */
string stateKey(const string& type)
{
  return "servers:state:{" + type + "}";
}

/** @brief Returns the per-type sorted set indexing retired server IDs. */
string retiredServersKey(const string& type)
{
  return "servers:retired:{" + type + "}";
}

/** @brief Returns the heartbeat key for one server. */
string heartbeatKey(const string& type, const string& id)
{
  return "heartbeat:server:{" + type + "}:" + id;
}

/** @brief Returns the server's job-reference set. */
string serverJobsKey(const string& type, const string& id)
{
  return "jobs:server:{" + type + "}:" + id;
}

/** @brief Returns the job-reference set for a server type. */
string typeJobsKey(const string& type)
{
  return "jobs:type:{" + type + "}";
}

/** @brief Returns the per-server summary hash. */
string serverSummaryKey(const string& type, const string& id)
{
  return "summary:server:{" + type + "}:" + id;
}

/** @brief Returns the per-type summary hash. */
string typeSummaryKey(const string& type)
{
  return "summary:type:{" + type + "}";
}

/**
 * @brief Creates a process-unique token for cleanup-lease ownership.
 * @return A hexadecimal token containing a clock value and random components.
 */
string ownerToken()
{
  std::random_device rd;
  std::ostringstream out;
  out << std::hex << std::chrono::steady_clock::now().time_since_epoch().count();

  for (int i = 0; i < 4; ++i)
  {
    out << '-' << rd();
  }

  return out.str();
}

/**
 * @brief Evaluates a Redis Lua script and converts its result to int64_t.
 * @tparam Redis Redis++ client type.
 * @param redis Redis client.
 * @param script Lua script source.
 * @param keys Redis keys supplied as KEYS.
 * @param args arguments supplied as ARGV.
 * @return Integer result returned by the script.
 */
template <typename Redis>
int64_t evalScript(
    Redis& redis,
    const char* script,
    const vector<string>& keys,
    const vector<string>& args)
{
  return redis.template eval<int64_t>(
      script, keys.begin(), keys.end(), args.begin(), args.end());
}

} // namespace

/**
 * @brief Constructs the cleanup manager, loads its configuration, and starts cleanup.
 * @param provider Source of manager timing settings; an empty provider selects defaults.
 * @throws std::invalid_argument if a configured duration is not positive.
 */
RedisHeartbeatManager::RedisHeartbeatManager(ConfigProvider provider)
    : m_config(provider ? provider() : Config{}),
      m_client(RedisClient::Instance())
{
  if (m_config.heartbeatTtlSeconds <= 0 ||
      m_config.heartbeatRefreshIntervalSeconds <= 0 ||
      m_config.cleanupLeaseTtlMilliseconds <= 0 ||
      m_config.cleanupLeaseRenewIntervalSeconds <= 0 ||
      m_config.cleanupScanIntervalSeconds <= 0 ||
      m_config.retiredTombstoneRetentionSeconds <= 0)
  {
    throw std::invalid_argument(
        "RedisHeartbeatManager: all configured durations must be positive");
  }

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
 * @brief Starts the process-wide heartbeat publisher exactly once.
 * @param status Shared worker status to retain and refresh.
 * @throws std::invalid_argument if status is null.
 */
void RedisHeartbeatManager::HeartbeatStart()
{
  const auto status = RedisJobStatus::Instance();

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
 * @brief Signals the cleanup loop to stop and waits for its thread to finish.
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
  const string token = ownerToken();
  const vector<string> leaseKeys = {cleanupLeaseKey()};
  const vector<string> acquireArgs = {
      token, std::to_string(m_config.cleanupLeaseTtlMilliseconds)};

  m_client->execute(
      [&](auto& redis)
      {
        if (evalScript(
                redis,
                kAcquireCleanupLeaseScript,
                leaseKeys,
                acquireArgs) != 1)
        {
          return; // Another process owns the registry scan lease.
        }

        auto lastRenewal = std::chrono::steady_clock::now();

        /**
         * @brief Renews the cleanup lease when its renewal interval elapses.
         * @throws std::runtime_error if this process no longer owns the lease.
         */
        const auto renewIfNeeded = [&]()
        {
          const auto now = std::chrono::steady_clock::now();

          if (now - lastRenewal <
              std::chrono::seconds(m_config.cleanupLeaseRenewIntervalSeconds))
          {
            return;
          }

          const vector<string> args = {
              token, std::to_string(m_config.cleanupLeaseTtlMilliseconds)};

          if (evalScript(
                  redis,
                  kRenewCleanupLeaseScript,
                  leaseKeys,
                  args) != 1)
          {
            throw std::runtime_error("cleanup scan lease was lost");
          }

          lastRenewal = now;
        };

        /** @brief Releases the lease only if this process still owns it. */
        const auto release = [&]()
        {
          const vector<string> args = {token};
          evalScript(redis, kReleaseCleanupLeaseScript, leaseKeys, args);
        };

        try
        {
          vector<string> types;
          redis.smembers(registryTypesKey(), back_inserter(types));

          for (const auto& type : types)
          {
            renewIfNeeded();

            vector<string> ids;
            redis.smembers(registryServersKey(type), back_inserter(ids));

            for (const auto& id : ids)
            {
              // If the lease expires during a slow Redis operation, overlapping
              // scans are safe because per-server Lua fencing is authoritative.
              renewIfNeeded();

              const vector<string> fenceKeys = {
                  heartbeatKey(type, id),
                  stateKey(type),
                  serverJobsKey(type, id),
                  serverSummaryKey(type, id),
                  liveKey(type),
                  retiredServersKey(type)};
              const vector<string> fenceArgs = {id};

              const auto fenced = redis.template eval<int64_t>(
                  kFenceExpiredServerScript,
                  fenceKeys.begin(),
                  fenceKeys.end(),
                  fenceArgs.begin(),
                  fenceArgs.end());

              if (fenced == 0)
              {
                continue;
              }

              if (fenced == 2)
              {
                redis.srem(registryServersKey(type), id);
                continue;
              }

              if (fenced != 1)
              {
                continue;
              }

              const string serverJobs = serverJobsKey(type, id);
              vector<string> refs;
              redis.smembers(serverJobs, back_inserter(refs));

              vector<string> keys = {
                  liveKey(type),
                  stateKey(type),
                  heartbeatKey(type, id),
                  serverJobs,
                  typeJobsKey(type),
                  serverSummaryKey(type, id),
                  typeSummaryKey(type)};

              for (const auto& ref : refs)
              {
                keys.push_back("job:{" + type + "}:" + ref);
              }

              vector<string> args = {
                  id, std::to_string(refs.size())};

              for (const auto& ref : refs)
              {
                args.push_back(ref);
              }

              args.push_back(type);

              const auto cleaned = redis.template eval<int64_t>(
                  kCleanupExpiredServerScript,
                  keys.begin(),
                  keys.end(),
                  args.begin(),
                  args.end());

              if (cleaned == 1 || cleaned == 2)
              {
                redis.srem(registryServersKey(type), id);
              }
            }

            // Backfill tombstones left by an older two-phase deployment.
            // HSCAN is incremental; repeated scans safely cover concurrent changes.
            const vector<string> indexKeys = {
                stateKey(type), retiredServersKey(type)};
            string cursor = "0";
            do
            {
              renewIfNeeded();
              const vector<string> indexArgs = {cursor};
              cursor = redis.template eval<string>(
                  kIndexUntrackedTombstonesScript,
                  indexKeys.begin(),
                  indexKeys.end(),
                  indexArgs.begin(),
                  indexArgs.end());
            } while (cursor != "0");

            // Phase C candidates are discovered from the retired index because
            // Phase B has already removed their IDs from the registry set.
            renewIfNeeded();
            const vector<string> retiredIndexKeys = {retiredServersKey(type)};
            const vector<string> retentionArgs = {
                std::to_string(m_config.retiredTombstoneRetentionSeconds * 1000)};
            const auto expiredIds = redis.template eval<vector<string>>(
                kListExpiredServerTombstonesScript,
                retiredIndexKeys.begin(),
                retiredIndexKeys.end(),
                retentionArgs.begin(),
                retentionArgs.end());

            for (const auto& id : expiredIds)
            {
              renewIfNeeded();

              // Registry and per-type keys occupy different Cluster slots.
              // Once-only registration makes absence stable after Phase B.
              if (redis.sismember(registryServersKey(type), id))
              {
                continue;
              }

              const vector<string> collectKeys = {
                  stateKey(type),
                  heartbeatKey(type, id),
                  serverJobsKey(type, id),
                  serverSummaryKey(type, id),
                  liveKey(type),
                  retiredServersKey(type)};
              const vector<string> collectArgs = {
                  id,
                  std::to_string(m_config.retiredTombstoneRetentionSeconds * 1000)};
              evalScript(
                  redis,
                  kCollectRetiredServerTombstoneScript,
                  collectKeys,
                  collectArgs);
            }
          }
        }
        catch (...)
        {
          try
          {
            release();
          }
          catch (...)
          {
          }

          throw;
        }

        release();
      });
}
