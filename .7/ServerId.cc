/**
 * @file ServerId.cc
 * @brief Generate a server identifier from the host IPv4 address and a random suffix.
 */

#include "ServerId.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>

#include <random>
#include <string>

using std::random_device;
using std::string;
using std::uniform_int_distribution;

namespace
{
string hostIPv4()
{
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0)
  {
    return "127.0.0.1";
  }

  string result = "127.0.0.1";
  for (const ifaddrs* item = interfaces; item; item = item->ifa_next)
  {
    if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
        (item->ifa_flags & IFF_LOOPBACK))
    {
      continue;
    }

    char addressText[INET_ADDRSTRLEN] = {};
    const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
    if (inet_ntop(AF_INET, &address->sin_addr, addressText, sizeof(addressText)))
    {
      result = addressText;
      break;
    }
  }

  freeifaddrs(interfaces);
  return result;
}

string randomFiveLetters()
{
  static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
  random_device random;
  uniform_int_distribution<int> choose(0, 25);

  string suffix;
  for (int i = 0; i < 5; ++i)
  {
    suffix.push_back(alphabet[choose(random)]);
  }
  return suffix;
}
} // namespace

string makeServerId()
{
  return hostIPv4() + "-" + randomFiveLetters();
}
