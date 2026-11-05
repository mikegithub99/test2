/**
 * @file RedisJobScripts.inc
 * @brief Embedded Lua scripts for RedisJobStatus.
 *
 * This file is included by RedisJobStatus.cc and RedisHeartbeatManager.cc.
 * Each translation unit includes it in its own implementation scope. Script
 * bodies are C++ raw string literals so they ship with the executable
 * without a generator or runtime script-file lookup.
 *
 * Lua features used by these scripts:
 * - `local` and `function` for scoped variables and helper functions.
 * - `if`, `and`, `or`, and `not` for validation and state checks.
 * - `for`, `#`, and `table.insert` to collect optional field/value pairs.
 * - Tables and `unpack` to pass a variable-length HSET argument list.
 * - `string.lower` and `tonumber` to normalize and validate counters.
 * - `KEYS[n]` and `ARGV[n]` at the start of each script to assign descriptive
 *   local names to the explicit keys and arguments supplied by the C++ caller;
 *   Lua tables are one-indexed.
 * - `redis.call` to execute Redis commands and `return` to report results.
 *
 * Redis executes each script atomically with respect to other commands, but
 * scripts do not provide general rollback after a runtime error. Validate
 * key types and state before writing, and keep scripts short.
 */

/**
 * @brief Creates a PENDING job and updates its indexes and summary counters.
 *
 * KEYS[1..5] are assigned to jobKey, typeJobsKey, serverJobsKey,
 * typeSummaryKey, and serverSummaryKey, respectively.
 * ARGV[1..4] are jobId, serverType, serverId, and composite reference.
 * ARGV[5] is job-field count. Remaining arguments are job field/value pairs.
 *
 * Returns RESULT_OK (0), RESULT_ALREADY_EXISTS (5), or
 * RESULT_DATA_INCONSISTENT (4).
 */
const char* const kCreatePendingScript = R"lua(

local RESULT_OK = 0
local RESULT_DATA_INCONSISTENT = 4
local RESULT_ALREADY_EXISTS = 5
local jobKey = KEYS[1]
local typeJobsKey = KEYS[2]
local serverJobsKey = KEYS[3]
local typeSummaryKey = KEYS[4]
local serverSummaryKey = KEYS[5]
local serverStateKey = KEYS[6]

local function keytype(key)
  return redis.call('TYPE', key).ok
end
local function validtype(key, expected)
  local t = keytype(key)
  return t == 'none' or t == expected
end

local existingJobKeyType = keytype(jobKey)
if existingJobKeyType == 'hash' then
  return RESULT_ALREADY_EXISTS
elseif existingJobKeyType ~= 'none' then
  -- A job key must be a HASH. A different existing type is corrupted data,
  -- not a duplicate job ID.
  return RESULT_DATA_INCONSISTENT
end
if not validtype(typeJobsKey, 'set') or
   not validtype(serverJobsKey, 'set') or
   not validtype(typeSummaryKey, 'hash') or
   not validtype(serverSummaryKey, 'hash') or
   keytype(serverStateKey) ~= 'hash' then
  return RESULT_DATA_INCONSISTENT
end
if redis.call('HGET', serverStateKey, ARGV[3]) ~= 'ACTIVE' then
  return 7 -- RESULT_SERVER_NOT_ACTIVE
end

local jobId = ARGV[1]
local serverType = ARGV[2]
local serverId = ARGV[3]
local reference = ARGV[4]
local jobFieldCount = tonumber(ARGV[5])
local fields = {'jobId', jobId, 'status', 'PENDING',
                'serverType', serverType, 'serverId', serverId}
local cursor = 6
for i = 1, jobFieldCount do
  table.insert(fields, ARGV[cursor])
  table.insert(fields, ARGV[cursor + 1])
  cursor = cursor + 2
end

redis.call('HSET', jobKey, unpack(fields))
redis.call('SADD', typeJobsKey, reference)
redis.call('SADD', serverJobsKey, reference)
redis.call('HINCRBY', typeSummaryKey, 'pending', 1)
redis.call('HINCRBY', typeSummaryKey, 'total', 1)
redis.call('HINCRBY', serverSummaryKey, 'pending', 1)
redis.call('HINCRBY', serverSummaryKey, 'total', 1)

