/**
 * @file HeartbeatManager.cc
 * @brief Cluster-safe, two-phase cleanup for heartbeat-expired server IDs.
 */

#include "HeartbeatManager.h"

#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using std::back_inserter;
using std::int64_t;
using std::runtime_error;
using std::string;
using std::vector;

namespace
{
const char* const kFenceScript = R"lua(
local heartbeat = KEYS[1]
local state = KEYS[2]
local serverJobs = KEYS[3]
local serverSummary = KEYS[4]
local live = KEYS[5]
local ht = redis.call('TYPE', heartbeat).ok
local st = redis.call('TYPE', state).ok
local jt = redis.call('TYPE', serverJobs).ok
local mt = redis.call('TYPE', serverSummary).ok
local lt = redis.call('TYPE', live).ok
if ht ~= 'none' and ht ~= 'string' then
  return redis.error_reply('wrong heartbeat key type')
end
if lt ~= 'none' and lt ~= 'set' then
  return redis.error_reply('wrong live-set key type')
end
if st == 'none' then
  if ht == 'none' and jt == 'none' and mt == 'none' and
     not redis.call('SISMEMBER', live, ARGV[1]) then return 2 end
  return redis.error_reply('missing lifecycle state with remaining server artifacts')
end
if st ~= 'hash' or (jt ~= 'none' and jt ~= 'set') or
   (mt ~= 'none' and mt ~= 'hash') then
  return redis.error_reply('wrong lifecycle/artifact key type')
end
if redis.call('EXISTS', heartbeat) ~= 0 then return 0 end
local current = redis.call('HGET', state, ARGV[1])
if current == 'DELETING' then
  if jt == 'none' and mt == 'none' and
     not redis.call('SISMEMBER', live, ARGV[1]) then return 2 end
  return 1
end
if current ~= 'ACTIVE' then return 0 end
-- A freshly registered process is not a cleanup candidate until its first
-- successful heartbeat has published live-set membership.
if redis.call('SISMEMBER', live, ARGV[1]) == 0 then return 0 end
redis.call('HSET', state, ARGV[1], 'DELETING')
return 1
)lua";

const char* const kCleanupScript = R"lua(
local live = KEYS[1]
local state = KEYS[2]
local heartbeat = KEYS[3]
local serverJobs = KEYS[4]
local typeJobs = KEYS[5]
local serverSummary = KEYS[6]
local typeSummary = KEYS[7]
local serverId = ARGV[1]
local count = tonumber(ARGV[2])

local function keytype(key)
  return redis.call('TYPE', key).ok
end
local function valid(key, expected)
  local t = keytype(key)
  return t == 'none' or t == expected
end

if not valid(live, 'set') or keytype(state) ~= 'hash' or
   not valid(heartbeat, 'string') or not valid(serverJobs, 'set') or
   not valid(typeJobs, 'set') or not valid(serverSummary, 'hash') or
   not valid(typeSummary, 'hash') then
  return redis.error_reply('inconsistent cleanup key type')
end
if redis.call('HGET', state, serverId) ~= 'DELETING' or
   redis.call('EXISTS', heartbeat) ~= 0 then
  return 0
end

local pending, active, suspended, total = 0, 0, 0, 0
for i = 1, count do
  local jobKey = KEYS[7 + i]
  if keytype(jobKey) ~= 'hash' then
    return redis.error_reply('missing or wrong-type job hash')
  end
  local jobType = redis.call('HGET', jobKey, 'serverType')
  local owner = redis.call('HGET', jobKey, 'serverId')
  local status = redis.call('HGET', jobKey, 'status')
  local ref = ARGV[2 + i]
  if owner ~= serverId or jobType ~= ARGV[3 + count] then
    return redis.error_reply('job ownership mismatch')
  end
  if redis.call('SISMEMBER', serverJobs, ref) ~= 1 or
     redis.call('SISMEMBER', typeJobs, ref) ~= 1 then
    return redis.error_reply('job index membership missing')
  end
  if status == 'PENDING' then pending = pending + 1
  elseif status == 'ACTIVE' then active = active + 1
  elseif status == 'SUSPENDED' then suspended = suspended + 1
  else return redis.error_reply('unsupported job status') end
  total = total + 1
end

local function enough(field, decrement)
  local raw = redis.call('HGET', typeSummary, field) or '0'
  local value = tonumber(raw)
  return value and value >= 0 and value == math.floor(value) and value >= decrement
end
if not enough('pending', pending) or not enough('active', active) or
   not enough('suspended', suspended) or not enough('total', total) then
  return redis.error_reply('invalid or insufficient type counters')
end

for i = 1, count do
  redis.call('SREM', typeJobs, ARGV[2 + i])
  redis.call('DEL', KEYS[7 + i])
