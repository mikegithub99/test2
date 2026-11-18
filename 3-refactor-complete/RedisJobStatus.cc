/**
 * @file RedisJobStatus.cc
 * @brief Implements Redis-backed job lifecycle and dashboard reads.
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <algorithm>
#include <cctype>
#include <limits>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "RedisJobStatus.h"

using std::back_inserter;
using std::tolower;
using std::transform;
using std::exception;
using std::int64_t;
using std::invalid_argument;
using std::map;
using std::pair;
using std::runtime_error;
using std::stoll;
using std::stoull;
using std::string;
using std::to_string;
using std::vector;

// Supplied by the host application's configuration registry.
string get_reg_val(const string& key);

namespace
{
#include "RedisJobScripts.inc"


   /** @brief Builds a collision-safe reference from a server ID and job ID.
      * @param serverId Server identifier.
      * @param jobId Job identifier.
      * @return Result produced by this helper.
      */
   string jobReference(const string& serverId, const string& jobId)
   {
      return to_string(serverId.size()) + ":" + serverId + ":" + jobId;
   }

   /** @brief Builds the Redis hash key for a job in the server-type Cluster slot.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @param serverId Server identifier.
      * @param jobId Job identifier.
      * @return Result produced by this helper.
      */
   string jobKey(const string& serverType, const string& serverId, const string& jobId)
   {
      return "job:{" + serverType + "}:" + jobReference(serverId, jobId);
   }

   /** @brief Returns the sorted-set key indexing jobs for a server type.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @return Result produced by this helper.
      */
   string typeJobsKey(const string& serverType)
   {
      return "jobs:type:{" + serverType + "}";
   }

   /** @brief Returns the sorted-set key indexing jobs owned by one server.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @param serverId Server identifier.
      * @return Result produced by this helper.
      */
   string serverJobsKey(
         const string& serverType,
         const string& serverId)
   {
      return "jobs:server:{" + serverType + "}:" + serverId;
   }

   /** @brief Returns the summary hash key for a server type.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @return Result produced by this helper.
      */
   string typeSummaryKey(const string& serverType)
   {
      return "summary:type:{" + serverType + "}";
   }

   /** @brief Returns the summary hash key for one server.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @param serverId Server identifier.
      * @return Result produced by this helper.
      */
   string serverSummaryKey(
         const string& serverType,
         const string& serverId)
   {
      return "summary:server:{" + serverType + "}:" + serverId;
   }

   /** @brief Returns the registry-slot set key for servers of one type.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @return Result produced by this helper.
      */
   string serverRegistryKey(const string& serverType)
   {
      return "servers:{registry}:type:" + serverType;
   }

   const string kServerTypesKey = "servers:{registry}:types";

   /** @brief Returns the heartbeat key for one server.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @param serverId Server identifier.
      * @return Result produced by this helper.
      */
   string heartbeatKey(
         const string& serverType,
         const string& serverId)
   {
      return "heartbeat:server:{" + serverType + "}:" + serverId;
   }

   /** @brief Returns the live-server set key for a server type.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @return Result produced by this helper.
      */
   string liveServersKey(const string& serverType)
   {
      return "servers:live:{" + serverType + "}";
   }

   /** @brief Returns the lifecycle-state hash key for a server type.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @return Result produced by this helper.
      */
   string serverStateKey(const string& serverType)
   {
      return "servers:state:{" + serverType + "}";
   }

   using KeyList = vector<string>;
   using ArgList = vector<string>;

   /** @brief Builds the same-slot Redis keys used by job lifecycle scripts.
      * @param jobId Job identifier.
      * @param serverType Server type used as the Redis Cluster hash tag.
      * @param serverId Server identifier.
      * @return Result produced by this helper.
      */
   KeyList lifecycleKeys(
         const string& jobId,
         const string& serverType,
         const string& serverId)
   {
      return {
            jobKey(serverType, serverId, jobId),
            typeJobsKey(serverType),
            serverJobsKey(serverType, serverId),
            typeSummaryKey(serverType),
            serverSummaryKey(serverType, serverId),
            serverStateKey(serverType)};
   }

   // Job hash fields reserved for the lifecycle API.
   constexpr const char* kJobIdField = "jobId";
   constexpr const char* kStatusField = "status";
   constexpr const char* kServerTypeField = "serverType";
   constexpr const char* kServerIdField = "serverId";
   constexpr const char* kReservedJobMetadataFields[] = {
         kJobIdField, kStatusField, kServerTypeField, kServerIdField};

   // Type-summary metadata initialized once per server type.
   constexpr const char* kCreatedAtMetadataField = "createdAt";
   constexpr const char* kCreatedByServerIdMetadataField = "createdByServerId";

   /** @brief Reports whether a job metadata field is owned by the lifecycle API.
      * @param field Metadata field name to inspect.
      * @return Result produced by this helper.
      */
   bool isReservedJobMetadataField(const string& field)
   {
      for (const char* reserved : kReservedJobMetadataFields)
      {
         if (field == reserved)
         {
            return true;
         }
      }
      return false;
   }

   /** @brief Reports whether a server-type metadata field is internally managed.
      * @param field Metadata field name to inspect.
      * @return Result produced by this helper.
      */
   bool isReservedServerTypeMetadataField(const string& field)
   {
      return field == kCreatedAtMetadataField ||
                  field == kCreatedByServerIdMetadataField;
   }

   /** @brief Reports whether a server metadata field is internally managed.
      * @param field Metadata field name to inspect.
      * @return Result produced by this helper.
      */
   bool isReservedServerMetadataField(const string& field)
   {
      return field == "registeredAt";
   }

   // Appends a count and accepted field/value pairs in the Lua argument format.
   /**
      * @brief Appends filtered metadata fields in the argument format expected by Lua scripts.
      * @tparam IncludeField Predicate deciding whether each metadata field is included.
      * @param args Destination Lua argument list; receives a count followed by field/value pairs.
      * @param metadata Metadata fields to append.
      * @param includeField Predicate applied to each field name.
      */
   template <typename IncludeField>
   void appendMetadataArgs(
         ArgList& args,
         const map<string, string>& metadata,
         IncludeField includeField)
   {
      const auto countIndex = args.size();
      args.push_back("0");

      std::size_t count = 0;
      for (const auto& item : metadata)
      {
         if (!includeField(item.first))
         {
            continue;
         }
         args.push_back(item.first);
         args.push_back(item.second);
         ++count;
      }

      args[countIndex] = to_string(count);
   }

   /**
      * @brief Evaluates a Redis Lua script and converts its result to a 64-bit integer.
      * @tparam Redis Redis client or connection type providing the eval interface.
      * @param redis Redis connection used to execute the script.
      * @param script Lua script source.
      * @param keys Redis keys supplied as KEYS.
      * @param args Arguments supplied as ARGV.
      * @return Integer result returned by the script.
      */
   template <typename Redis>
   int64_t evalResult(
         Redis& redis,
         const string& script,
         const KeyList& keys,
         const ArgList& args)
   {
      return redis.template eval<int64_t>(
            script, keys.begin(), keys.end(), args.begin(), args.end());
   }

   /**
      * @brief Evaluates a lifecycle script after prepending counter-validation logic.
      * @tparam Redis Redis client or connection type providing the eval interface.
      * @param redis Redis connection used to execute the script.
      * @param script Lifecycle Lua script source.
      * @param keys Redis keys supplied as KEYS.
      * @param args Arguments supplied as ARGV.
      * @return Integer result returned by the script.
      */
   template <typename Redis>
   int64_t evalCounterResult(
         Redis& redis,
         const char* script,
         const KeyList& keys,
         const ArgList& args)
   {
      const string validatedScript = string(kCounterValidationPrelude) + script;
      return evalResult(redis, validatedScript, keys, args);
   }


   /**
      * @brief Evaluates a Redis Lua script and converts its result to a string vector.
      * @tparam Redis Redis client or connection type providing the eval interface.
      * @param redis Redis connection used to execute the script.
      * @param script Lua script source.
      * @param keys Redis keys supplied as KEYS.
      * @param args Arguments supplied as ARGV.
      * @return String values returned by the script.
      */
   template <typename Redis>
   vector<string> evalStrings(Redis& redis, const string& script,
                                             const KeyList& keys, const ArgList& args)
   {
      return redis.template eval<vector<string>>(
            script, keys.begin(), keys.end(), args.begin(), args.end());
   }

   /** @brief Parses a non-negative decimal counter, rejecting malformed or overflowing values.
      * @param value Input value to parse or convert.
      * @param name Counter name used in diagnostic errors.
      * @return Result produced by this helper.
      */
   int64_t parseCounter(const string& value, const string& name)
   {
      try
      {
         if (value.empty() ||
               !std::all_of(value.begin(), value.end(),
                                    [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
         {
            throw invalid_argument("invalid counter syntax");
         }
         std::size_t used = 0;
         const auto parsed = stoll(value, &used);
         if (used != value.size())
         {
            throw invalid_argument("invalid counter");
         }
         return parsed;
      }
      catch (const exception&)
      {
         throw runtime_error("RedisJobStatus: invalid non-negative integer counter '" +
                                       name + "'");
      }
   }

   /** @brief Extracts numeric counters from a flat field/value snapshot, excluding metadata.
      * @param flat Flat field/value response returned by a Lua script.
      * @param offset Index of the first field in the flat response.
      * @param fieldCount Number of field/value pairs to parse.
      * @return Result produced by this helper.
      */
   map<string, int64_t> countersFromSnapshot(
         const vector<string>& flat, std::size_t offset, std::size_t fieldCount)
   {
      if (offset > flat.size() || fieldCount > (flat.size() - offset) / 2)
      {
         throw runtime_error("RedisJobStatus: malformed summary snapshot");
      }
      map<string, int64_t> counters;
      for (std::size_t i = 0; i < fieldCount; ++i)
      {
         const auto& name = flat[offset + i * 2];
         const auto& value = flat[offset + i * 2 + 1];
         if (name.rfind("meta:", 0) == 0)
         {
            continue;
         }
         counters[name] = parseCounter(value, name);
      }
      return counters;
   }

   std::size_t parseSnapshotSize(const string& value, const char* context);

   /** @brief Parses and validates the flat Lua response containing server summaries.
      * @param flat Flat field/value response returned by a Lua script.
      * @return Result produced by this helper.
      */
   vector<RedisJobStatus::ServerSummary> parseServerSummariesSnapshot(
         const vector<string>& flat)
   {
      if (flat.empty())
      {
         throw runtime_error("RedisJobStatus: empty summary snapshot");
      }

      std::size_t cursor = 0;
      const auto count = parseSnapshotSize(flat[cursor++], "summary record count");
      if (count > (flat.size() - cursor) / 2)
      {
         throw runtime_error("RedisJobStatus: malformed summary count");
      }

      vector<RedisJobStatus::ServerSummary> summaries;
      summaries.reserve(count);
      for (std::size_t i = 0; i < count; ++i)
      {
         if (cursor + 2 > flat.size())
         {
            throw runtime_error("RedisJobStatus: malformed summary record");
         }

         RedisJobStatus::ServerSummary summary;
         summary.serverId = flat[cursor++];
         const auto fields = parseSnapshotSize(flat[cursor++], "summary field count");
         summary.counters = countersFromSnapshot(flat, cursor, fields);
         cursor += fields * 2;
         summaries.push_back(std::move(summary));
      }

      if (cursor != flat.size())
      {
         throw runtime_error("RedisJobStatus: trailing summary snapshot data");
      }
      return summaries;
   }

   /**
      * @brief Reads numeric counters from a Redis summary hash, excluding metadata fields.
      * @tparam Redis Redis client or connection type providing the hgetall interface.
      * @param redis Redis connection used to read the hash.
      * @param key Redis summary hash key.
      * @return Map of counter names to their parsed values.
      */
   template <typename Redis>
   map<string, int64_t> readCounters(
         Redis& redis,
         const string& key)
   {
      vector<pair<string, string>> fields;
      redis.hgetall(key, back_inserter(fields));

      map<string, int64_t> counters;
      for (const auto& field : fields)
      {
         // Summary hashes also contain descriptive metadata; only numeric fields
         // belong in the counters API.
         if (field.first.rfind("meta:", 0) == 0)
         {
            continue;
         }
         counters[field.first] = parseCounter(field.second, field.first);
      }
      return counters;
   }

   /** @brief Parses a non-negative snapshot size and reports malformed values with context.
      * @param value Input value to parse or convert.
      * @param context Description of the value used in diagnostic errors.
      * @return Result produced by this helper.
      */
   std::size_t parseSnapshotSize(const string& value, const char* context)
   {
      try
      {
         if (value.empty() ||
               !std::all_of(value.begin(), value.end(),
                                    [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
         {
            throw invalid_argument("invalid unsigned integer syntax");
         }
         std::size_t used = 0;
         const auto parsed = stoull(value, &used);
         if (used != value.size())
         {
            throw invalid_argument("trailing characters");
         }
         return static_cast<std::size_t>(parsed);
      }
      catch (const exception&)
      {
         throw runtime_error(string("RedisJobStatus: malformed ") + context);
      }
   }

   /** @brief Parses the redisJobStatusEnabled configuration value as a boolean.
      * @param value Input value to parse or convert.
      * @return Result produced by this helper.
      */
   bool parseEnabledFlag(const string& value)
   {
      string normalized = value;
      transform(normalized.begin(), normalized.end(), normalized.begin(),
                     [](unsigned char ch)
                     {
                        return static_cast<char>(tolower(ch));
                     });

      if (normalized.empty() || normalized == "true" || normalized == "1" || normalized == "yes")
      {
         return true;
      }
      if (normalized == "false" || normalized == "0" || normalized == "no")
      {
         return false;
      }
      throw invalid_argument(
            "RedisJobStatus: redisJobStatusEnabled must be true/false, 1/0, or yes/no");
   }

   /** @brief Loads RedisJobStatus settings from the host application's configuration registry.
      * @return Result produced by this helper.
      */
   RedisJobStatus::Config defaultConfig()
   {
      RedisJobStatus::Config config;
      config.enabled = parseEnabledFlag(get_reg_val("redisJobStatusEnabled"));
      config.serverType = get_reg_val("redisJobStatusServerType");
      config.serverId = get_reg_val("redisJobStatusServerId");
      return config;
   }
} // namespace

RedisJobStatus::RedisJobStatus(ConfigProvider provider)
{
  const Config config = provider ? provider() : defaultConfig();
  m_enabled = config.enabled;
  m_serverType = config.serverType;
  m_serverId = config.serverId;
  if (m_enabled)
  {
    m_client = RedisClient::Instance();
  }
}

std::shared_ptr<RedisJobStatus> RedisJobStatus::Instance(ConfigProvider provider)
{
  static const std::shared_ptr<RedisJobStatus> instance(
      new RedisJobStatus(std::move(provider)));
  return instance;
}

void RedisJobStatus::validateLifecycleIdentity(const string& jobId) const
{
  validateIdentifier(m_serverType, "serverType", true);
  validateIdentifier(m_serverId, "serverId");
  validateIdentifier(jobId, "jobId");
}

RedisJobStatus::Result RedisJobStatus::resultFromScript(std::int64_t value)
{
  switch (value)
  {
    case 0: return Result::Success;
    case 1: return Result::NoOp;
    case 2: return Result::JobNotFound;
    case 3: return Result::InvalidTransition;
    case 4: return Result::InconsistentData;
    case 5: return Result::JobAlreadyExists;
    case 6: return Result::Disabled;
    case 7: return Result::ServerNotActive;
    default:
      throw runtime_error("RedisJobStatus: unknown lifecycle script result " +
                          to_string(value));
  }
}

RedisJobStatus::RegistrationResult
RedisJobStatus::registrationResultFromScript(std::int64_t value)
{
  switch (value)
  {
    case 0: return RegistrationResult::AlreadyPresent;
    case 1: return RegistrationResult::Added;
    default:
      throw runtime_error("RedisJobStatus: unknown registry script result " +
                          to_string(value));
  }
}

RedisJobStatus::ActivationResult
RedisJobStatus::activationResultFromScript(std::int64_t value)
{
  switch (value)
  {
    case 0: return ActivationResult::AlreadyHasLifecycleState;
    case 1: return ActivationResult::Activated;
    default:
      throw runtime_error("RedisJobStatus: unknown activation script result " +
                          to_string(value));
  }
}

RedisJobStatus::HeartbeatResult
RedisJobStatus::heartbeatResultFromScript(std::int64_t value)
{
  switch (value)
  {
    case 0: return HeartbeatResult::Rejected;
    case 1: return HeartbeatResult::Refreshed;
    default:
      throw runtime_error("RedisJobStatus: unknown heartbeat script result " +
                          to_string(value));
  }
}

RedisJobStatus::Result RedisJobStatus::createPendingJob(
    const string& jobId,
    const map<string, string>& metadata)
{
  if (!m_enabled)
  {
    return Result::Disabled;
  }
  validateLifecycleIdentity(jobId);
  for (const auto& item : metadata)
  {
    if (item.first.empty())
    {
      throw invalid_argument("RedisJobStatus: job metadata field must not be empty");
    }
  }

  const string reference = jobReference(m_serverId, jobId);
  const KeyList keys = lifecycleKeys(jobId, m_serverType, m_serverId);
  ArgList args = {jobId, m_serverType, m_serverId, reference};
  appendMetadataArgs(args, metadata, [](const string& field)
  {
    return !isReservedJobMetadataField(field);
  });
  const auto result = m_client->execute([&](auto& redis)
  {
    return evalCounterResult(redis, kCreatePendingScript, keys, args);
  });
  return resultFromScript(result);
}

RedisJobStatus::Result RedisJobStatus::transitionJob(
    const string& jobId,
    State newState)
{
  if (!m_enabled)
  {
    return Result::Disabled;
  }
  validateLifecycleIdentity(jobId);
  if (newState == State::Pending)
  {
    return Result::InvalidTransition;
  }
  const string reference = jobReference(m_serverId, jobId);
  const string newStateName = stateName(newState);
  const KeyList keys = lifecycleKeys(jobId, m_serverType, m_serverId);
  ArgList args = {jobId, m_serverType, m_serverId, newStateName, reference};
  const auto result = m_client->execute([&](auto& redis)
  {
    return evalCounterResult(redis, kTransitionScript, keys, args);
  });
  return resultFromScript(result);
}

RedisJobStatus::Result RedisJobStatus::completeJob(const string& jobId)
{
  if (!m_enabled)
  {
    return Result::Disabled;
  }
  validateLifecycleIdentity(jobId);
  const string reference = jobReference(m_serverId, jobId);
  const KeyList keys = lifecycleKeys(jobId, m_serverType, m_serverId);
  ArgList args = {jobId, m_serverType, m_serverId, reference};
  const auto result = m_client->execute([&](auto& redis)
  {
    return evalCounterResult(redis, kCompleteScript, keys, args);
  });
  return resultFromScript(result);
}

void RedisJobStatus::registerServer(
    const map<string, string>& serverTypeMetadata,
    const map<string, string>& serverMetadata)
{
  if (!m_enabled)
  {
    return;
  }

  validateIdentifier(m_serverType, "serverType", true);
  validateIdentifier(m_serverId, "serverId");
  validateRegistrationMetadata(serverTypeMetadata, serverMetadata);
  registerServerInRegistry();
  activateRegisteredServer(serverTypeMetadata, serverMetadata);
}

void RedisJobStatus::validateRegistrationMetadata(
    const map<string, string>& serverTypeMetadata,
    const map<string, string>& serverMetadata) const
{
  for (const auto& item : serverTypeMetadata)
  {
    if (item.first.empty() || isReservedServerTypeMetadataField(item.first))
    {
      throw invalid_argument(
          "RedisJobStatus: empty or reserved server-type metadata field");
    }
  }

  for (const auto& item : serverMetadata)
  {
    if (item.first.empty() || isReservedServerMetadataField(item.first))
    {
      throw invalid_argument(
          "RedisJobStatus: empty or reserved server metadata field");
    }
  }
}

void RedisJobStatus::registerServerInRegistry() const
{
  // The registry and per-type state use different Cluster slots. Check the
  // authoritative per-type state before inserting into the registry slot.
  const auto rawResult = m_client->execute([&](auto& redis)
  {
    const auto existingState =
        redis.hget(serverStateKey(m_serverType), m_serverId);

    if (existingState)
    {
      return int64_t{0};
    }

    const KeyList keys = {kServerTypesKey, serverRegistryKey(m_serverType)};
    const ArgList args = {m_serverType, m_serverId};
    return evalResult(redis, kRegisterServerScript, keys, args);
  });

  if (registrationResultFromScript(rawResult) ==
      RegistrationResult::AlreadyPresent)
  {
    throw runtime_error(
        "RedisJobStatus: serverId is already registered or still has a retirement tombstone");
  }
}

void RedisJobStatus::activateRegisteredServer(
    const map<string, string>& serverTypeMetadata,
    const map<string, string>& serverMetadata) const
{
  // Metadata and ACTIVE publication share the per-type Cluster slot.
  const auto now = std::time(nullptr);
  const std::tm* utc = std::gmtime(&now);
  if (!utc)
  {
    throw runtime_error("RedisJobStatus: unable to generate metadata timestamp");
  }

  std::ostringstream timestamp;
  timestamp << std::put_time(utc, "%Y-%m-%dT%H:%M:%SZ");

  const KeyList keys = {
      typeSummaryKey(m_serverType),
      serverSummaryKey(m_serverType, m_serverId),
      serverStateKey(m_serverType)};
  ArgList args = {m_serverId, timestamp.str()};
  appendMetadataArgs(args, serverTypeMetadata, [](const string&) { return true; });
  appendMetadataArgs(args, serverMetadata, [](const string&) { return true; });

  const auto rawResult = m_client->execute([&](auto& redis)
  {
    return evalResult(redis, kRegisterAndActivateServerScript, keys, args);
  });

  if (activationResultFromScript(rawResult) != ActivationResult::Activated)
  {
    throw runtime_error(
        "RedisJobStatus: serverId already has a lifecycle state and cannot be registered");
  }
}

vector<string> RedisJobStatus::getServerTypes()
{
  if (!m_enabled)
  {
    return {};
  }
  return m_client->execute(
      [&](auto& redis)
      {
        vector<string> serverTypes;
        redis.smembers(kServerTypesKey, back_inserter(serverTypes));
        return serverTypes;
      });
}

void RedisJobStatus::refreshHeartbeat(int64_t ttlSeconds)
{
  if (!m_enabled)
  {
    return;
  }
  validateIdentifier(m_serverType, "serverType", true);
  validateIdentifier(m_serverId, "serverId");
  if (ttlSeconds <= 0)
  {
    throw invalid_argument(
        "RedisJobStatus: heartbeat TTL must be positive");
  }

  const KeyList keys = {
      heartbeatKey(m_serverType, m_serverId),
      liveServersKey(m_serverType),
      serverStateKey(m_serverType)};
  const ArgList args = {m_serverId, to_string(ttlSeconds)};
  const auto refreshed = m_client->execute([&](auto& redis)
  {
    return evalResult(redis, kRefreshHeartbeatScript, keys, args);
  });
  const auto heartbeatOutcome = heartbeatResultFromScript(refreshed);
  if (heartbeatOutcome != HeartbeatResult::Refreshed)
  {
    throw runtime_error("RedisJobStatus: heartbeat rejected because server is not ACTIVE");
  }
}

map<string, int64_t> RedisJobStatus::getServerTypeSummary(
    const string& serverType)
{
  if (!m_enabled)
  {
    return {};
  }
  validateIdentifier(serverType, "serverType", true);
  const auto key = typeSummaryKey(serverType);
  return m_client->execute(
      [&](auto& redis)
      {
        return readCounters(redis, key);
      });
}

vector<RedisJobStatus::ServerSummary>
RedisJobStatus::getServerSummaries(const string& serverType)
{
  if (!m_enabled)
  {
    return {};
  }
  validateIdentifier(serverType, "serverType", true);

  return m_client->execute([&](auto& redis)
  {
    vector<string> ids;
    redis.smembers(serverRegistryKey(serverType), back_inserter(ids));

    // Keep a same-slot routing key even when the registry contains no servers.
    KeyList keys = {typeSummaryKey(serverType)};
    for (const auto& id : ids)
    {
      keys.push_back(serverSummaryKey(serverType, id));
    }

    const auto flat =
        evalStrings(redis, kReadServerSummariesSnapshotScript, keys, ids);
    return parseServerSummariesSnapshot(flat);
  });
}

namespace
{
   /**
      * @brief Parses a flat Lua job snapshot and verifies every job belongs to the requested server scope.
      * @param flat Flat sequence of job references, field counts, and field/value pairs.
      * @param expectedServerType Required server type for every returned job.
      * @param expectedServerId Optional required server ID; null accepts any server ID of the type.
      * @return Parsed jobs with lifecycle fields separated from user metadata.
      */
   vector<RedisJobStatus::Job> parseJobsSnapshot(
         const vector<string>& flat,
         const string& expectedServerType,
         const string* expectedServerId)
   {
      if (flat.empty())
      {
         throw runtime_error("RedisJobStatus: empty job snapshot");
      }
      std::size_t cursor = 0;
      const auto count = parseSnapshotSize(flat[cursor++], "job record count");
      if (count > (flat.size() - cursor) / 2)
      {
         throw runtime_error("RedisJobStatus: malformed job record count");
      }
      vector<RedisJobStatus::Job> jobs;
      jobs.reserve(count);
      for (std::size_t i = 0; i < count; ++i)
      {
         if (cursor + 2 > flat.size())
         {
            throw runtime_error("RedisJobStatus: malformed job record");
         }
         const string reference = flat[cursor++];
         const auto fields = parseSnapshotSize(flat[cursor++], "job field count");
         if (cursor > flat.size() || fields > (flat.size() - cursor) / 2)
         {
            throw runtime_error("RedisJobStatus: truncated job record");
         }
         RedisJobStatus::Job job;
         for (std::size_t j = 0; j < fields; ++j)
         {
            const auto& name = flat[cursor++];
            const auto& value = flat[cursor++];
            if (name == kJobIdField)
            {
               job.jobId = value;
            }
            else if (name == kStatusField)
            {
               job.status = value;
            }
            else if (name == kServerTypeField)
            {
               job.serverType = value;
            }
            else if (name == kServerIdField)
            {
               job.serverId = value;
            }
            else
            {
               job.fields[name] = value;
            }
         }
         if (job.serverType != expectedServerType ||
               (expectedServerId && job.serverId != *expectedServerId))
         {
            throw runtime_error("RedisJobStatus: foreign job reference in index: " + reference);
         }
         jobs.push_back(std::move(job));
      }
      if (cursor != flat.size())
      {
         throw runtime_error("RedisJobStatus: trailing job snapshot data");
      }
      return jobs;
   }
} // namespace

vector<RedisJobStatus::Job> RedisJobStatus::getServerJobs(
    const string& serverType, const string& serverId)
{
  if (!m_enabled)
  {
    return {};
  }
  validateIdentifier(serverType, "serverType", true);
  validateIdentifier(serverId, "serverId");
  const string indexKey = serverJobsKey(serverType, serverId);
  return m_client->execute([&](auto& redis)
  {
    vector<string> refs;
    redis.smembers(indexKey, back_inserter(refs));
    KeyList keys = {indexKey};
    for (const auto& ref : refs)
      keys.push_back("job:{" + serverType + "}:" + ref);
    ArgList args = {to_string(refs.size())};
    args.insert(args.end(), refs.begin(), refs.end());
    const auto flat = evalStrings(redis, kReadJobsSnapshotScript, keys, args);
    return parseJobsSnapshot(flat, serverType, &serverId);
  });
}

vector<RedisJobStatus::Job> RedisJobStatus::getServerTypeJobs(
    const string& serverType)
{
  if (!m_enabled)
  {
    return {};
  }
  validateIdentifier(serverType, "serverType", true);
  const string indexKey = typeJobsKey(serverType);
  return m_client->execute([&](auto& redis)
  {
    vector<string> refs;
    redis.smembers(indexKey, back_inserter(refs));
    KeyList keys = {indexKey};
    for (const auto& ref : refs)
      keys.push_back("job:{" + serverType + "}:" + ref);
    ArgList args = {to_string(refs.size())};
    args.insert(args.end(), refs.begin(), refs.end());
    const auto flat = evalStrings(redis, kReadJobsSnapshotScript, keys, args);
    return parseJobsSnapshot(flat, serverType, nullptr);
  });
}

string RedisJobStatus::stateName(State state)
{
  switch (state)
  {
    case State::Pending:
      return "PENDING";
    case State::Active:
      return "ACTIVE";
    case State::Suspended:
      return "SUSPENDED";
  }
  throw invalid_argument("RedisJobStatus: unknown state");
}

void RedisJobStatus::validateIdentifier(
    const string& value,
    const char* name,
    bool isServerType)
{
  if (value.empty())
  {
    throw invalid_argument(
        string("RedisJobStatus: ") + name + " must not be empty");
  }
  if (isServerType &&
      (value.find('{') != string::npos ||
       value.find('}') != string::npos))
  {
    throw invalid_argument(
        "RedisJobStatus: serverType must not contain '{' or '}'");
  }
}
