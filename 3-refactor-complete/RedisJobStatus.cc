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


   /**
   * @brief Builds a collision-safe reference from a server ID and job ID.
   /**
   * @brief Provides the jobReference helper operation.
   * @param serverId Parameter used by this helper.
   * @param jobId Parameter used by this helper.
   * @return Result produced by this helper.
   */
   * @tparam IncludeField Predicate deciding whether each/**
   * @brief /**
   * @brief Provides the to_string helper operation.
   * @param serverType Parameter used by this helper.
   * @param serverId Parameter used by this helper.
   * @param jobId Parameter used by this helper.
   * @return Result produced by this helper.
   */
   Builds a collision-safe reference from a server ID and job ID.
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   */**
   * @brief Provides the typeJobsKey helper operation.
   * @param serverType Parameter used by this helper.
   * @return Result produced by this helper.
   */
   @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @t/**
   * @brief Provides the serverJobsKey helper operation.
   * @param serverType Parameter used by this helper.
   * @param serverId Parameter used by this helper.
   * @return Result produced by this helper.
   */
   param Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type pr/**
   * @brief Provides the typeSummaryKey helper operation.
   * @param serverType Parameter used by this helper.
   * @return Result produced by this helper.
   */
   oviding the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeField Predicate deciding wheth/**
   * @brief Provides the serverSummaryKey helper operation.
   * @param serverType Parameter used by this helper.
   * @param serverId Parameter used by this helper.
   * @return Result produced by this helper.
   */
   er each metadata field is includ/
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis c/**
   * @brief Provides the serverRegistryKey helper operation.
   * @param serverType Parameter used by this helper.
   * @return Result produced by this helper.
   */
   lient or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tp/**
   * @brief Provides the heartbeatKey helper operation.
   * @param serverType Parameter used by this helper.
   * @param serverId Parameter used by this helper.
   * @return Result produced by this helper.
   */
   aram IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type provi/**
   * @brief Provides the liveServersKey helper operation.
   * @param serverType Parameter used by this helper.
   * @return Result produced by this helper.
   */
   ding the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the hg/**
   * @brief Provides the serverStateKey helper operation.
   * @param serverType Parameter used by this helper.
   * @return Result produced by this helper.
   */
   etall interface.
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the/**
   * @brief Provides the lifecycleKeys helper operation.
   * @param jobId Parameter used by this helper.
   * @param serverType Parameter used by this helper.
   * @param serverId Parameter used by this helper.
   * @return Result produced by this helper.
   */
   eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeFie/**
   * @brief Provides the isReservedJobMetadataField helper operation.
   * @param field Parameter used by this helper.
   * @return Result produced by this helper.
   */
   ld Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing /**
   * @brief Provides the isReservedServerTypeMetadataField helper operation.
   * @param field Parameter used by this helper.
   * @return Result produced by this helper.
   */
   the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type pro/**
   * @brief Provides the isReservedServerMetadataField helper operation.
   * @param field Parameter used by this helper.
   * @return Result produced by this helper.
   */
   viding the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeField Predicate deciding whether e/
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval inter/**
   * @brief Provides the appendMetadataArgs helper operation.
   * @param args Parameter used by this helper.
   * @param metadata Parameter used by this helper.
   * @param includeField Parameter used by this helper.
   */
   face.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing t/**
   * @brief Provides the evalResult helper operation.
   * @param redis Parameter used by this helper.
   * @param script Parameter used by this helper.
   * @param keys Parameter used by this helper.
   * @param args Parameter used by this helper.
   * @return Result produced by this helper.
   */
   he eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @tparam IncludeField Predicate deciding whether each metadata field is included.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the eval interface.
   * @tparam Redis Redis client or connection type providing the hgetall interface.
   * @param redis Parameter used by this helper.
   * @param validatedScript Parameter used by this helper.
   * @param keys Parameter used by this helper.
   * @param /**
   * @brief Provides the evalCounterResult helper operation.
   * @param redis Parameter used by this helper.
   * @param script Parameter used by this helper.
   * @param keys Parameter used by this helper.
   * @param args Parameter used by this helper.
   * @return Result produced by this helper.
   */
   redis Parameter used by this helper.
   * @param script Parameter used by this helper.
   * @param keys Parameter used by this helper.
   * @param args Parameter used by this helper.
   * @return Result produced by this helper.
   */
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
   * @param flat Parameter used by this helper.
   * @param expectedServerType Parameter used by this helper.
   * @param expectedServerId Parameter used by this helper.
   * @return Result produced by this helper.
   */
   vector<RedisJobStatus::Job> parseJobsSnapshot(
         const vector<string>& flat,
         const string& expectedServerType,
         const string* expectedServerId)
   {
      if (flat.empty())
      {
         /**
   * @brief Provides the runtime_error helper operation.
   * @param count Parameter used by this helper.
   * @return Result produced by this helper.
   */
   throw runtime_error("RedisJobStatus: empty job snapshot");
      }
      std::size_t cursor = 0;
      const auto count = parseSnapshotSize(flat[cursor++], "job record count");
      if (count > (flat.size() - cursor) / 2)
      {
         /**
   * @brief Provides the runtime_error helper operation.
   * @param i Parameter used by this helper.
   * @return Result produced by this helper.
   */
   throw runtime_error("RedisJobStatus: malformed job record count");
      }
      vector<RedisJobStatus::Job> jobs;
      jobs.reserve(count);
      for (std::size_t i = 0; i < count; ++i)
      {
         if (cursor + 2 > flat.size())
         {
            /**
   * @brief Provides the runtime_error helper operation.
   * @param fields Parameter used by this helper.
   * @return Result produced by this helper.
   */
   throw runtime_error("RedisJobStatus: malformed job record");
         }
         const string reference = flat[cursor++];
         const auto fields = parseSnapshotSize(flat[cursor++], "job field count");
         if (cursor > flat.size() || fields > (flat.size() - cursor) / 2)
         {
            /**
   * @brief Provides the runtime_error helper operation.
   * @param j Parameter used by this helper.
   * @return Result produced by this helper.
   */
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
            /**
   * @brief Provides the runtime_error helper operation.
   * @return Result produced by this helper.
   */
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
