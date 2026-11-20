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
   /**
    * @brief Result codes returned by job lifecycle operations.
    *
    * Numeric values are part of the Lua-script/C++ result contract and must remain stable.
    */
   enum class Result : std::int64_t
   {
      /** Operation completed successfully. */
      Success = 0,
      /**
       * Requested operation required no state change.
       */
      NoOp = 1,
      /** The requested job does not exist. */
      JobNotFound = 2,
      /** The requested job-state transition is invalid. */
      InvalidTransition = 3,
      /**
       * Redis contains inconsistent job data.
       */
      InconsistentData = 4,
      /** A job with the requested identifier already exists. */
      JobAlreadyExists = 5,
      /** Redis-backed status reporting is disabled. */
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
      /** Job has been created but not activated. */
      Pending,
      /**
       * Job is currently active.
       */
      Active,
      /** Job is temporarily suspended. */
      Suspended
   };

   /**
    * Configuration identifying and enabling this status reporter.
    */
   struct Config
   {
      /** Enables or disables Redis-backed status reporting. */
      bool enabled = true;
      /**
       * Configured server type used for Redis key construction.
       */
      std::string serverType;
      /** Configured server instance identifier. */
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
   /**
    * @brief Prevents copy assignment of the status API object.
    */
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
    * @param jobId Application job identifier.
    * @param newState Target lifecycle state.
    * @param metadata Metadata name/value pairs to add or update. Existing
    *        metadata names not supplied remain unchanged. Reserved lifecycle
    *        fields are managed by the API. Metadata is applied even when the
    *        requested state is already current and the result is NoOp.
    */
   Result transitionJob(
         const std::string& jobId,
         State newState,
         const std::map<std::string, std::string>& metadata = {});

   /**
    * @brief Removes a tracked job and updates indexes/counters atomically.
    * @param jobId Application job identifier to remove.
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
    * @param ttlSeconds Positive heartbeat TTL in seconds.
    */
   void refreshHeartbeat(std::int64_t ttlSeconds);

 private:
   /**
    * @brief Validates caller-supplied registration metadata.
    * @param serverTypeMetadata Metadata merged into the server-type summary.
    * @param serverMetadata Metadata merged into this server's summary.
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
    * @param serverTypeMetadata Metadata merged into the server-type summary.
    * @param serverMetadata Metadata merged into this server's summary.
    */
   void activateRegisteredServer(
         const std::map<std::string, std::string>& serverTypeMetadata,
         const std::map<std::string, std::string>& serverMetadata) const;

   /**
    * @brief Constructs the singleton using the first call's configuration provider.
    */
   explicit RedisJobStatus(ConfigProvider provider);

   /**
    * @brief Validates the configured server identity and supplied job identifier.
    * @param jobId Application job identifier to validate.
    */
   void validateLifecycleIdentity(const std::string& jobId) const;
   /** Indicates whether Redis-backed status reporting is enabled. */
   bool m_enabled = true;
   /**
    * Configured server type used for Redis key construction.
    */
   std::string m_serverType;
   /** Configured server instance identifier. */
   std::string m_serverId;
   /** Redis client used by job-status operations. */
   std::shared_ptr<RedisClient> m_client;
};

#endif // REDIS_JOB_STATUS_H
