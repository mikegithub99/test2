/**
 * @file CleanupManager.cc
 * @brief Implements per-type cleanup-manager lease control.
 */
#include "CleanupManager.h"

#include "CleanupManagerScripts.inc"
#include "RedisClient.h"
#include "RedisJobStatus.h"

#include <chrono>
#include <random>
#include <stdexcept>
#include <utility>
#include <iterator>
#include <vector>

namespace
{
template <typename Redis>
std::int64_t evalInt(
    Redis& redis,
    const char* script,
    const std::vector<std::string>& keys,
    const std::vector<std::string>& args)
{
  return redis.template eval<std::int64_t>(
      script, keys.begin(), keys.end(), args.begin(), args.end());
}
std::string registryServersKey(const std::string& type)
{
  return "servers:{registry}:type:" + type;
}
std::string registryTypesKey()
{
  return "servers:{registry}:types";
}
std::string liveKey(const std::string& type)
{
  return "servers:live:{" + type + "}";
}
std::string stateKey(const std::string& type)
{
  return "servers:state:{" + type + "}";
}
std::string retiredKey(const std::string& type)
{
  return "servers:retired:{" + type + "}";
}
std::string heartbeatKey(const std::string& type, const std::string& id)
{
  return "heartbeat:server:{" + type + "}:" + id;
}
std::string serverJobsKey(const std::string& type, const std::string& id)
{
  return "jobs:server:{" + type + "}:" + id;
}
std::string typeJobsKey(const std::string& type)
{
  return "jobs:type:{" + type + "}";
}
std::string serverSummaryKey(const std::string& type, const std::string& id)
{
  return "summary:server:{" + type + "}:" + id;
}
std::string typeSummaryKey(const std::string& type)
{
  return "summary:type:{" + type + "}";
}
} // namespace

CleanupManager::~CleanupManager()
{
  stop();
}

CleanupManager::CleanupManager(Config config)
    : m_config(config)
{
  if (m_config.leaseTtlMilliseconds <= 0 ||
      m_config.firstHeartbeatTimeoutMilliseconds <= 0 ||
      m_config.tombstoneRetentionMilliseconds <= 0)
  {
    throw std::invalid_argument("CleanupManager lease TTL must be positive");
  }

  const auto status = RedisJobStatus::Instance();
  if (!status->isEnabled())
  {
    return;
  }
  if (status->serverTypeId().empty() || status->serverId().empty())
  {
    throw std::invalid_argument("CleanupManager requires configured worker identity");
  }

  m_ownerPrefix = makeOwnerPrefix();
  m_client = RedisClient::Instance();
}

std::string CleanupManager::leaseKey(const std::string& type)
{
  return "servers:cleanup-manager:{" + type + "}:lease";
}

std::string CleanupManager::generationKey(const std::string& type)
{
  return "servers:cleanup-manager:{" + type + "}:generation";
}

std::string CleanupManager::makeOwnerPrefix() const
{
  const auto status = RedisJobStatus::Instance();
  std::random_device rd;
  return status->serverId() + ":" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
      ":" + std::to_string(rd()) + ":" + std::to_string(rd());
}

bool CleanupManager::acquire(const std::string& type)
{
  if (!m_client || type.empty()) return false;

  bool hasLocalAuthority = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    hasLocalAuthority = m_authorities.find(type) != m_authorities.end();
  }
  if (hasLocalAuthority && renew(type)) return true;

  const std::vector<std::string> keys = {
      leaseKey(type), generationKey(type)};
  const std::vector<std::string> args = {
      m_ownerPrefix, std::to_string(m_config.leaseTtlMilliseconds)};

  const auto acquired = m_client->execute([&](auto& redis)
  {
    return redis.template eval<std::vector<std::string>>(
        kAcquireTypeCleanupManagerScript,
        keys.begin(), keys.end(), args.begin(), args.end());
  });

  if (acquired.empty() || acquired[0] != "1" || acquired.size() != 3)
    return false;

  Authority authority;
  authority.generation = std::stoll(acquired[1]);
  authority.token = acquired[2];

  std::lock_guard<std::mutex> lock(m_mutex);
  m_authorities[type] = std::move(authority);
  return true;
}

