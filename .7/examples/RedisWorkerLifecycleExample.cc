/**
 * @file RedisWorkerLifecycleExample.cc
 * @brief Demonstrates every public RedisJobStatus API.
 *
 * Usage:
 *   ./RedisWorkerLifecycleExample --serverType=query-server-type1
 *       [--redisUrl=redis://127.0.0.1:6379] [--cluster=false]
 *
 * This standalone example injects a mock provider on first use. Production
 * code can omit the provider; RedisClient's default provider calls get_reg_val().
 */

#include "RedisJobStatus.h"
#include "RedisClient.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using std::cerr;
using std::cout;
using std::exception;
using std::hash;
using std::invalid_argument;
using std::map;
using std::runtime_error;
using std::string;
using std::to_string;


namespace
{
struct Options
{
  string serverType;
  string redisUrl = "redis://127.0.0.1:6379";
  bool cluster = false;
  bool statusEnabled = true;
};

// Mock configuration registry for this standalone example.
string mockGetRegVal(const string& key, const Options& options)
{
  if (key == "redisUrl")
  {
    return options.redisUrl;
  }
  if (key == "isRedisCluster")
  {
    return options.cluster ? "true" : "false";
  }
  if (key == "redisJobStatusEnabled")
  {
    return options.statusEnabled ? "true" : "false";
  }
  throw invalid_argument("Unknown mock configuration key: " + key);
}

Options parseArgs(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i)
  {
    const string arg(argv[i]);
    if (arg.rfind("--serverType=", 0) == 0)
      options.serverType = arg.substr(13);
    else if (arg.rfind("--redisUrl=", 0) == 0)
      options.redisUrl = arg.substr(11);
    else if (arg.rfind("--cluster=", 0) == 0)
    {
      const auto value = arg.substr(10);
      options.cluster = value == "true" || value == "1";
    }
    else if (arg.rfind("--statusEnabled=", 0) == 0)
    {
      const auto value = arg.substr(16);
      options.statusEnabled = value == "true" || value == "1" || value == "yes";
    }
    else
      throw invalid_argument("Unknown argument: " + arg);
  }
  if (options.serverType.empty())
    throw invalid_argument(
        "Usage: RedisWorkerLifecycleExample "
        "--serverType=query-server-type1 "
        "[--redisUrl=redis://127.0.0.1:6379] [--cluster=false] "
        "[--statusEnabled=true|false]");
  return options;
}

const char* resultName(RedisJobStatus::Result result)
{
  switch (result)
  {
    case RedisJobStatus::Result::Success: return "Success";
    case RedisJobStatus::Result::NoOp: return "NoOp";
    case RedisJobStatus::Result::JobNotFound: return "JobNotFound";
    case RedisJobStatus::Result::InvalidTransition: return "InvalidTransition";
    case RedisJobStatus::Result::InconsistentData: return "InconsistentData";
    case RedisJobStatus::Result::JobAlreadyExists: return "JobAlreadyExists";
    case RedisJobStatus::Result::Disabled: return "Disabled";
  }
  return "UnknownResult";
}

void printCounters(const map<string, int64_t>& counters)
{
  for (const auto& item : counters)
    cout << "  " << item.first << "=" << item.second << '\n';
}
} // namespace

int main(int argc, char** argv)
{
  try
  {
    const Options options = parseArgs(argc, argv);

    // Do not initialize RedisClient when job-status reporting is disabled.
    if (options.statusEnabled)
    {
      RedisClient::Instance([options]
      {
        const auto url = mockGetRegVal("redisUrl", options);
        const auto cluster = mockGetRegVal("isRedisCluster", options);
        return RedisClient::Config{
            url, cluster == "true" || cluster == "1"};
      });
    }

    const string serverId = "lifecycle-example-" + to_string(
        static_cast<unsigned long long>(hash<string>{}(
            options.serverType + options.redisUrl)) % 1000000ULL);
    RedisJobStatus api([options, serverId]
    {
      return RedisJobStatus::Config{
          options.statusEnabled, options.serverType, serverId};
    });

    if (!options.statusEnabled)
    {
      cout << "RedisJobStatus is disabled; no Redis connection is initialized.\n";
      cout << "createPendingJob result="
           << resultName(api.createPendingJob("")) << '\n';
      cout << "getServerTypes count=" << api.getServerTypes().size() << '\n';
      api.registerServer();
      api.refreshHeartbeat(0);
      cout << "Read methods return empty collections; void methods are no-ops.\n";
      return EXIT_SUCCESS;
    }
    cout << "1) registerServer\n";
    api.registerServer();

    cout << "2) refreshHeartbeat\n";
    api.refreshHeartbeat(60);

    const string jobId = "lifecycle-example-job-" + serverId;
    const map<string, string> metadata = {
        {"command", "demo"}, {"submittedBy", "RedisWorkerLifecycleExample"}};

    cout << "3) createPendingJob\n";
    auto result = api.createPendingJob(jobId, metadata);
    cout << "  result=" << resultName(result) << '\n';
    if (result != RedisJobStatus::Result::Success)
      throw runtime_error(
          "createPendingJob did not return Success; use a fresh serverType "
          "or remove the prior demo job");

    cout << "4) getServerTypes\n";
    for (const auto& type : api.getServerTypes())
      cout << "  " << type << '\n';

    cout << "5) getTypeSummary (PENDING expected)\n";
    printCounters(api.getTypeSummary(options.serverType));

    cout << "6) getServerSummaries\n";
    for (const auto& server : api.getServerSummaries(options.serverType))
    {
      cout << "  serverId=" << server.serverId << '\n';
      printCounters(server.counters);
    }

    cout << "7) getServerJobs (PENDING expected)\n";
    for (const auto& job : api.getServerJobs(options.serverType, serverId))
      cout << "  jobId=" << job.jobId << " status=" << job.status << '\n';

    cout << "8) transitionJob PENDING -> ACTIVE\n";
    result = api.transitionJob(
        jobId, RedisJobStatus::State::Active);
    cout << "  result=" << resultName(result) << '\n';

    cout << "9) transitionJob ACTIVE -> SUSPENDED\n";
    result = api.transitionJob(
        jobId, RedisJobStatus::State::Suspended);
    cout << "  result=" << resultName(result) << '\n';

    cout << "10) transitionJob SUSPENDED -> ACTIVE\n";
    result = api.transitionJob(
        jobId, RedisJobStatus::State::Active);
    cout << "  result=" << resultName(result) << '\n';

    cout << "11) completeJob\n";
    result = api.completeJob(jobId);
    cout << "  result=" << resultName(result) << '\n';

    cout << "12) final getTypeSummary\n";
    printCounters(api.getTypeSummary(options.serverType));

    cout << "13) final getServerJobs (job should be absent)\n";
    cout << "  jobs=" <<
        api.getServerJobs(options.serverType, serverId).size() << '\n';

    cout << "Demo complete. Server registry remains; heartbeat expires "
                 "after its TTL.\n";
    return EXIT_SUCCESS;
  }
  catch (const exception& ex)
  {
    cerr << "RedisWorkerLifecycleExample: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
