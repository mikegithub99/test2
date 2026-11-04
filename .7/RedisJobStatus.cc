/**
 * @file RedisJobStatus.cc
 * @brief Implementation of logical Redis job-status APIs.
 */

#include "RedisJobStatus.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

using std::back_inserter;
using std::chrono::seconds;
using std::tolower;
using std::transform;
using std::exception;
using std::int64_t;
using std::invalid_argument;
using std::map;
using std::pair;
using std::runtime_error;
using std::stoll;
using std::string;
using std::to_string;
using std::vector;

// The host application provides this registry lookup.
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

using KeyList = vector<string>;
using ArgList = vector<string>;

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
map<string, int64_t> readCounters(
    Redis& redis,
    const string& key)
{
  vector<pair<string, string>> fields;
  redis.hgetall(key, back_inserter(fields));

  map<string, int64_t> counters;
  for (const auto& field : fields)
  {
    try
    {
      counters[field.first] = stoll(field.second);
    }
    catch (const exception&)
    {
      throw runtime_error(
          "RedisJobStatus: non-integer counter '" + field.first + "'");
    }
  }
  return counters;
}

template <typename Redis>
RedisJobStatus::Job readJob(Redis& redis, const string& key)
{
  vector<pair<string, string>> fields;
  redis.hgetall(key, back_inserter(fields));

  RedisJobStatus::Job job;
  for (const auto& field : fields)
  {
    if (field.first == "jobId")
    {
      job.jobId = field.second;
    }
    else if (field.first == "status")
    {
      job.status = field.second;
    }
    else if (field.first == "serverType")
    {
      job.serverType = field.second;
    }
    else if (field.first == "serverId")
    {
      job.serverId = field.second;
    }
    else
    {
      job.fields[field.first] = field.second;
    }
  }
  return job;
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

RedisJobStatus::Result RedisJobStatus::createPendingJob(
    const string& jobId,
    const map<string, string>& fields,
    const map<string, string>& serverTypeMetadata,
    const map<string, string>& serverMetadata)
{
  if (!m_enabled)
  {
    return Result::Disabled;
  }
  validateIdentifier(m_serverType, "serverType", true);
  validateIdentifier(m_serverId, "serverId");
  validateIdentifier(jobId, "jobId");
  for (const auto& item : serverTypeMetadata)
  {
    if (item.first.empty() || item.first == "createdByServerId" ||
        item.first == "createdAt")
      throw invalid_argument("RedisJobStatus: empty or reserved server-type metadata field");
  }
  for (const auto& item : serverMetadata)
  {
    if (item.first.empty())
      throw invalid_argument("RedisJobStatus: server metadata field must not be empty");
  }
  const auto now = std::time(nullptr);
  const std::tm* utc = std::gmtime(&now);
  if (!utc) throw runtime_error("RedisJobStatus: unable to generate metadata timestamp");
  std::ostringstream timestamp;
  timestamp << std::put_time(utc, "%Y-%m-%dT%H:%M:%SZ");

  const string reference = jobReference(m_serverId, jobId);
  KeyList keys = {
      jobKey(m_serverType, m_serverId, jobId), typeJobsKey(m_serverType),
      serverJobsKey(m_serverType, m_serverId), typeSummaryKey(m_serverType),
      serverSummaryKey(m_serverType, m_serverId)};
  ArgList args = {jobId, m_serverType, m_serverId, reference, "0",
                   std::to_string(serverTypeMetadata.size()),
                   std::to_string(serverMetadata.size()), timestamp.str()};
  int jobFieldCount = 0;
  for (const auto& field : fields)
  {
    if (field.first == "jobId" || field.first == "status" ||
        field.first == "serverType" || field.first == "serverId") continue;
    args.push_back(field.first);
    args.push_back(field.second);
    ++jobFieldCount;
  }
  args[4] = std::to_string(jobFieldCount);
  for (const auto& field : serverTypeMetadata)
  {
    args.push_back(field.first);
    args.push_back(field.second);
  }
  for (const auto& field : serverMetadata)
  {
    args.push_back(field.first);
    args.push_back(field.second);
  }
  const auto result = m_client->execute([&](auto& redis)
  {
    return evalResult(redis, kCreatePendingScript, keys, args);
  });
  return static_cast<Result>(result);
}

RedisJobStatus::Result RedisJobStatus::transitionJob(
    const string& jobId,
    State newState)
{
  if (!m_enabled) return Result::Disabled;
  validateIdentifier(m_serverType, "serverType", true);
  validateIdentifier(m_serverId, "serverId");
  validateIdentifier(jobId, "jobId");
  if (newState == State::Pending) return Result::InvalidTransition;
  const string reference = jobReference(m_serverId, jobId);
  const string newStateName = stateName(newState);
  KeyList keys = {
      jobKey(m_serverType, m_serverId, jobId), typeJobsKey(m_serverType),
      serverJobsKey(m_serverType, m_serverId), typeSummaryKey(m_serverType),
      serverSummaryKey(m_serverType, m_serverId)};
  ArgList args = {jobId, m_serverType, m_serverId, newStateName, reference};
  const auto result = m_client->execute([&](auto& redis)
  {
    return evalResult(redis, kTransitionScript, keys, args);
  });
  return static_cast<Result>(result);
}

RedisJobStatus::Result RedisJobStatus::completeJob(const string& jobId)
{
  if (!m_enabled) return Result::Disabled;
  validateIdentifier(m_serverType, "serverType", true);
  validateIdentifier(m_serverId, "serverId");
  validateIdentifier(jobId, "jobId");
  const string reference = jobReference(m_serverId, jobId);
  KeyList keys = {
      jobKey(m_serverType, m_serverId, jobId), typeJobsKey(m_serverType),
      serverJobsKey(m_serverType, m_serverId), typeSummaryKey(m_serverType),
      serverSummaryKey(m_serverType, m_serverId)};
  ArgList args = {jobId, m_serverType, m_serverId, reference};
  const auto result = m_client->execute([&](auto& redis)
  {
    return evalResult(redis, kCompleteScript, keys, args);
  });
  return static_cast<Result>(result);
}

void RedisJobStatus::registerServer()
{
  if (!m_enabled) return;
  validateIdentifier(m_serverType, "serverType", true);
  validateIdentifier(m_serverId, "serverId");
  const KeyList keys = {kServerTypesKey, serverRegistryKey(m_serverType)};
  const ArgList args = {m_serverType, m_serverId};

  // Both registry keys share the {registry} hash tag, so one script can
  // update them atomically in standalone Redis and Redis Cluster.
  m_client->execute(
      [&](auto& redis)
      {
        return evalResult(redis, kRegisterServerScript, keys, args);
      });
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
  if (!m_enabled) return;
  validateIdentifier(m_serverType, "serverType", true);
  validateIdentifier(m_serverId, "serverId");
  if (ttlSeconds <= 0)
  {
    throw invalid_argument(
        "RedisJobStatus: heartbeat TTL must be positive");
  }

  const auto key = heartbeatKey(m_serverType, m_serverId);
  m_client->execute(
      [&](auto& redis)
      {
        redis.set(
            key,
            m_serverId,
            seconds(ttlSeconds));
      });
}

map<string, int64_t> RedisJobStatus::getTypeSummary(
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
  const auto registryKey = serverRegistryKey(serverType);

  return m_client->execute(
      [&](auto& redis)
      {
        vector<string> serverIds;
        redis.smembers(registryKey, back_inserter(serverIds));

        vector<ServerSummary> summaries;
        summaries.reserve(serverIds.size());
        for (const auto& serverId : serverIds)
        {
          summaries.push_back(
              ServerSummary{serverId,
                            readCounters(
                                redis,
                                serverSummaryKey(serverType, serverId))});
        }
        return summaries;
      });
}

vector<RedisJobStatus::Job> RedisJobStatus::getServerJobs(
    const string& serverType,
    const string& serverId)
{
  if (!m_enabled)
  {
    return {};
  }
  validateIdentifier(serverType, "serverType", true);
  validateIdentifier(serverId, "serverId");
  const auto indexKey = serverJobsKey(serverType, serverId);

  return m_client->execute(
      [&](auto& redis)
      {
        vector<string> jobIds;
        redis.smembers(indexKey, back_inserter(jobIds));

        vector<Job> jobs;
        jobs.reserve(jobIds.size());
        for (const auto& reference : jobIds)
        {
          jobs.push_back(readJob(redis, "job:{" + serverType + "}:" + reference));
        }
        return jobs;
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
