/**
 * @file CleanupManagerScripts.inc
 * @brief Redis Cluster slot-local per-server-type manager election scripts.
 * All keys passed to these scripts must use the same {serverType} hash tag.
 * The generation key is persistent and must not be reset during normal operation.
 */

// KEYS[1]=lease, KEYS[2]=generation.
// ARGV[1]=unique owner prefix, ARGV[2]=lease TTL milliseconds.
// Returns {"1", generation, token} on acquisition, or {"0"} when busy.
const char* const kAcquireTypeCleanupManagerScript = R"lua(
local lease = KEYS[1]
local generation = KEYS[2]
local lt = redis.call('TYPE', lease).ok
local gt = redis.call('TYPE', generation).ok
if (lt ~= 'none' and lt ~= 'string') or
   (gt ~= 'none' and gt ~= 'string') then
  return redis.error_reply('wrong manager key type')
end
if redis.call('EXISTS', lease) == 1 then return {'0'} end
local ttl = tonumber(ARGV[2])
if not ttl or ttl <= 0 or ttl ~= math.floor(ttl) then
  return redis.error_reply('invalid manager lease ttl')
end
local nextGeneration = redis.call('INCR', generation)
local token = ARGV[1] .. ':' .. tostring(nextGeneration)
redis.call('SET', lease, token, 'PX', ttl)
return {'1', tostring(nextGeneration), token}
)lua";

const char* const kRenewTypeCleanupManagerScript = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] or
   redis.call('PTTL', KEYS[1]) <= 0 then
  return 0
end
local ttl = tonumber(ARGV[2])
if not ttl or ttl <= 0 or ttl ~= math.floor(ttl) then
  return redis.error_reply('invalid manager lease ttl')
end
redis.call('PEXPIRE', KEYS[1], ttl)
return 1
)lua";

const char* const kReleaseTypeCleanupManagerScript = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
return redis.call('DEL', KEYS[1])
)lua";

// Include at the start of every manager-owned per-type mutation script.
// KEYS[1]=lease and ARGV[1]=expected token; shift other arguments accordingly.
const char* const kValidateTypeCleanupManagerScript = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] or
   redis.call('PTTL', KEYS[1]) <= 0 then
  return -10 -- RESULT_STALE_CLEANUP_MANAGER
end
)lua";
