/**
 * @file CleanupManager.h
 * @brief Per-type cleanup-manager lease controller.
 */
#ifndef DUAL_ROLE_CLEANUP_MANAGER_H
#define DUAL_ROLE_CLEANUP_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class RedisClient;

/**
 * Controls independent cleanup-manager authority for each server type.
 *
 * This class provides independent per-type discovery, campaigning, scanning,
 * and lease renewal. Worker heartbeat publication remains a separate role.
 */
class CleanupManager final
{
public:
  struct Config
  {
    std::int64_t leaseTtlMilliseconds = 30000;
    std::int64_t firstHeartbeatTimeoutMilliseconds = 60000;
    std::int64_t tombstoneRetentionMilliseconds = 86400000;
    std::int64_t renewalIntervalMilliseconds = 10000;
    std::int64_t scanIntervalMilliseconds = 5000;
  };

  explicit CleanupManager(Config config = {});
  ~CleanupManager();

  CleanupManager(const CleanupManager&) = delete;
  CleanupManager& operator=(const CleanupManager&) = delete;

  /** Starts automatic per-type discovery, campaigning, scanning, and renewal. */
  void start();
  /** Stops background loops and compare-releases locally held type leases. */
  void stop();

  /** Attempts acquisition for one type; returns true only when acquired. */
  bool acquire(const std::string& serverType);

  /** Renews only the exact, still-live lease held by this instance. */
  bool renew(const std::string& serverType);

  /** Runs one fenced cleanup pass for a single type, acquiring authority if needed. */
  void scanType(const std::string& serverType);

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

  void managerLoop();
  void renewalLoop();
  static std::string leaseKey(const std::string& serverType);
  static std::string generationKey(const std::string& serverType);
  std::string makeOwnerPrefix() const;

  Config m_config;
  std::shared_ptr<RedisClient> m_client;
  std::string m_ownerPrefix;
  mutable std::mutex m_mutex;
  std::map<std::string, Authority> m_authorities;
  std::atomic<bool> m_running{false};
  std::condition_variable m_wake;
  std::mutex m_wakeMutex;
  std::thread m_managerThread;
  std::thread m_renewalThread;
};

#endif
