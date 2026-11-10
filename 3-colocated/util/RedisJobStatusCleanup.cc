#include <sw/redis++/redis++.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{

const std::vector<std::string>& redisJobStatusPatterns()
{
    static const std::vector<std::string> patterns = {
        "servers:{registry}:types",
        "servers:{registry}:type:*",
        "summary:type:{*}",
        "summary:server:{*}:*",
        "job:{*}:*",
        "jobs:type:{*}",
        "jobs:server:{*}:*",
        "heartbeat:server:{*}:*",
        "servers:state:{*}",
        "servers:retired:{*}",
        "servers:{registry}:cleanup-lease"
    };

    return patterns;
}

std::unordered_set<std::string> findRedisJobStatusKeys(sw::redis::Redis& redis)
{
    std::unordered_set<std::string> keys;

    for (const auto& pattern : redisJobStatusPatterns())
    {
        long long cursor = 0;

        do
        {
            std::vector<std::string> batch;
            cursor = redis.scan(cursor, pattern, 500, std::back_inserter(batch));
            keys.insert(batch.begin(), batch.end());
        }
        while (cursor != 0);
    }

    return keys;
}

std::string redisUrl()
{
    const char* value = std::getenv("REDIS_URL");
    return value != nullptr && *value != '\0'
        ? value
        : "redis://127.0.0.1:6379";
}

} // namespace

int main(int argc, char* argv[])
{
    bool deleteKeys = false;

    if (argc == 2 && std::string(argv[1]) == "--delete")
    {
        deleteKeys = true;
    }
    else if (argc != 1)
    {
        std::cerr << "Usage: " << argv[0] << " [--delete]\n"
                  << "Without --delete, matching keys are only listed.\n";
        return 2;
    }

    try
    {
        sw::redis::Redis redis(redisUrl());
        const auto keys = findRedisJobStatusKeys(redis);

        std::vector<std::string> sortedKeys(keys.begin(), keys.end());
        std::sort(sortedKeys.begin(), sortedKeys.end());

        std::cout << "Found " << sortedKeys.size()
                  << " Redis Job Status key(s), including per-type server state hashes and retired-server indexes.\n";

        for (const auto& key : sortedKeys)
        {
            std::cout << "  " << key << '\n';
        }

        if (!deleteKeys)
        {
            std::cout << "\nDry run only. Re-run with --delete to remove these keys.\n";
            return 0;
        }

        std::cout << "\nType DELETE to remove only the listed keys: ";
        std::string confirmation;
        std::getline(std::cin, confirmation);

        if (confirmation != "DELETE")
        {
            std::cout << "Deletion cancelled. No keys were removed.\n";
            return 0;
        }

        std::size_t deleted = 0;

        for (const auto& key : sortedKeys)
        {
            deleted += static_cast<std::size_t>(redis.del(key));
        }

        std::cout << "Deleted " << deleted << " key(s).\n";
    }
    catch (const sw::redis::Error& error)
    {
        std::cerr << "Redis error: " << error.what() << '\n';
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
