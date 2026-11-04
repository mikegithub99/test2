/**
 * @file RedisWorkerExample.cc
 * @brief Register a worker instance and maintain its heartbeat.
 */

#include "RedisJobStatus.h"
#include "ServerId.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using std::atomic;
using std::chrono::seconds;
using std::cerr;
using std::cout;
using std::exception;
using std::int64_t;
using std::invalid_argument;
using std::string;
using std::this_thread;


namespace
{
atomic<bool> g_running{true};

void onSignal(int)
{
  g_running.store(false);
}

void initializeApplicationConfiguration()
{
  // TODO: initialize the host application's configuration registry before constructing RedisJobStatus.
}

string parseServerType(int argc, char** argv)
{
  const string prefix = "--serverType=";
  for (int i = 1; i < argc; ++i)
  {
    const string arg(argv[i]);
    if (arg.rfind(prefix, 0) == 0 && arg.size() > prefix.size())
    {
      return arg.substr(prefix.size());
    }
  }
  throw invalid_argument(
      "Usage: RedisWorkerExample --serverType=query-server-type1");
}
} // namespace

int main(int argc, char** argv)
{
  try
  {
    const auto serverType = parseServerType(argc, argv);
    initializeApplicationConfiguration();

    const auto serverId = makeServerId();
    RedisJobStatus status([serverType, serverId]
    {
      return RedisJobStatus::Config{true, serverType, serverId};
    });
    status.registerServer();

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    constexpr int64_t ttlSeconds = 30;
    status.refreshHeartbeat(ttlSeconds);
    cout << "Registered " << serverType << " / " << serverId << '\n';

    while (g_running.load())
    {
      this_thread::sleep_for(seconds(10));
      if (g_running.load())
      {
        status.refreshHeartbeat(ttlSeconds);
      }
    }

    cout << "Stopping; heartbeat will expire in up to "
              << ttlSeconds << " seconds.\n";
    return EXIT_SUCCESS;
  }
  catch (const exception& ex)
  {
    cerr << "RedisWorkerExample: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
