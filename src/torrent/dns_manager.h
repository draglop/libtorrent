#ifndef LIBTORRENT_DNS_MANAGER_H
#define LIBTORRENT_DNS_MANAGER_H

#include <torrent/common.h>

#include <functional>

struct sockaddr;

namespace torrent {

class ConnectionManager;

class LIBTORRENT_EXPORT DnsManager {
public:

  using resolve_result_callback_type = std::function<void (const sockaddr*, int)>;

  DnsManager(const ConnectionManager &);
  DnsManager(const DnsManager &) = delete;
  DnsManager& operator=(const DnsManager &) = delete;

  // resolving can be sync or async
  // return false if the manager didn't/won't try to resolve
  bool resolve(const char* host, int family, int socktype, resolve_result_callback_type cb);
  void server_set(const sockaddr* sa);

  void cache_clear();

private:
  struct Implementation;
  Implementation *m_impl;
};

}

#endif