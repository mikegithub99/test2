/**
 * @file heartbeat.cc
 * @brief Host-application startup/shutdown integration example for dual-role mode.
 *
 * The host supplies initializeConfiguration(), get_reg_val(), and its worker
 * event loop. The ConfigProvider callbacks below are the injection boundary:
 * replace their get_reg_val lookups with the application's ConfigProvider
 * service if it exposes typed accessors.
 */
#include "CleanupManager.h"
#include "RedisJobStatus.h"
#include "WorkerHeartbeatPublisher.h"

#include <map>
#include <string>

void initializeConfiguration();
int runWorkerApplication();
std::string get_reg_val(const std::string& key);

namespace
{
WorkerHeartbeatPublisher::Config heartbeatConfig()
{
  WorkerHeartbeatPublisher::Config config;
  const auto ttl = get_reg_val("heartbeatTtlSeconds");
  const auto interval = get_reg_val("heartbeatRefreshIntervalSeconds");
  if (!ttl.empty()) config.ttlSeconds = std::stoll(ttl);
  if (!interval.empty()) config.refreshIntervalSeconds = std::stoll(interval);
  return config;
}

CleanupManager::Config cleanupConfig()
{
  CleanupManager::Config config;
  const auto leaseTtl = get_reg_val("cleanupManagerLeaseTtlMilliseconds");
  const auto firstHeartbeat = get_reg_val("cleanupFirstHeartbeatTimeoutMilliseconds");
  const auto retention = get_reg_val("cleanupTombstoneRetentionMilliseconds");
  const auto renewal = get_reg_val("cleanupManagerRenewalIntervalMilliseconds");
  const auto scan = get_reg_val("cleanupManagerScanIntervalMilliseconds");
  if (!leaseTtl.empty()) config.leaseTtlMilliseconds = std::stoll(leaseTtl);
  if (!firstHeartbeat.empty()) config.firstHeartbeatTimeoutMilliseconds = std::stoll(firstHeartbeat);
  if (!retention.empty()) config.tombstoneRetentionMilliseconds = std::stoll(retention);
  if (!renewal.empty()) config.renewalIntervalMilliseconds = std::stoll(renewal);
  if (!scan.empty()) config.scanIntervalMilliseconds = std::stoll(scan);
  return config;
}
} // namespace

int main()
{
  initializeConfiguration();

  // Initialize the worker identity before either background service.
  const auto status = RedisJobStatus::Instance();
  if (status->isEnabled())
  {
    status->registerServer(
        std::map<std::string, std::string>{},
        std::map<std::string, std::string>{});
  }

  // Both services are process-wide singletons. The injected providers are
  // consumed once, on the first Instance() call.
  const auto heartbeat = WorkerHeartbeatPublisher::Instance(heartbeatConfig);
  const auto cleanup = CleanupManager::Instance(cleanupConfig);

  heartbeat->start();
  cleanup->start();

  const int result = runWorkerApplication();

  // Stop manager activity first, then stop publishing this worker's heartbeat.
  cleanup->stop();
  heartbeat->stop();
  return result;
}
