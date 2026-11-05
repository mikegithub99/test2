/**
 * @file heartbeat.cc
 * @brief Complete worker-process skeleton showing Redis heartbeat ownership.
 *
 * Build this into each server process (not into the central cleanup service).
 * Each process has its own ConfigProvider identity (serverType, serverId).
 * RedisJobStatus starts that process's heartbeat publisher after registration.
 * The dedicated RedisHeartbeatManager service independently scans all types
 * and cleans up processes whose heartbeat keys have expired.
 *
 * Replace the marked application hook with the real queue/job-processing
 * operation for your server. This standalone sample idles until SIGINT/SIGTERM
 * so it can also be used to verify registration and heartbeat refreshes.
 */

#include "RedisJobStatus.h"

#include <chrono>
#include <csignal>
#include <thread>

namespace
{
volatile std::sig_atomic_t g_running = 1;

void requestShutdown(int)
{
  g_running = 0;
}

// Application-specific work hook. Replace this with one iteration of the
// real worker's queue polling / job execution. Keep each iteration bounded
// so the worker can observe shutdown promptly.
void processOneWorkCycle()
{
  // TODO: poll this server's work source and process available work.
  // When no work is available, this sample sleeps briefly rather than
  // spinning at 100% CPU.
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

void runWorker()
{
  while (g_running)
  {
    processOneWorkCycle();
  }
}
} // namespace

// Supplied by the host application. It must initialize the configuration
// registry and RedisClient settings before RedisJobStatus is constructed.
void initializeConfiguration();

int main()
{
  std::signal(SIGINT, requestShutdown);
  std::signal(SIGTERM, requestShutdown);

  initializeConfiguration();

  // Keep this object alive for the entire worker lifetime. Its configured
  // serverType/serverId identify this process; registerServer takes the two
  // metadata maps and does not require callers to pass identity values.
  RedisJobStatus status;
  status.registerServer(
      std::map<std::string, std::string>{}, // server-type metadata
      std::map<std::string, std::string>{}  // this server's metadata
  );

  // The per-process publisher refreshes this server's heartbeat every
  // 10 seconds with a 30-second Redis TTL. No cleanup scanner is started here.
  runWorker();

  // Returning destroys status, stops its heartbeat thread, and allows the
  // central RedisHeartbeatManager to observe expiry and clean up this server.
  return 0;
}
