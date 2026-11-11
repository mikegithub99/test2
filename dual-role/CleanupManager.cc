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
} // namespace

CleanupManager::CleanupManager(Config config)
    : m_config(config)
{
  if (m_config.leaseTtlMilliseconds <= 0)
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

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_authorities.find(type) != m_authorities.end()) return true;
  }

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

std::int64_t CleanupManager::generationFor(const std::string& type) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_authorities.find(type);
  return it == m_authorities.end() ? 0 : it->second.generation;
}
