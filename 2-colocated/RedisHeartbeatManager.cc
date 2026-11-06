#include "RedisHeartbeatManager.h"
#include "RedisJobStatus.h"
#include <cstdint>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include "RedisJobScripts.inc"
using std::back_inserter;
using std::int64_t;
using std::string;
using std::vector;
namespace {
constexpr int64_t kLeaseTtlMs = 30000;
constexpr auto kLeaseRenewInterval = std::chrono::seconds(5);
string registryTypesKey() { return "servers:{registry}:types"; }
string registryServersKey(const string& type) { return "servers:{registry}:type:" + type; }
string cleanupLeaseKey() { return "servers:{registry}:cleanup-lease"; }
string liveKey(const string& type) { return "servers:live:{" + type + "}"; }
string stateKey(const string& type) { return "servers:state:{" + type + "}"; }
string heartbeatKey(const string& type, const string& id) { return "heartbeat:server:{" + type + "}:" + id; }
string serverJobsKey(const string& type, const string& id) { return "jobs:server:{" + type + "}:" + id; }
string typeJobsKey(const string& type) { return "jobs:type:{" + type + "}"; }
string serverSummaryKey(const string& type, const string& id) { return "summary:server:{" + type + "}:" + id; }
string typeSummaryKey(const string& type) { return "summary:type:{" + type + "}"; }
string ownerToken()
{
  std::random_device rd;
  std::ostringstream out;
  out << std::hex << std::chrono::steady_clock::now().time_since_epoch().count();
  for (int i = 0; i < 4; ++i) out << '-' << rd();
  return out.str();
}
std::mutex& managerMutex() { static std::mutex value; return value; }
std::unique_ptr<RedisHeartbeatManager>& processManager()
{ static std::unique_ptr<RedisHeartbeatManager> value; return value; }
template <typename Redis>
int64_t evalScript(Redis& redis, const char* script,
                   const vector<string>& keys, const vector<string>& args)
{
  return redis.template eval<int64_t>(script, keys.begin(), keys.end(), args.begin(), args.end());
}
} // namespace
class HeartbeatRunner final
{
public:
  explicit HeartbeatRunner(std::shared_ptr<RedisJobStatus> status)
      : m_status(std::move(status)),
        m_thread(&HeartbeatRunner::run, this)
  {
  }

  ~HeartbeatRunner()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_wakeup.notify_all();
    if (m_thread.joinable()) m_thread.join();
  }

private:
  void run()
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stopping)
    {
      lock.unlock();
      try
      {
        m_status->refreshHeartbeat(30);
      }
      catch (const std::exception& e)
      {
        std::cerr << "HeartbeatStart: heartbeat refresh failed: "
                  << e.what() << '\n';
      }
      catch (...)
      {
        std::cerr << "HeartbeatStart: heartbeat refresh failed: unknown exception\n";
      }
      lock.lock();
      m_wakeup.wait_for(lock, std::chrono::seconds(10),
                        [this] { return m_stopping; });
    }
  }

  std::shared_ptr<RedisJobStatus> m_status;
  std::mutex m_mutex;
  std::condition_variable m_wakeup;
  bool m_stopping = false;
  std::thread m_thread;
};

namespace {
std::mutex& heartbeatRunnerMutex()
{
  static std::mutex value;
  return value;
}
std::unique_ptr<HeartbeatRunner>& processHeartbeatRunner()
{
  static std::unique_ptr<HeartbeatRunner> value;
  return value;
}
} // namespace

RedisHeartbeatManager::RedisHeartbeatManager() : m_client(RedisClient::Instance())
{ m_cleanupThread = std::thread(&RedisHeartbeatManager::cleanupLoop, this); }
RedisHeartbeatManager::~RedisHeartbeatManager() { stop(); }
void RedisHeartbeatManager::Start()
{
  std::lock_guard<std::mutex> lock(managerMutex());
  auto& manager = processManager();
  if (!manager) manager.reset(new RedisHeartbeatManager());
}
void RedisHeartbeatManager::HeartbeatStart(std::shared_ptr<RedisJobStatus> status)
{
  if (!status)
    throw std::invalid_argument("HeartbeatStart requires a non-null RedisJobStatus");
  std::lock_guard<std::mutex> lock(heartbeatRunnerMutex());
  auto& runner = processHeartbeatRunner();
  if (!runner) runner.reset(new HeartbeatRunner(std::move(status)));
}