end
redis.call('DEL', serverJobs)
if pending > 0 then redis.call('HINCRBY', typeSummary, 'pending', -pending) end
if active > 0 then redis.call('HINCRBY', typeSummary, 'active', -active) end
if suspended > 0 then redis.call('HINCRBY', typeSummary, 'suspended', -suspended) end
if total > 0 then redis.call('HINCRBY', typeSummary, 'total', -total) end
redis.call('DEL', serverSummary, heartbeat)
redis.call('SREM', live, serverId)
-- Keep the terminal DELETING marker: this process ID is never reused.
-- It prevents a delayed/repeated registerServer() from reactivating the ID.
return 1
)lua";

string registryTypesKey() { return "servers:{registry}:types"; }
string registryServersKey(const string& type)
{ return "servers:{registry}:type:" + type; }
string liveKey(const string& type) { return "servers:live:{" + type + "}"; }
string stateKey(const string& type) { return "servers:state:{" + type + "}"; }
string heartbeatKey(const string& type, const string& id)
{ return "heartbeat:server:{" + type + "}:" + id; }
string serverJobsKey(const string& type, const string& id)
{ return "jobs:server:{" + type + "}:" + id; }
string typeJobsKey(const string& type) { return "jobs:type:{" + type + "}"; }
string serverSummaryKey(const string& type, const string& id)
{ return "summary:server:{" + type + "}:" + id; }
string typeSummaryKey(const string& type) { return "summary:type:{" + type + "}"; }
} // namespace

HeartbeatManager::HeartbeatManager(std::chrono::seconds scanInterval)
    : m_client(RedisClient::Instance()), m_scanInterval(scanInterval)
{
  if (m_scanInterval.count() <= 0)
    throw std::invalid_argument("HeartbeatManager: scan interval must be positive");
  m_thread = std::thread(&HeartbeatManager::run, this);
}

HeartbeatManager::~HeartbeatManager()
{
  stop();
}

void HeartbeatManager::stop()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopping = true;
  }
  m_wakeup.notify_all();
  if (m_thread.joinable()) m_thread.join();
}

void HeartbeatManager::run()
{
  std::unique_lock<std::mutex> lock(m_mutex);
  while (!m_stopping)
  {
    lock.unlock();
    try { scanOnce(); }
    catch (const std::exception& error)
    { std::cerr << "Heartbeat cleanup scan failed: " << error.what() << '\n'; }
    catch (...)
    { std::cerr << "Heartbeat cleanup scan failed: unknown exception\n"; }
    lock.lock();
    m_wakeup.wait_for(lock, m_scanInterval, [this] { return m_stopping; });
  }
}

void HeartbeatManager::scanOnce()
{
  m_client->execute([&](auto& redis)
  {
    vector<string> types;
    redis.smembers(registryTypesKey(), back_inserter(types));

    for (const auto& type : types)
    {
      vector<string> ids;
      redis.smembers(registryServersKey(type), back_inserter(ids));
      for (const auto& id : ids)
      {
        const string heartbeat = heartbeatKey(type, id);
        const string state = stateKey(type);
        if (redis.exists(heartbeat)) continue;

        const vector<string> fenceKeys = {
            heartbeat, state, serverJobsKey(type, id), serverSummaryKey(type, id), liveKey(type)};
        const vector<string> fenceArgs = {id};
        const auto fenced = redis.template eval<int64_t>(
            kFenceScript, fenceKeys.begin(), fenceKeys.end(),
            fenceArgs.begin(), fenceArgs.end());

        if (fenced == 0) continue;
        if (fenced == 2)
        {
          // A missing state is a completed Phase A from an earlier pass.
          // It is safe to finish Phase B because IDs are never reused.
          redis.srem(registryServersKey(type), id);
          continue;
        }
        if (fenced != 1) continue;

        const string serverJobs = serverJobsKey(type, id);
        vector<string> refs;
        redis.smembers(serverJobs, back_inserter(refs));

        vector<string> keys = {
            liveKey(type), state, heartbeat, serverJobs, typeJobsKey(type),
            serverSummaryKey(type, id), typeSummaryKey(type)};
        for (const auto& ref : refs)
          keys.push_back("job:{" + type + "}:" + ref);

        vector<string> args = {id, std::to_string(refs.size())};
        for (const auto& ref : refs) args.push_back(ref);
        args.push_back(type);

        const auto cleaned = redis.template eval<int64_t>(
            kCleanupScript, keys.begin(), keys.end(), args.begin(), args.end());
        if (cleaned == 1)
        {
          // Registry keys are in the {registry} slot, so this is a separate
          // command after the per-type Phase A script has committed.
          redis.srem(registryServersKey(type), id);
        }
      }
    }
  });
}
