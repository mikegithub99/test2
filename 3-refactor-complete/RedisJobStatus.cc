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


string jobReference(const string& serverId, const string& jobId)
{
  return to_string(serverId.size()) + ":" + serverId + ":" + jobId;
}

string jobKey(const string& serverType, const string& serverId, const string& jobId)
{
  return "job:{" + serverType + "}:" + jobReference(serverId, jobId);
}

string typeJobsKey(const string& serverType)
{
  return "jobs:type:{" + serverType + "}";
}

string serverJobsKey(
    const string& serverType,
    const string& serverId)
{
  return "jobs:server:{" + serverType + "}:" + serverId;
}

string typeSummaryKey(const string& serverType)
{
  return "summary:type:{" + serverType + "}";
}

string serverSummaryKey(
    const string& serverType,
    const string& serverId)
{
  return "summary:server:{" + serverType + "}:" + serverId;
}

string serverRegistryKey(const string& serverType)
{
  return "servers:{registry}:type:" + serverType;
}

const string kServerTypesKey = "servers:{registry}:types";

string heartbeatKey(
    const string& serverType,
    const string& serverId)
{
  return "heartbeat:server:{" + serverType + "}:" + serverId;
}

string liveServersKey(const string& serverType)
{
  return "servers:live:{" + serverType + "}";
}

string serverStateKey(const string& serverType)
{
  return "servers:state:{" + serverType + "}";
}

using KeyList = vector<string>;
using ArgList = vector<string>;

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

bool isReservedServerTypeMetadataField(const string& field)
{
  return field == kCreatedAtMetadataField ||
         field == kCreatedByServerIdMetadataField;
}

bool isReservedServerMetadataField(const string& field)
{
  return field == "registeredAt";
}

// Appends a count and accepted field/value pairs in the Lua argument format.
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


template <typename Redis>
vector<string> evalStrings(Redis& redis, const string& script,
                           const KeyList& keys, const ArgList& args)
{
  return redis.template eval<vector<string>>(
      script, keys.begin(), keys.end(), args.begin(), args.end());
}

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