bool CleanupManager::renew(const std::string& type)
{
  if (!m_client) return false;

  Authority authority;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_authorities.find(type);
    if (it == m_authorities.end()) return false;
    authority = it->second;
  }

  const std::vector<std::string> keys = {leaseKey(type)};
  const std::vector<std::string> args = {
      authority.token, std::to_string(m_config.leaseTtlMilliseconds)};
  const auto renewed = m_client->execute([&](auto& redis)
  {
    return evalInt(redis, kRenewTypeCleanupManagerScript, keys, args);
  });

  if (renewed != 1)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_authorities.erase(type);
    return false;
  }
  return true;
}

bool CleanupManager::release(const std::string& type)
{
  if (!m_client) return false;

  Authority authority;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_authorities.find(type);
    if (it == m_authorities.end()) return false;
    authority = it->second;
  }

  const std::vector<std::string> keys = {leaseKey(type)};
  const std::vector<std::string> args = {authority.token};
  const auto released = m_client->execute([&](auto& redis)
  {
    return evalInt(redis, kReleaseTypeCleanupManagerScript, keys, args);
  });

  std::lock_guard<std::mutex> lock(m_mutex);
  m_authorities.erase(type);
  return released == 1;
}

std::string CleanupManager::tokenFor(const std::string& type) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_authorities.find(type);
  return it == m_authorities.end() ? std::string{} : it->second.token;
}


// Performs a single type-local scan. Every mutating Lua operation includes
// the lease key and token in the same {serverType} slot. Renewal is checked
// at each scan checkpoint; an expired/ambiguous lease stops this pass.
void CleanupManager::scanType(const std::string& type)
{
  if (!m_client || type.empty()) return;
  if (!acquire(type)) return;

  const auto token = tokenFor(type);
  if (token.empty()) return;
  const auto lease = leaseKey(type);

  try
  {
    m_client->execute([&](auto& redis)
    {
    const auto renewCheckpoint = [&]()
    {
      const std::vector<std::string> keys = {lease};
      const std::vector<std::string> args = {
          token, std::to_string(m_config.leaseTtlMilliseconds)};
      if (evalInt(redis, kRenewTypeCleanupManagerScript, keys, args) != 1)
        throw std::runtime_error("per-type cleanup manager lease lost");
    };

    std::vector<std::string> ids;
    redis.smembers(registryServersKey(type), std::back_inserter(ids));
    for (const auto& id : ids)
    {
      renewCheckpoint();
      const std::vector<std::string> fenceKeys = {
          lease, heartbeatKey(type, id), stateKey(type),
          serverJobsKey(type, id), serverSummaryKey(type, id),
          liveKey(type), retiredKey(type)};
      const std::vector<std::string> fenceArgs = {
          token, id, std::to_string(m_config.firstHeartbeatTimeoutMilliseconds)};
      const auto fenced = evalInt(
          redis, kFenceExpiredServerScript, fenceKeys, fenceArgs);
      if (fenced == -10)
        throw std::runtime_error("stale manager rejected by expiration fence");
      if (fenced == 0) continue;
      if (fenced == 2)
      {
        // Cross-slot registry housekeeping is idempotent; worker IDs are
        // globally unique and never reused across process incarnations.
        redis.srem(registryServersKey(type), id);
        continue;
      }
      if (fenced != 1) continue;

      const auto serverJobs = serverJobsKey(type, id);
      std::vector<std::string> refs;
      redis.smembers(serverJobs, std::back_inserter(refs));
      std::vector<std::string> keys = {
          lease, liveKey(type), stateKey(type), heartbeatKey(type, id),
          serverJobs, typeJobsKey(type), serverSummaryKey(type, id),
          typeSummaryKey(type)};
      for (const auto& ref : refs)
        keys.push_back("job:{" + type + "}:" + ref);

      std::vector<std::string> args = {
          token, id, std::to_string(refs.size())};
      for (const auto& ref : refs) args.push_back(ref);
      args.push_back(type);

      const auto cleaned = evalInt(
          redis, kCleanupExpiredServerScript, keys, args);
      if (cleaned == -10)
        throw std::runtime_error("stale manager rejected by server cleanup");
      if (cleaned == 1 || cleaned == 2)
        redis.srem(registryServersKey(type), id);
    }

    // Backfill tombstones incrementally; this script is manager-fenced.
    const std::vector<std::string> indexKeys = {
        lease, stateKey(type), retiredKey(type)};
    std::string cursor = "0";
    do
    {
      renewCheckpoint();
      const std::vector<std::string> args = {token, cursor};
      cursor = redis.template eval<std::string>(
          kIndexUntrackedTombstonesScript,
          indexKeys.begin(), indexKeys.end(), args.begin(), args.end());
      if (cursor == "-10")
        throw std::runtime_error("stale manager rejected by tombstone backfill");
    } while (cursor != "0");

    renewCheckpoint();
    const std::vector<std::string> retiredIndexKeys = {retiredKey(type)};
    const std::vector<std::string> retentionArgs = {
        std::to_string(m_config.tombstoneRetentionMilliseconds)};
    const auto expiredIds = redis.template eval<std::vector<std::string>>(
        kListExpiredServerTombstonesScript,
        retiredIndexKeys.begin(), retiredIndexKeys.end(),
        retentionArgs.begin(), retentionArgs.end());

    for (const auto& id : expiredIds)
    {
      renewCheckpoint();
      // Registry and type data are in different Cluster slots. This read is
      // intentionally outside Lua; final deletion is revalidated in type slot.
      if (redis.sismember(registryServersKey(type), id)) continue;

      const std::vector<std::string> collectKeys = {
          lease, stateKey(type), heartbeatKey(type, id),
          serverJobsKey(type, id), serverSummaryKey(type, id),
          liveKey(type), retiredKey(type)};
      const std::vector<std::string> collectArgs = {
          token, id, std::to_string(m_config.tombstoneRetentionMilliseconds)};
      const auto collected = evalInt(
          redis, kCollectRetiredServerTombstoneScript, collectKeys, collectArgs);
      if (collected == -10)
        throw std::runtime_error("stale manager rejected by tombstone collection");
    }
    });
  }
  catch (...)
  {
    try { release(type); } catch (...) {}
    throw;
  }
}