return RESULT_OK
)lua";

/**
 * @brief Moves a job between supported lifecycle states and updates counters.
 *
 * jobKey job hash; typeJobsKey type job-ID set; serverJobsKey server job-ID set;
 * typeSummaryKey type summary hash; serverSummaryKey server summary hash.
 * ARGV[1..5] are jobId, serverType, serverId, target state, and composite reference.
 *
 * Returns RESULT_OK (0), RESULT_NO_OP (1), RESULT_NOT_FOUND (2),
 * RESULT_INVALID_TRANSITION (3), or RESULT_DATA_INCONSISTENT (4).
 */
const char* const kTransitionScript = R"lua(

local RESULT_OK = 0
local RESULT_NO_OP = 1
local RESULT_NOT_FOUND = 2
local RESULT_INVALID_TRANSITION = 3
local RESULT_DATA_INCONSISTENT = 4
local jobKey = KEYS[1]
local typeJobsKey = KEYS[2]
local serverJobsKey = KEYS[3]
local typeSummaryKey = KEYS[4]
local serverSummaryKey = KEYS[5]
local serverStateKey = KEYS[6]

local function keytype(key)
  return redis.call('TYPE', key).ok
end
local function validtype(key, expected)
  local t = keytype(key)
  return t == 'none' or t == expected
end

if keytype(serverStateKey) ~= 'hash' then
  return RESULT_DATA_INCONSISTENT
end
if redis.call('HGET', serverStateKey, ARGV[3]) ~= 'ACTIVE' then
  return 7 -- RESULT_SERVER_NOT_ACTIVE
end
if redis.call('EXISTS', jobKey) == 0 then
  return RESULT_NOT_FOUND
end
if not validtype(typeJobsKey, 'set') or
   not validtype(serverJobsKey, 'set') or
   not validtype(typeSummaryKey, 'hash') or
   not validtype(serverSummaryKey, 'hash') then
  return RESULT_DATA_INCONSISTENT
end

local old = redis.call('HGET', jobKey, 'status')
local storedType = redis.call('HGET', jobKey, 'serverType')
local storedServer = redis.call('HGET', jobKey, 'serverId')
local jobId = ARGV[1]
local serverType = ARGV[2]
local serverId = ARGV[3]
local newState = ARGV[4]
local reference = ARGV[5]
if storedType ~= serverType or storedServer ~= serverId or not old then
  return RESULT_DATA_INCONSISTENT
end
if not redis.call('SISMEMBER', typeJobsKey, reference) or
   not redis.call('SISMEMBER', serverJobsKey, reference) then
  return RESULT_DATA_INCONSISTENT
end
if old == newState then
  return RESULT_NO_OP
end

local oldCounter = string.lower(old)
local newCounter = string.lower(newState)
local allowed =
  (old == 'PENDING' and newState == 'ACTIVE') or
  (old == 'ACTIVE' and newState == 'SUSPENDED') or
  (old == 'SUSPENDED' and newState == 'ACTIVE')
if not allowed then
  return RESULT_INVALID_TRANSITION
end

local function hasCounters(key)
  local oldValue = tonumber(redis.call('HGET', key, oldCounter) or '0')
  local total = tonumber(redis.call('HGET', key, 'total') or '0')
  return oldValue and total and oldValue >= 1 and total >= 1
end
if not hasCounters(typeSummaryKey) or not hasCounters(serverSummaryKey) then
  return RESULT_DATA_INCONSISTENT
end

redis.call('HSET', jobKey, 'status', newState)
redis.call('HINCRBY', typeSummaryKey, oldCounter, -1)
redis.call('HINCRBY', typeSummaryKey, newCounter, 1)
redis.call('HINCRBY', serverSummaryKey, oldCounter, -1)
redis.call('HINCRBY', serverSummaryKey, newCounter, 1)
return RESULT_OK
)lua";

/**
 * @brief Removes a job and decrements its state and total counters.
 *
 * jobKey job hash; typeJobsKey type job-ID set; serverJobsKey server job-ID set;
 * typeSummaryKey type summary hash; serverSummaryKey server summary hash.
 * ARGV[1..4] are jobId, serverType, serverId, and composite reference.
 *
 * Returns RESULT_OK (0), RESULT_NOT_FOUND (2), or
 * RESULT_DATA_INCONSISTENT (4).
 */
