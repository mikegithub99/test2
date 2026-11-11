/**
 * @file CleanupManager.h
 * @brief Per-type cleanup-manager lease controller.
 */
#ifndef DUAL_ROLE_CLEANUP_MANAGER_H
#define DUAL_ROLE_CLEANUP_MANAGER_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class RedisClient;

/**
 * Controls independent cleanup-manager authority for each server type.
 *
 * This class implements lease acquisition, renewal, and release only. A
 * scanner must pass the returned token to every protected mutation script.
 */
class CleanupManager final
{
public:
  struct Config
  {
    std::int64_t leaseTtlMilliseconds = 30000;
  };

  explicit CleanupManager(Config config = {});
  ~CleanupManager() = default;

  CleanupManager(const CleanupManager&) = delete;
  CleanupManager& operator=(const CleanupManager&) = delete;

  /** Attempts acquisition for one type; returns true only when acquired. */
  bool acquire(const std::string& serverType);

  /** Renews only the exact, still-live lease held by this instance. */
  bool renew(const std::string& serverType);

  /** Compare-and-delete release. */
  bool release(const std::string& serverType);

  /** Returns the token only for a locally recorded authority. */
  std::string tokenFor(const std::string& serverType) const;

  /** Returns the acquired generation, or zero if not held. */
  std::int64_t generationFor(const std::string& serverType) const;

private:
  struct Authority
  {
    std::string token;
    std::int64_t generation = 0;
  };

  static std::string leaseKey(const std::string& serverType);
  static std::string generationKey(const std::string& serverType);
  std::string makeOwnerPrefix() const;

  Config m_config;
  std::shared_ptr<RedisClient> m_client;
  std::string m_ownerPrefix;
  mutable std::mutex m_mutex;
  std::map<std::string, Authority> m_authorities;
};

#endif
