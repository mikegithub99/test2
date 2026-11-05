/**
 * @file RedisHeartbeatManager.cc
 * @brief Process-wide heartbeat refresh and cluster-safe expired-server cleanup.
 */

#include "RedisHeartbeatManager.h"

#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

#include "RedisJobScripts.inc"

using std::back_inserter;
using std::int64_t;
using std::string;
using std::vector;

namespace
{
string registryTypesKey() { return "servers:{registry}:types"; }
string registryServersKey(const string& type)
{ return "servers:{registry}:type:" + type; }
string liveKey(const string& type) { return "servers:live:{" + type + "}"; }
string stateKey(const string& type) { return "servers:state:{" + type + "}"; }
string heartbeatKey(const string& type, const string& id)
{ return "heartbeat:server:{" + type + "}:" + id; }
string serverJobsKey(const string& type, const string& id)
{ return "jobs:server:{" + type + "}:" + id; }
string typeJobsKey(const string& type) { return "jobs:type:{" + type + "}"; }
string serverSummaryKey(const string& type, const string& id)
{ return "summary:server:{" + type + "}:" + id; }
string typeSummaryKey(const string& type) { return "summary:type:{" + type + "}"; }

std::mutex& managerMutex()
{
  static std::mutex value;
  return value;
}
std::unique_ptr<RedisHeartbeatManager>& processManager()
{
  static std::unique_ptr<RedisHeartbeatManager> value;
  return value;
}
} // namespace

RedisHeartbeatManager::RedisHeartbeatManager(string serverType, string serverId)
    : m_client(RedisClient::Instance()),
      m_serverType(std::move(serverType)),
      m_serverId(std::move(serverId))
{
  if (m_serverType.empty() || m_serverId.empty())
    throw std::invalid_argument("RedisHeartbeatManager: serverType and serverId are required");
  m_thread = std::thread(&RedisHeartbeatManager::run, this);
}

RedisHeartbeatManager::~RedisHeartbeatManager()
{
  stop();
}

void RedisHeartbeatManager::Start(const string& serverType, const string& serverId)
{
  std::lock_guard<std::mutex> lock(managerMutex());
  auto& manager = processManager();
  if (manager)
  {
    if (manager->m_serverType != serverType || manager->m_serverId != serverId)
      throw std::logic_error("RedisHeartbeatManager already started for a different process identity");
    return;
  }
  manager.reset(new RedisHeartbeatManager(serverType, serverId));
}

void RedisHeartbeatManager::stop()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopping = true;
  }
  m_wakeup.notify_all();
  if (m_thread.joinable()) m_thread.join();
}

void RedisHeartbeatManager::run()
{
  auto nextRefresh = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(m_mutex);
  while (!m_stopping)
  {
    lock.unlock();
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextRefresh)
    {
      try
      {
        refreshOwnHeartbeat();
      }
      catch (const std::exception& error)
      {
        std::cerr << "RedisHeartbeatManager: heartbeat refresh failed: "
                  << error.what() << '\n';
      }
      catch (...)
      {
        std::cerr << "RedisHeartbeatManager: heartbeat refresh failed: unknown exception\n";
      }
      nextRefresh = std::chrono::steady_clock::now() +
                    std::chrono::seconds(kHeartbeatIntervalSeconds);
    }

    try { scanOnce(); }
    catch (const std::exception& error)
    {
      std::cerr << "RedisHeartbeatManager: cleanup scan failed: "
                << error.what() << '\n';
    }
    catch (...)
    {
      std::cerr << "RedisHeartbeatManager: cleanup scan failed: unknown exception\n";
    }

    lock.lock();
    m_wakeup.wait_for(lock, m_scanInterval, [this] { return m_stopping; });
  }
}

void RedisHeartbeatManager::refreshOwnHeartbeat()
{
  const vector<string> keys = {
      heartbeatKey(m_serverType, m_serverId),
      liveKey(m_serverType),
      stateKey(m_serverType)};
  const vector<string> args = {
      m_serverId, std::to_string(kHeartbeatTtlSeconds)};
  m_client->execute([&](auto& redis)
  {
    const auto result = redis.template eval<int64_t>(
        kRefreshHeartbeatScript, keys.begin(), keys.end(),
        args.begin(), args.end());
    if (result != 1)
      throw std::runtime_error("own heartbeat rejected: server is not ACTIVE");
  });
}

void RedisHeartbeatManager::scanOnce()
{
  m_client->execute([&](auto& redis)
  {
    vector<string> types;
    redis.smembers(registryTypesKey(), back_inserter(types));
    for (const auto& type : types)
    {
      vector<string> ids;
      redis.smembers(registryServersKey(type), back_inserter(ids));
      for (const auto& id : ids)
      {
        const vector<string> fenceKeys = {
            heartbeatKey(type, id), stateKey(type),
            serverJobsKey(type, id), serverSummaryKey(type, id), liveKey(type)};
        const vector<string> fenceArgs = {id};
        const auto fenced = redis.template eval<int64_t>(
            kFenceExpiredServerScript, fenceKeys.begin(), fenceKeys.end(),
            fenceArgs.begin(), fenceArgs.end());

        if (fenced == 0) continue;
        if (fenced == 2)
        {
          // Phase A already committed before a previous worker interruption.
          redis.srem(registryServersKey(type), id);
          continue;
        }
        if (fenced != 1) continue;

        const string serverJobs = serverJobsKey(type, id);
        vector<string> refs;
        redis.smembers(serverJobs, back_inserter(refs));

        vector<string> keys = {
            liveKey(type), stateKey(type), heartbeatKey(type, id),
            serverJobs, typeJobsKey(type), serverSummaryKey(type, id),
            typeSummaryKey(type)};
        for (const auto& ref : refs)
          keys.push_back("job:{" + type + "}:" + ref);

        vector<string> args = {id, std::to_string(refs.size())};
        for (const auto& ref : refs) args.push_back(ref);
        args.push_back(type);

        const auto cleaned = redis.template eval<int64_t>(
            kCleanupExpiredServerScript, keys.begin(), keys.end(),
            args.begin(), args.end());
        if (cleaned == 1)
        {
          // Registry keys use {registry}, so Phase B is a separate command.
          redis.srem(registryServersKey(type), id);
        }
      }
    }
  });
}
