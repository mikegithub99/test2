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
   /**
    * Result codes returned by job lifecycle operations.
    */
   enum class Result : std::int64_t
   {
      Success = 0,
      /**
       * Requested operation required no state change.
       */
      NoOp = 1,
      JobNotFound = 2,
      InvalidTransition = 3,
      /**
       * Redis contains inconsistent job data.
       */
      InconsistentData = 4,
      JobAlreadyExists = 5,
      Disabled = 6,
      /**
       * The server is not active.
       */
      ServerNotActive = 7
   };

   /**
    * Lifecycle states supported by the job API.
    */
   enum class State
   {
      Pending,
      /**
       * Job is currently active.
       */
      Active,
      Suspended
   };

   /**
    * Job record returned by dashboard read operations.
    */
   struct Job
   {
      std::string jobId;
      /**
       * Current lifecycle status of the job.
       */
      std::string status;
      std::string serverType;
      std::string serverId;
      /**
       * Additional job metadata fields.
       */
      std::map<std::string, std::string> fields;
   };

   /**
    * Summary counters for one registered server instance.
    */
   struct ServerSummary
   {
      std::string serverId;
      /**
       * Named counters materialized for the server.
       */
      std::map<std::string, std::int64_t> counters;
   };

   /**
    * Configuration identifying and enabling this status reporter.
    */
   struct Config
   {
      bool enabled = true;
      /**
       * Configured server type used for Redis key construction.
       */
      std::string serverType;
      std::string serverId;
   };

   /**
    * Callable that supplies status reporter configuration.
    */
   using ConfigProvider = std::function<Config()>;

   /**
    * @brief Gets the process-wide status API, initializing it lazily.
    * @param provider Optional configuration provider. If omitted, the default
    *        provider reads redisJobStatusEnabled, redisJobStatusServerType,
    *        and redisJobStatusServerId through get_reg_val(). A supplied
    *        provider must be passed on the first call.
    */
   static std::shared_ptr<RedisJobStatus> Instance(ConfigProvider provider = {});

   /**
    * @brief Returns whether Redis-backed status reporting is enabled.
    */
   bool isEnabled() const noexcept
   {
      return m_enabled;
   }

   /**
    * Prevents copying the status API object.
    */
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

   /**
    * @brief Refreshes this object's heartbeat with a positive TTL in seconds.
    */
   void refreshHeartbeat(std::int64_t ttlSeconds);

   /**
    * @brief Discovers server types registered by workers.
    */
   std::vector<std::string> getServerTypes();

   /**
    * @brief Returns the materialized summary for one server type.
    */
   std::map<std::string, std::int64_t> getServerTypeSummary(
         const std::string& serverType);

   /**
    * @brief Returns registered instances and their summary counters.
    */
   std::vector<ServerSummary> getServerSummaries(
         const std::string& serverType);

   /**
    * @brief Best-effort read of jobs for one server instance.
    *
    * References are enumerated before the Lua snapshot. Jobs removed before
    * the snapshot are skipped; jobs added after enumeration are not included.
    * Existing records are checked for serverType/serverId ownership.
    */
   std::vector<Job> getServerJobs(
         const std::string& serverType,
         const std::string& serverId);

   /**
    * @brief Best-effort read of jobs for all servers of one server type.
    *
    * Uses the type-wide job index. Jobs removed before the Lua snapshot are
    * skipped; jobs added after enumeration are not included. Existing records
    * are checked for serverType ownership.
    */
   std::vector<Job> getServerTypeJobs(const std::string& serverType);

 private:
   enum class RegistrationResult : std::int64_t
   {
      AlreadyPresent = 0,
      Added = 1
   };

   enum class ActivationResult : std::int64_t
   {
      AlreadyHasLifecycleState = 0,
      Activated = 1
   };

   enum class HeartbeatResult : std::int64_t
   {
      Rejected = 0,
      Refreshed = 1
   };

   /**
    * @brief Converts a lifecycle script result, rejecting unknown values.
    */
   static Result resultFromScript(std::int64_t value);
   /**
    * @brief Converts a registry script result, rejecting unknown values.
    */
   static RegistrationResult registrationResultFromScript(std::int64_t value);
   /**
    * @brief Converts an activation script result, rejecting unknown values.
    */
   static ActivationResult activationResultFromScript(std::int64_t value);
   /**
    * @brief Converts a heartbeat script result, rejecting unknown values.
    */
   static HeartbeatResult heartbeatResultFromScript(std::int64_t value);

   /**
    * @brief Validates caller-supplied registration metadata.
    */
   void validateRegistrationMetadata(
         const std::map<std::string, std::string>& serverTypeMetadata,
         const std::map<std::string, std::string>& serverMetadata) const;

   /**
    * @brief Inserts the server into the global registry slot after checking its state.
    */
   void registerServerInRegistry() const;

   /**
    * @brief Writes per-type metadata and activates the server in its type slot.
    */
   void activateRegisteredServer(
         const std::map<std::string, std::string>& serverTypeMetadata,
         const std::map<std::string, std::string>& serverMetadata) const;

   /**
    * @brief Constructs the singleton using the first call's configuration provider.
    */
   explicit RedisJobStatus(ConfigProvider provider);

   static std::string stateName(State state);
   /**
    * Validates a server type or server identifier.
    */
   static void validateIdentifier(
         const std::string& value,
         const char* name,
         /**
          * Selects server-type-specific validation when true.
          */
         bool isServerType = false);
   void validateLifecycleIdentity(const std::string& jobId) const;
   bool m_enabled = true;
   /**
    * Configured server type used for Redis key construction.
    */
   std::string m_serverType;
   std::string m_serverId;
   std::shared_ptr<RedisClient> m_client;
};

#endif // REDIS_JOB_STATUS_H
