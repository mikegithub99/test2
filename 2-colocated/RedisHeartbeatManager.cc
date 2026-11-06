/**
 * @file RedisHeartbeatManager.cc
 * @brief Implements worker heartbeat publication and distributed cleanup.
 */

#include "RedisHeartbeatManager.h"

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
constexpr auto kLeaseRenewInterval = std::chrono::seconds(5);

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

/** @brief Returns the mutex protecting the process-wide cleanup manager. */
std::mutex& managerMutex()
{
  static std::mutex value;
  return value;
}

/** @brief Returns the process-wide cleanup manager instance. */
std::unique_ptr<RedisHeartbeatManager>& processManager()
{
  static std::unique_ptr<RedisHeartbeatManager> value;
  return value;
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
 * @class HeartbeatRunner
 * @brief Owns the background thread that periodically refreshes one worker heartbeat.
 *
 * The runner retains shared ownership of RedisJobStatus. It refreshes the
 * heartbeat immediately, then waits between refresh attempts. A failed refresh
 * is logged and retried on the next interval.
 */
class HeartbeatRunner final
{
public:
  /**
   * @brief Creates the runner and starts its background thread.
   * @param status Worker status whose heartbeat is published.
   */
  HeartbeatRunner(
      std::shared_ptr<RedisJobStatus> status,
      std::int64_t heartbeatTtlSeconds)
      : m_status(std::move(status)),
        m_heartbeatTtlSeconds(heartbeatTtlSeconds),
        m_thread(&HeartbeatRunner::run, this)
  {
  }

  /**
   * @brief Requests shutdown and joins the heartbeat thread.
   */
  ~HeartbeatRunner()
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
  /** @brief Refreshes the heartbeat immediately and on each configured interval. */
  void run()
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
          std::chrono::seconds(10),
          [this]
          {
            return m_stopping;
          });
    }
  }

  std::shared_ptr<RedisJobStatus> m_status; ///< Retained worker status object.
  std::int64_t m_heartbeatTtlSeconds; ///< TTL supplied to heartbeat refreshes.
  std::mutex m_mutex; ///< Protects the runner shutdown flag.
  std::condition_variable m_wakeup; ///< Wakes the runner during shutdown.
  bool m_stopping = false; ///< True after shutdown is requested.
  std::thread m_thread; ///< Background heartbeat publication thread.
};

namespace
{
/** @brief Returns the mutex protecting the process-wide heartbeat runner. */
std::mutex& heartbeatRunnerMutex()
{
  static std::mutex value;
  return value;
}

/** @brief Returns the process-wide heartbeat runner instance. */
std::unique_ptr<HeartbeatRunner>& processHeartbeatRunner()
{
  static std::unique_ptr<HeartbeatRunner> value;
  return value;
}
} // namespace

/**
 * @brief Constructs the cleanup manager and starts its background thread.
 */
RedisHeartbeatManager::RedisHeartbeatManager(const Config& config)
    : m_config(config),
      m_client(RedisClient::Instance())
{
  m_cleanupThread = std::thread(&RedisHeartbeatManager::cleanupLoop, this);
}

/** @brief Stops the cleanup thread before destroying the manager. */
RedisHeartbeatManager::~RedisHeartbeatManager()
{
  stop();
}

/**
 * @brief Starts the process-wide cleanup manager exactly once.
 */
void RedisHeartbeatManager::Start(ConfigProvider provider)
{
  std::lock_guard<std::mutex> lock(managerMutex());
  auto& manager = processManager();

  if (!manager)
  {
    const Config config = provider ? provider() : Config{};
    if (config.heartbeatTtlSeconds <= 0 ||
        config.cleanupLeaseTtlMilliseconds <= 0)
    {
      throw std::invalid_argument(
          "RedisHeartbeatManager: configured TTL values must be positive");
    }
    manager.reset(new RedisHeartbeatManager(config));
  }
}

/**
 * @brief Starts the process-wide heartbeat publisher exactly once.
 * @param status Shared worker status to retain and refresh.
 * @throws std::invalid_argument if status is null.
 */
void RedisHeartbeatManager::HeartbeatStart(
    std::shared_ptr<RedisJobStatus> status)
{
  if (!status)
  {
    throw std::invalid_argument(
        "HeartbeatStart requires a non-null RedisJobStatus");
  }

  // Ensure the runner uses the same injected TTL configuration as cleanup.
  Start();

  std::int64_t heartbeatTtlSeconds = 30;
  {
    std::lock_guard<std::mutex> managerLock(managerMutex());
    heartbeatTtlSeconds = processManager()->m_config.heartbeatTtlSeconds;
  }

  std::lock_guard<std::mutex> lock(heartbeatRunnerMutex());
  auto& runner = processHeartbeatRunner();

  if (!runner)
  {
    runner.reset(new HeartbeatRunner(std::move(status), heartbeatTtlSeconds));
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
        m_scanInterval,
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

          if (now - lastRenewal < kLeaseRenewInterval)
          {
            return;
          }

          const vector<string> args = {
              token, std::to_string(kLeaseTtlMs)};

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
                  liveKey(type)};
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
