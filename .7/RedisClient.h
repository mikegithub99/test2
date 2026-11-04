/**
 * @file RedisClient.h
 * @brief Lazy singleton wrapper for standalone sw::redis::Redis or sw::redis::Redis Cluster.
 *
 * The default provider reads the application's get_reg_val() registry. Pass a ConfigProvider to Instance() on its first call only when overriding configuration (for example, in tests).
 */

#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include <functional>
#include <memory>
#include <std::string>
#include <utility>
#include <std::variant>

#include <sw/redis++/redis++.h>


/**
 * @class RedisClient
 * @brief Provides one configured sw::redis::Redis client instance.
 *
 * Configuration is read once during lazy initialization. The first call to
 * Instance() determines the provider for the process-wide singleton.
 */
class RedisClient final
{
public:
  struct Config
  {
    std::string redisUrl;
    bool isCluster = false;
  };

  using ConfigProvider = std::function<Config()>;
  using Client = std::variant<
      std::unique_ptr<sw::redis::Redis>,
      std::unique_ptr<sw::redis::RedisCluster>>;

  /**
   * @brief Gets the process-wide client wrapper, initializing it lazily.
   * @param provider Optional configuration provider. If omitted, the default
   *        provider reads redisUrl and isRedisCluster through get_reg_val().
   *        A supplied provider must be passed on the first call.
   */
  static std::shared_ptr<RedisClient> Instance(ConfigProvider provider = {});

  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;

  /**
   * @brief Invokes a callable with the active sw::redis::Redis or sw::redis::Redis Cluster client.
   * @tparam Fn Callable accepting either concrete client reference.
   */
  template <typename Fn>
  decltype(auto) execute(Fn&& fn)
  {
    return std::visit(
        [&fn](auto& client) -> decltype(auto)
        {
          return std::forward<Fn>(fn)(*client);
        },
        m_client);
  }

private:
  explicit RedisClient(ConfigProvider provider = {});
  Client m_client;
};

#endif // REDIS_CLIENT_H
