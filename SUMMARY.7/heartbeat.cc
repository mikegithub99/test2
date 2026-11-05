/**
 * @file heartbeat.cc
 * @brief Worker-process integration example for RedisJobStatus heartbeats.
 *
 * Build this into each server process, not into the central cleanup service.
 * Each process registers its configured (serverType, serverId) and owns one
 * RedisJobStatus instance for its entire lifetime.
 *
 * RedisJobStatus starts and owns the per-process heartbeat thread when
 * registerServer() succeeds. That thread refreshes immediately, then every
 * 10 seconds, using a 30-second Redis TTL. Do not create a second
 * HeartbeatRunner here: that would publish duplicate refreshes.
 *
 * The central RedisHeartbeatManager is a separate service. It scans all
 * server types and cleans up instances whose heartbeat has expired.
 *
 * Replace processOneWorkCycle() with one bounded iteration of the real
 * application's queue/job-processing operation.
 */

#include "RedisJobStatus.h"

#include <chrono>
#include <csignal>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace
{
volatile std::sig_atomic_t g_running = 1;

void requestShutdown(int)
{
  // Keep the signal handler async-signal-safe. The worker observes this flag
  // on its next bounded wait; RedisJobStatus stops its own thread at teardown.
  g_running = 0;
}

class WorkerLoop final
{
public:
  void run()
  {
    std::unique_lock<std::mutex> lock(m_mutex);

    while (g_running)
    {
      lock.unlock();
      processOneWorkCycle();
      lock.lock();

      // Avoid a busy spin when the work source is empty. The real work hook
      // should itself block efficiently when it can do so.
      m_wakeup.wait_for(
          lock,
          std::chrono::milliseconds(250),
          [] { return !g_running; });
    }
  }

private:
  void processOneWorkCycle()
  {
    // TODO: poll this server's work source and process available work.
    // Keep each iteration bounded so shutdown remains responsive.
  }

  std::mutex m_mutex;
  std::condition_variable m_wakeup;
};
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
  // serverType/serverId identify this process; the two metadata maps describe
  // the server type and this server instance, respectively.
  RedisJobStatus status;
  status.registerServer(
      std::map<std::string, std::string>{}, // server-type metadata
      std::map<std::string, std::string>{}  // server-instance metadata
  );

  // The status object owns the heartbeat publisher. No cleanup scanner runs
  // in this worker process; RedisHeartbeatManager is deployed separately.
  WorkerLoop worker;
  worker.run();

  // On return, status is destroyed and its heartbeat thread is stopped and
  // joined before the process exits.
  return 0;
}