void CleanupManager::start()
{
  if (!m_client) return;
  bool expected = false;
  if (!m_running.compare_exchange_strong(expected, true)) return;
  m_managerThread = std::thread(&CleanupManager::managerLoop, this);
  m_renewalThread = std::thread(&CleanupManager::renewalLoop, this);
}

void CleanupManager::stop()
{
  if (!m_running.exchange(false)) return;
  m_wake.notify_all();
  if (m_managerThread.joinable()) m_managerThread.join();
  if (m_renewalThread.joinable()) m_renewalThread.join();
  std::vector<std::string> types;
  { std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& entry : m_authorities) types.push_back(entry.first); }
  for (const auto& type : types) { try { release(type); } catch (...) {} }
}

void CleanupManager::managerLoop()
{
  while (m_running.load())
  {
    try
    {
      const auto types = RedisJobStatus::Instance()->getServerTypes();
      for (const auto& type : types)
      {
        if (!m_running.load()) break;
        if (!acquire(type)) continue;
        try { scanType(type); } catch (...) { /* retry next cycle */ }
      }
    }
    catch (...) { /* fail closed; retry discovery */ }
    std::unique_lock<std::mutex> lock(m_wakeMutex);
    m_wake.wait_for(lock, std::chrono::milliseconds(m_config.scanIntervalMilliseconds),
                    [this] { return !m_running.load(); });
  }
}

void CleanupManager::renewalLoop()
{
  while (m_running.load())
  {
    std::vector<std::string> types;
    { std::lock_guard<std::mutex> lock(m_mutex);
      for (const auto& entry : m_authorities) types.push_back(entry.first); }
    for (const auto& type : types)
    {
      if (!m_running.load()) break;
      try { renew(type); }
      catch (...) { std::lock_guard<std::mutex> lock(m_mutex); m_authorities.erase(type); }
    }
    std::unique_lock<std::mutex> lock(m_wakeMutex);
    m_wake.wait_for(lock, std::chrono::milliseconds(m_config.renewalIntervalMilliseconds),
                    [this] { return !m_running.load(); });
  }
}

std::int64_t CleanupManager::generationFor(const std::string& type) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_authorities.find(type);
  return it == m_authorities.end() ? 0 : it->second.generation;
}