const char* const kCompleteScript = R"lua(

local RESULT_OK = 0
local RESULT_NOT_FOUND = 2
local RESULT_DATA_INCONSISTENT = 4
local jobKey = KEYS[1]
local typeJobsKey = KEYS[2]
local serverJobsKey = KEYS[3]
local typeSummaryKey = KEYS[4]
local serverSummaryKey = KEYS[5]
local serverStateKey = KEYS[6]

local function keytype(key)
  return redis.call('TYPE', key).ok
end
local function validtype(key, expected)
  local t = keytype(key)
  return t == 'none' or t == expected
end

if keytype(serverStateKey) ~= 'hash' then
  return RESULT_DATA_INCONSISTENT
end
if redis.call('HGET', serverStateKey, ARGV[3]) ~= 'ACTIVE' then
  return 7 -- RESULT_SERVER_NOT_ACTIVE
end
if redis.call('EXISTS', jobKey) == 0 then
  return RESULT_NOT_FOUND
end
if not validtype(typeJobsKey, 'set') or
   not validtype(serverJobsKey, 'set') or
   not validtype(typeSummaryKey, 'hash') or
   not validtype(serverSummaryKey, 'hash') then
  return RESULT_DATA_INCONSISTENT
end

local status = redis.call('HGET', jobKey, 'status')
local storedType = redis.call('HGET', jobKey, 'serverType')
local storedServer = redis.call('HGET', jobKey, 'serverId')
local jobId = ARGV[1]
local serverType = ARGV[2]
local serverId = ARGV[3]
local reference = ARGV[4]
if storedType ~= serverType or storedServer ~= serverId or not status then
  return RESULT_DATA_INCONSISTENT
end
if not redis.call('SISMEMBER', typeJobsKey, reference) or
   not redis.call('SISMEMBER', serverJobsKey, reference) then
  return RESULT_DATA_INCONSISTENT
end

local counter = string.lower(status)
if status ~= 'PENDING' and status ~= 'ACTIVE' and status ~= 'SUSPENDED' then
  return RESULT_DATA_INCONSISTENT
end
local function hasCounters(key)
  local count = tonumber(redis.call('HGET', key, counter) or '0')
  local total = tonumber(redis.call('HGET', key, 'total') or '0')
  return count and total and count >= 1 and total >= 1
end
if not hasCounters(typeSummaryKey) or not hasCounters(serverSummaryKey) then
  return RESULT_DATA_INCONSISTENT
end

redis.call('HINCRBY', typeSummaryKey, counter, -1)
redis.call('HINCRBY', typeSummaryKey, 'total', -1)
redis.call('HINCRBY', serverSummaryKey, counter, -1)
redis.call('HINCRBY', serverSummaryKey, 'total', -1)
redis.call('SREM', typeJobsKey, reference)
redis.call('SREM', serverJobsKey, reference)
redis.call('DEL', jobKey)
return RESULT_OK
)lua";

/**
 * @brief Adds a server type to global discovery and an instance to that type.
 *
 * KEYS[1] is the global server-type set and KEYS[2] is the type-specific
 * server-ID set. Both keys must use the {registry} Redis Cluster hash tag.
 * ARGV[1] is serverType and ARGV[2] is serverId.
 *
 * Returns the SADD result for the type-specific server set. Returns a Redis
 * error reply before writing if either key exists with a type other than SET.
 */
/**
 * @brief Initializes server-type metadata without overwriting existing values.
 *
 * KEYS[1] is the type summary hash. ARGV[1] is the UTC creation timestamp,
 * ARGV[2] is metadata pair count, and remaining args are field/value pairs.
 */
const char* const kInitializeTypeSummaryMetadataScript = R"lua(
local summaryKey = KEYS[1]
local keyType = redis.call('TYPE', summaryKey).ok
if keyType ~= 'none' and keyType ~= 'hash' then
  return redis.error_reply('wrong key type')
end
redis.call('HSETNX', summaryKey, 'meta:createdAt', ARGV[1])
local count = tonumber(ARGV[2])
local cursor = 3
for i = 1, count do
  redis.call('HSET', summaryKey, 'meta:' .. ARGV[cursor], ARGV[cursor + 1])
  cursor = cursor + 2
