/**
 * @file RedisJobStatus.h
 * @brief Logical job-status APIs backed by Redis/Redis Cluster.
 *
 * Requires C++17 and the RedisClient wrapper.
 */

#ifndef REDIS_JOB_STATUS_H
#define REDIS_JOB_STATUS_H

#include <cstdint>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RedisClient.h"

/**
 * @class RedisJobStatus
 * @brief Implements atomic job lifecycle operations and dashboard reads.
 *
 * Server types are tracked in SET servers:{registry}:types; each type's
 * instances are tracked in servers:{registry}:type:serverType. Both registry
 * keys share the {registry} Redis Cluster hash tag for atomic registration.
 *
 * All lifecycle keys use the registered server type as their Redis Cluster
 * hash tag. Worker identity is read from host configuration at construction
 * and retained by this object for lifecycle and heartbeat calls. Lua scripts
 * validate ownership against the stored job HASH before changing state.
 */
class RedisJobStatus final
{
public:
  // Numeric values are part of the Lua-script/C++ result contract; keep stable.
  enum class Result : std::int64_t
  {
    Success = 0,
    NoOp = 1,
    JobNotFound = 2,
    InvalidTransition = 3,
    InconsistentData = 4,
    JobAlreadyExists = 5,
    Disabled = 6,
    ServerNotActive = 7
  };

  enum class State
  {
    Pending,
    Active,
    Suspended
  };

  struct Job
  {
    std::string jobId;
    std::string status;
    std::string serverType;
    std::string serverId;
    std::map<std::string, std::string> fields;
  };

  struct ServerSummary
  {
    std::string serverId;
    std::map<std::string, std::int64_t> counters;
  };

  struct Config
  {
    bool enabled = true;
    std::string serverType;
    std::string serverId;
  };

  using ConfigProvider = std::function<Config()>;

  /**
   * @brief Gets the process-wide status API, initializing it lazily.
   * @param provider Optional configuration provider. If omitted, the default
   *        provider reads redisJobStatusEnabled, redisJobStatusServerType,
   *        and redisJobStatusServerId through get_reg_val(). A supplied
   *        provider must be passed on the first call.
   */
  static std::shared_ptr<RedisJobStatus> Instance(ConfigProvider provider = {});

  RedisJobStatus(const RedisJobStatus&) = delete;
  RedisJobStatus& operator=(const RedisJobStatus&) = delete;

  /**
   * @brief Creates a PENDING job under this object's configured server identity.
   * @param jobId Original application job ID; preserved unchanged for display.
   * @param metadata Additional job display metadata; reserved fields are supplied by the API.
   */
  Result createPendingJob(
      const std::string& jobId,
      const std::map<std::string, std::string>& metadata = {});

  /**
   * @brief Transitions a tracked job to ACTIVE or SUSPENDED.
   *
   * Valid transitions are PENDING->ACTIVE, ACTIVE->SUSPENDED, and
   * SUSPENDED->ACTIVE. Repeating the current state returns NoOp.
   */
  Result transitionJob(
      const std::string& jobId,
      State newState);

  /**
   * @brief Removes a tracked job and updates indexes/counters atomically.
   */
  Result completeJob(const std::string& jobId);

  /**
   * @brief Registers this configured server and stores its metadata.
   * @param serverTypeMetadata Metadata merged into the server-type summary.
   * @param serverMetadata Metadata merged into this server's summary.
   */
  void registerServer(
      const std::map<std::string, std::string>& serverTypeMetadata = {},
      const std::map<std::string, std::string>& serverMetadata = {});

  /** @brief Refreshes this object's heartbeat with a positive TTL in seconds. */
  void refreshHeartbeat(std::int64_t ttlSeconds);

  /** @brief Discovers server types registered by workers. */
  std::vector<std::string> getServerTypes();

  /** @brief Returns the materialized summary for one server type. */
  std::map<std::string, std::int64_t> getTypeSummary(
      const std::string& serverType);

  /** @brief Returns registered instances and their summary counters. */
  std::vector<ServerSummary> getServerSummaries(
      const std::string& serverType);

  /**
   * @brief Reads current jobs for an instance.
   *
   * This returns the complete current set; callers should add bounded
   * pagination if an instance can own a large number of jobs.
   */
  std::vector<Job> getServerJobs(
      const std::string& serverType,
      const std::string& serverId);

private:
  /** @brief Constructs the singleton using the first call's configuration provider. */
  explicit RedisJobStatus(ConfigProvider provider);

  static std::string stateName(State state);
  static void validateIdentifier(
      const std::string& value,
      const char* name,
      bool isServerType = false);
  void validateLifecycleIdentity(const std::string& jobId) const;
  bool m_enabled = true;
  std::string m_serverType;
  std::string m_serverId;
  std::shared_ptr<RedisClient> m_client;
};

#endif // REDIS_JOB_STATUS_H
