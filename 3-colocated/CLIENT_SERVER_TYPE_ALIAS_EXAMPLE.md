# Client-side server-type aliases

This example adds friendly server-type names in the client without changing the
Redis registration format or any server-side code.

The important distinction is:

- **Alias**: a client-owned name used by the application, such as
  `background-processors`.
- **Real server type**: the value registered by the worker and used by the
  existing Redis keys/hash tags, such as `worker`.

The Redis implementation continues to register and discover only real server
types. The client resolves an alias to its real server type before calling
existing `RedisJobStatus` read APIs.

## Example

Assume the worker's existing configuration is unchanged:

```cpp
RedisJobStatus::Config config;
config.serverType = "worker";
config.serverId = "worker-01";

// The worker still registers under "worker".
const auto status = RedisJobStatus::Instance();
status->registerServer();
```

The client maintains its own alias map:

```cpp
#include "RedisJobStatus.h"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using ServerSummary = RedisJobStatus::ServerSummary;

// Client-owned names. Values must match the real serverType configured by
// workers and registered in Redis.
const std::unordered_map<std::string, std::string> kServerTypeAliases = {
    {"background-processors", "worker"},
    {"scheduled-tasks", "scheduler"},
    {"notifications", "notification"}
};

const std::string& resolveServerTypeAlias(const std::string& alias)
{
    const auto it = kServerTypeAliases.find(alias);
    if (it == kServerTypeAliases.end())
    {
        throw std::invalid_argument("Unknown server type alias: " + alias);
    }

    return it->second;
}

// Callers use the alias. The existing Redis API receives the real server type.
std::vector<ServerSummary> getServersForAlias(
    RedisJobStatus& status,
    const std::string& alias)
{
    const std::string& realServerType = resolveServerTypeAlias(alias);
    return status.getServerSummaries(realServerType);
}

// Example client usage.
void showBackgroundProcessors()
{
    const auto status = RedisJobStatus::Instance();

    // "background-processors" is resolved locally to "worker".
    const auto servers = getServersForAlias(*status, "background-processors");

    for (const auto& server : servers)
    {
        // server.serverId and server.counters contain the existing summary data.
        // Display the alias in the client UI if desired.
    }
}
```

## Discovering available aliases

`getServerTypes()` still returns the real registered types, not aliases. A
client can intersect that result with its configured alias map to present only
aliases whose real types are currently registered:

```cpp
#include <algorithm>
#include <string>
#include <vector>

std::vector<std::string> getRegisteredAliases(RedisJobStatus& status)
{
    const auto registeredTypes = status.getServerTypes();
    std::vector<std::string> aliases;

    for (const auto& [alias, realServerType] : kServerTypeAliases)
    {
        if (std::find(
                registeredTypes.begin(),
                registeredTypes.end(),
                realServerType) != registeredTypes.end())
        {
            aliases.push_back(alias);
        }
    }

    return aliases;
}
```

The client can then display `background-processors` while passing `worker`
to `getServerSummaries()`, `getServerTypeSummary()`,
`getServerTypeJobs()`, or other APIs that expect the real server type.

## Constraints

- Do not pass an alias to `registerServer()`, worker configuration, or any
  existing getter that constructs per-type Redis keys.
- Alias values must exactly match the real server types used by workers.
- Keep the mapping in client configuration/code; no Redis alias key or server
  side alias resolution is introduced.
- If an alias changes, only the client mapping changes. Existing Redis data
  and worker registrations are unaffected.
- If multiple aliases point to one real type, they intentionally show the
  same registered servers.