end
return 1
)lua";

 /**
  * @brief Merges metadata into one server-ID summary hash.
  *
  * KEYS[1] is the server summary hash. ARGV[1] is metadata pair count;
  * remaining arguments are field/value pairs.
  */
const char* const kSetServerSummaryMetadataScript = R"lua(
local summaryKey = KEYS[1]
local keyType = redis.call('TYPE', summaryKey).ok
if keyType ~= 'none' and keyType ~= 'hash' then
  return redis.error_reply('wrong key type')
end
local count = tonumber(ARGV[1])
local cursor = 2
for i = 1, count do
  redis.call('HSET', summaryKey, 'meta:' .. ARGV[cursor], ARGV[cursor + 1])
  cursor = cursor + 2
end
return 1
)lua";

// Compatibility names for the shorter server-summary metadata script name.
const char* const kSetServerSummaryMetaScript = kSetServerSummaryMetadataScript;
const char* const KSetServerSummaryMetaScript = kSetServerSummaryMetadataScript;

const char* const kRegisterServerScript = R"lua(
local serverTypesKey = KEYS[1]
local typeServersKey = KEYS[2]

local function validtype(key)
  local t = redis.call('TYPE', key).ok
  return t == 'none' or t == 'set'
end

-- Validate both keys before either write so a wrong-type key cannot leave
-- the two registries out of sync.
if not validtype(serverTypesKey) or not validtype(typeServersKey) then
  return redis.error_reply('wrong key type')
end

local serverType = ARGV[1]
local serverId = ARGV[2]
redis.call('SADD', serverTypesKey, serverType)
return redis.call('SADD', typeServersKey, serverId)
)lua";


// Per-type lifecycle state initialization. This script is deliberately separate
// from registry registration because the two key groups occupy different slots.
const char* const kActivateServerScript = R"lua(
local stateKey = KEYS[1]
local keyType = redis.call('TYPE', stateKey).ok
if keyType ~= 'none' and keyType ~= 'hash' then
  return redis.error_reply('wrong state key type')
end
local current = redis.call('HGET', stateKey, ARGV[1])
if current == 'ACTIVE' then return 1 end
if current ~= false then return 0 end
redis.call('HSET', stateKey, ARGV[1], 'ACTIVE')
return 1
)lua";

// Heartbeat refresh and live-set publication are one atomic per-type action.
const char* const kRefreshHeartbeatScript = R"lua(
local heartbeatKey = KEYS[1]
local liveKey = KEYS[2]
local stateKey = KEYS[3]
local heartbeatType = redis.call('TYPE', heartbeatKey).ok
local liveType = redis.call('TYPE', liveKey).ok
local stateType = redis.call('TYPE', stateKey).ok
if (heartbeatType ~= 'none' and heartbeatType ~= 'string') or
   (liveType ~= 'none' and liveType ~= 'set') or stateType ~= 'hash' then
  return redis.error_reply('wrong heartbeat key type')
end
if redis.call('HGET', stateKey, ARGV[1]) ~= 'ACTIVE' then return 0 end
redis.call('SET', heartbeatKey, ARGV[1], 'EX', ARGV[2])
redis.call('SADD', liveKey, ARGV[1])
return 1
)lua";


// ---------------------------------------------------------------------------
// RedisHeartbeatManager scripts. Kept in this shared include alongside the
// job lifecycle and heartbeat-refresh scripts so all Lua remains in one file.
// Fence KEYS: heartbeat, state hash, server jobs set, server summary hash,
// live set. ARGV[1]: serverId. Returns 0=not expired/not eligible,
// 1=fenced or already fenced, 2=Phase A already completed.
// ---------------------------------------------------------------------------

const char* const kFenceExpiredServerScript = R"lua(
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

if (ht ~= 'none' and ht ~= 'string') or
   (st ~= 'none' and st ~= 'hash') or
   (jt ~= 'none' and jt ~= 'set') or
   (mt ~= 'none' and mt ~= 'hash') or
   (lt ~= 'none' and lt ~= 'set') then
  return redis.error_reply('wrong type during heartbeat fencing')
