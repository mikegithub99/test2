/**
 * @file RedisClient.cc
 * @brief Implementation of RedisClient configuration and lazy singleton.
 */

#include "RedisClient.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

using std::invalid_argument;
using std::make_unique;
using std::move;
using std::shared_ptr;
using std::string;
using std::tolower;
using std::transform;
using std::unique_ptr;
using sw::redis::Redis;
using sw::redis::RedisCluster;

// The host application provides this registry lookup.
string get_reg_val(const string& key);

namespace
{
bool parseClusterFlag(string value)
{
  transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch)
                 {
                   return static_cast<char>(tolower(ch));
                 });

  if (value.empty() || value == "false" || value == "0" || value == "no")
  {
    return false;
  }
  if (value == "true" || value == "1" || value == "yes")
  {
    return true;
  }

  throw invalid_argument(
      "RedisClient: isRedisCluster must be true/false, 1/0, or yes/no");
}

RedisClient::Config defaultConfigProvider()
{
  const auto url = get_reg_val("redisUrl");
  if (url.empty())
  {
    throw invalid_argument(
        "RedisClient: get_reg_val(\"redisUrl\") returned an empty value");
  }

  return RedisClient::Config{
      url, parseClusterFlag(get_reg_val("isRedisCluster"))};
}
} // namespace

RedisClient::RedisClient(ConfigProvider provider)
{
  const Config config = provider ? provider() : defaultConfigProvider();
  if (config.redisUrl.empty())
  {
    throw invalid_argument("RedisClient: redisUrl is empty");
  }

  if (config.isCluster)
  {
    m_client.emplace<unique_ptr<RedisCluster>>(
        make_unique<RedisCluster>(config.redisUrl));
  }
  else
  {
    m_client.emplace<unique_ptr<Redis>>(
        make_unique<Redis>(config.redisUrl));
  }
}

shared_ptr<RedisClient> RedisClient::Instance(ConfigProvider provider)
{
  static const shared_ptr<RedisClient> instance(
      new RedisClient(move(provider)));
  return instance;
}
