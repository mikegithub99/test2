/**
 * @file CleanupManager.h
 * @brief Per-type cleanup-manager lease controller.
 */
#ifndef DUAL_ROLE_CLEANUP_MANAGER_H
#define DUAL_ROLE_CLEANUP_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class RedisClient;

/**
 * Controls independent cleanup-manager authority for each server type.
 * One process-wide instance owns the campaigning, scanning, and renewal loops.
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

  using ConfigProvider = std::function<Config()>;

  /**
   * Gets the process-wide manager. The provider is evaluated once, on first
   * initialization. If omitted, timing values are read from get_reg_val().
   */
  static std::shared_ptr<CleanupManager> Instance(ConfigProvider provider = {});
  ~CleanupManager();

  CleanupManager(const CleanupManager&) = delete;
  CleanupManager& operator=(const CleanupManager&) = delete;

  /** Starts automatic per-type discovery, campaigning, scanning, and renewal. */
  void start();
  /** Stops background loops and compare-releases locally held type leases. */
  void stop();

  bool acquire(const std::string& serverType);
  bool renew(const std::string& serverType);
  void scanType(const std::string& serverType);
  bool release(const std::string& serverType);
  std::string tokenFor(const std::string& serverType) const;
  std::int64_t generationFor(const std::string& serverType) const;

private:
  struct Authority
  {
    std::string token;
    std::int64_t generation = 0;
  };

  explicit CleanupManager(ConfigProvider provider);
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