end

if redis.call('EXISTS', heartbeat) == 1 then return 0 end

local current = redis.call('HGET', state, ARGV[1])
if current == 'DELETING' then
  if jt == 'none' and mt == 'none' and
     redis.call('SISMEMBER', live, ARGV[1]) == 0 then return 2 end
  return 1
end
if current ~= 'ACTIVE' then return 0 end

-- Do not collect a registered process until it has published its first
-- heartbeat. The state change and expiry check are atomic with refresh.
if redis.call('SISMEMBER', live, ARGV[1]) == 0 then return 0 end
redis.call('HSET', state, ARGV[1], 'DELETING')
return 1
)lua";

// Cleanup KEYS[1..7]: live set, state hash, heartbeat, server jobs set,
// type jobs set, server summary hash, type summary hash. KEYS[8..] are the
// candidate job hashes in the same order as ARGV[3..]. ARGV[1]=serverId,
// ARGV[2]=job count, ARGV[3..2+count]=composite refs, ARGV[3+count]=type.
const char* const kCleanupExpiredServerScript = R"lua(
local live = KEYS[1]
local state = KEYS[2]
local heartbeat = KEYS[3]
local serverJobs = KEYS[4]
local typeJobs = KEYS[5]
local serverSummary = KEYS[6]
local typeSummary = KEYS[7]
local serverId = ARGV[1]
local count = tonumber(ARGV[2])
local serverType = ARGV[3 + (count or 0)]

if not count or count < 0 or count ~= math.floor(count) or
   #KEYS ~= 7 + count or #ARGV ~= 3 + count then
  return redis.error_reply('invalid cleanup arguments')
end

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

-- Another manager may have completed Phase A after this worker fenced/read
-- the same candidate. Recognize the committed shape without decrementing twice.
if keytype(serverJobs) == 'none' and keytype(serverSummary) == 'none' and
   redis.call('SISMEMBER', live, serverId) == 0 then
  return 2
end

local refs = redis.call('SMEMBERS', serverJobs)
if #refs ~= count then
  return redis.error_reply('server job index changed during cleanup preflight')
end
local supplied = {}
for i = 1, count do supplied[ARGV[2 + i]] = true end
for _, ref in ipairs(refs) do
  if not supplied[ref] then
    return redis.error_reply('server job reference missing from cleanup arguments')
  end
end

local pending, active, suspended, total = 0, 0, 0, 0
local jobKeys = {}
for i = 1, count do
  local ref = ARGV[2 + i]
  local jobKey = KEYS[7 + i]
  if keytype(jobKey) ~= 'hash' then
    return redis.error_reply('missing or wrong-type job hash')
  end
  local jobType = redis.call('HGET', jobKey, 'serverType')
  local owner = redis.call('HGET', jobKey, 'serverId')
  local status = redis.call('HGET', jobKey, 'status')
  if owner ~= serverId or jobType ~= serverType then
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
  jobKeys[i] = jobKey
  total = total + 1
end

local function enough(field, decrement)
  if decrement == 0 then return true end
  local raw = redis.call('HGET', typeSummary, field)
  if not raw or not string.match(raw, '^%d+$') then return false end
  local value = tonumber(raw)
  return value and value >= decrement
end
if not enough('pending', pending) or not enough('active', active) or
   not enough('suspended', suspended) or not enough('total', total) then
  return redis.error_reply('invalid or insufficient type counters')
end

-- All validations precede the first write. Every key is in this type's slot.
if pending > 0 then redis.call('HINCRBY', typeSummary, 'pending', -pending) end
if active > 0 then redis.call('HINCRBY', typeSummary, 'active', -active) end
if suspended > 0 then redis.call('HINCRBY', typeSummary, 'suspended', -suspended) end
if total > 0 then redis.call('HINCRBY', typeSummary, 'total', -total) end
for i = 1, count do
  redis.call('SREM', typeJobs, ARGV[2 + i])
  redis.call('DEL', jobKeys[i])
end
redis.call('DEL', serverJobs)
redis.call('DEL', serverSummary, heartbeat)
redis.call('SREM', live, serverId)
-- Keep DELETING as a terminal tombstone; process IDs are never reused.
return 1
)lua";