void RedisHeartbeatManager::stop()
{
  { std::lock_guard<std::mutex> lock(m_mutex); m_stopping = true; }
  m_wakeup.notify_all();
  if (m_cleanupThread.joinable()) m_cleanupThread.join();
}
void RedisHeartbeatManager::cleanupLoop()
{
  std::unique_lock<std::mutex> lock(m_mutex);
  while (!m_stopping)
  {
    lock.unlock();
    try { scanOnce(); }
    catch (const std::exception& e)
    { std::cerr << "RedisHeartbeatManager: cleanup scan failed: " << e.what() << '\n'; }
    catch (...)
    { std::cerr << "RedisHeartbeatManager: cleanup scan failed: unknown exception\n"; }
    lock.lock();
    m_wakeup.wait_for(lock, m_scanInterval, [this] { return m_stopping; });
  }
}
void RedisHeartbeatManager::scanOnce()
{
  const string token = ownerToken();
  const vector<string> leaseKeys = {cleanupLeaseKey()};
  const vector<string> acquireArgs = {token, std::to_string(kLeaseTtlMs)};
  m_client->execute([&](auto& redis)
  {
    if (evalScript(redis, kAcquireCleanupLeaseScript, leaseKeys, acquireArgs) != 1)
      return; // another process owns the registry scan lease
    auto lastRenewal = std::chrono::steady_clock::now();
    const auto renewIfNeeded = [&]()
    {
      const auto now = std::chrono::steady_clock::now();
      if (now - lastRenewal < kLeaseRenewInterval) return;
      const vector<string> args = {token, std::to_string(kLeaseTtlMs)};
      if (evalScript(redis, kRenewCleanupLeaseScript, leaseKeys, args) != 1)
        throw std::runtime_error("cleanup scan lease was lost");
      lastRenewal = now;
    };
    const auto release = [&]()
    {
      const vector<string> args = {token};
      evalScript(redis, kReleaseCleanupLeaseScript, leaseKeys, args);
    };
    try
    {
      vector<string> types;
      redis.smembers(registryTypesKey(), back_inserter(types));
      for (const auto& type : types)
      {
        renewIfNeeded();
        vector<string> ids;
        redis.smembers(registryServersKey(type), back_inserter(ids));
        for (const auto& id : ids)
        {
          // If the lease expires during a slow Redis operation, overlapping
          // scans are safe because per-server Lua fencing is authoritative.
          renewIfNeeded();
          const vector<string> fenceKeys = {
            heartbeatKey(type, id), stateKey(type), serverJobsKey(type, id),
            serverSummaryKey(type, id), liveKey(type)};
          const vector<string> fenceArgs = {id};
          const auto fenced = redis.template eval<int64_t>(
            kFenceExpiredServerScript, fenceKeys.begin(), fenceKeys.end(),
            fenceArgs.begin(), fenceArgs.end());
          if (fenced == 0) continue;
          if (fenced == 2)
          { redis.srem(registryServersKey(type), id); continue; }
          if (fenced != 1) continue;
          const string serverJobs = serverJobsKey(type, id);
          vector<string> refs;
          redis.smembers(serverJobs, back_inserter(refs));
          vector<string> keys = {
            liveKey(type), stateKey(type), heartbeatKey(type, id), serverJobs,
            typeJobsKey(type), serverSummaryKey(type, id), typeSummaryKey(type)};
          for (const auto& ref : refs) keys.push_back("job:{" + type + "}:" + ref);
          vector<string> args = {id, std::to_string(refs.size())};
          for (const auto& ref : refs) args.push_back(ref);
          args.push_back(type);
          const auto cleaned = redis.template eval<int64_t>(
            kCleanupExpiredServerScript, keys.begin(), keys.end(),
            args.begin(), args.end());
          if (cleaned == 1 || cleaned == 2) redis.srem(registryServersKey(type), id);
        }
      }
    }
    catch (...)
    {
      try { release(); } catch (...) {}
      throw;
    }
    release();
  });
}
