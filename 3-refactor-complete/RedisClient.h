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
#include <string>
#include <utility>
#include <variant>

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
   /**
    * Redis connection configuration.
    */
   struct Config
   {
      /**
       * URL used to connect to Redis.
       */
      std::string redisUrl;
      /**
       * True when the connection targets a Redis Cluster.
       */
      bool isCluster = false;
   };

   /**
    * Callable that supplies Redis connection configuration.
    */
   using ConfigProvider = std::function<Config()>;
   /**
    * Holds the active standalone or cluster Redis client.
    */
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

   /**
    * @brief Prevents copying the Redis client wrapper.
    */
   RedisClient(const RedisClient&) = delete;
   /**
    * Prevents copy assignment of the Redis client wrapper.
    */
   RedisClient& operator=(const RedisClient&) = delete;

   /**
    * @brief Invokes a callable with the active sw::redis::Redis or sw::redis::Redis Cluster client.
    * @tparam Fn Callable accepting either concrete client reference.
    * @param fn Callable invoked with the active Redis client.
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
   /**
    * @brief Constructs the wrapper using the supplied configuration provider.
    * @param provider Provider used to initialize the Redis connection.
    */
   explicit RedisClient(ConfigProvider provider = {});
   /**
    * Owns the active standalone or cluster Redis client.
    */
   Client m_client;
};

#endif // REDIS_CLIENT_H
