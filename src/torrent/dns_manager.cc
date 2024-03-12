#include "config.h"

#include "dns_manager.h"
#include "connection_manager.h"
#include "manager.h"
#include "net/socket_address.h"
#include "utils/log.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <map>

#ifdef HAVE_RESOLV_H
#include <resolv.h>
#endif

#define LT_LOG_THIS(log_fmt, ...)                                       \
  lt_log_print_subsystem(torrent::LOG_CONNECTION_DNS, "dns", log_fmt, __VA_ARGS__);

namespace {

// TODO Fix TrackerUdp, etc, if this is made async.
static int
resolve_host_system(const char* host, int family, int socktype, torrent::sa_unique_ptr &sa) {
  sa.reset();

  if (torrent::manager->main_thread_main()->is_current()) {
    torrent::thread_base::release_global_lock();
  }

  struct addrinfo *result = NULL;

  struct addrinfo hints;
  memset(&hints, 0x00, sizeof(hints));
  hints.ai_family = family;
  hints.ai_socktype = socktype;

  const int error_code = getaddrinfo(host, NULL, &hints, &result);

  if (torrent::manager->main_thread_main()->is_current()) {
    torrent::thread_base::acquire_global_lock();
  }

  if (error_code == 0) {
    sa = torrent::sa_copy(result->ai_addr);
    freeaddrinfo(result);
  }

  return error_code;
}

#ifdef HAVE_RESOLV_H
static int
resolve_host_custom(const char* host, int family, int socktype, torrent::sa_unique_ptr &sa) {
  sa.reset();

  unsigned char response[NS_PACKETSZ];
  // TODO ipv6 / T_AAAA ?
  const int len = res_nquery(&_res, host, C_IN, T_A, response, sizeof(response));
  if (len > -1) {
    ns_msg handle;
    if (ns_initparse(response, len, &handle) > -1) {
      for (int i_msg = 0; i_msg < ns_msg_count(handle, ns_s_an); ++i_msg) {
        ns_rr rr;
        if (ns_parserr(&handle, ns_s_an, i_msg, &rr) == 0) {
          if (ns_rr_type(rr) == ns_t_a) {
            if (rr.rdlength != 4) {
              fprintf(stderr, "unexpected rd length [%u]\n", rr.rdlength);
              throw torrent::internal_error("Invalid rd length.");
            }
            const int address = ns_get32(rr.rdata);
            struct sockaddr_in sin;
            memset(&sin, 0x00, sizeof(sin));
            sin.sin_addr.s_addr = address;
            sin.sin_family = AF_INET;
            sa = torrent::sa_copy_in(&sin);
          }
        }
      }
    }
  }

  const int error_code = sa ? 0 : errno;

  return error_code;
}
#endif

struct Cache {
  struct Key {
      std::string m_host;
      int m_family;
      int m_socktype;

      Key(const char *host, int family, int socktype) :
        m_host(host) ,
        m_family(family) ,
        m_socktype(socktype) {
      }

      bool operator<(const Key &other) const {
        if (m_host != other.m_host) {
          return m_host < other.m_host;
        }

        // do these afther cause they will unlikey differ
        if (m_family != other.m_family) {
          return m_family < other.m_family;
        }

        return m_socktype < other.m_socktype;
      }
  };

  struct Value {
    torrent::sa_unique_ptr m_sa;
    int m_error_code;

    Value(torrent::sa_unique_ptr &&sa, int error_code) :
      m_sa(std::move(sa)),
      m_error_code(error_code) {
    }
  };

  sockaddr * add(const char *host, int family, int socktype, int error_code, torrent::sa_unique_ptr &&sa) {
    Key key(host, family, socktype);
    Value value(std::move(sa), error_code);

    auto result = m_values.insert(std::make_pair(key, std::move(value)));
    return result.first->second.m_sa.get();
  }

  void clear() {
    m_values.clear();
  }

  bool retrieve(const char *host, int family, int socktype, int *error_code, sockaddr **sa) const {
    bool retrieved = false;

    Key key(host, family, socktype);

    auto it = m_values.find(key);
    if (it != m_values.end()) {
      *error_code = it->second.m_error_code;
      *sa = it->second.m_sa.get();
      retrieved = true;
    }

    return retrieved;
  }

  std::map<Key, Value> m_values;
};

}

namespace torrent {

struct DnsManager::Implementation {
  Implementation(const ConnectionManager &cm) :
    m_networkManager(cm) { // do not use ConnectionManager here, it's not fully built yet

  }

  bool is_on() const {
    return m_networkManager.network_active_get() && m_enabled;
  }

  const ConnectionManager &m_networkManager;
  const sockaddr *m_custom_server = nullptr;
  Cache m_cache;
  bool m_enabled = true;
};

DnsManager::DnsManager(const ConnectionManager &cm) :
  m_impl(new Implementation(cm)) { // do not use ConnectionManager here, it's not fully built yet
}

void
DnsManager::cache_clear() {
  LT_LOG_THIS("clearing cache", nullptr);
  m_impl->m_cache.clear();
}

bool
DnsManager::resolve(const char* host, int family, int socktype, resolve_result_callback_type cb) {
  bool skip = true;

  LT_LOG_THIS("resolving [%s]", host);

  if (m_impl->is_on()) {
    int error_code = 0;
    sockaddr *sa = NULL;

    if (m_impl->m_cache.retrieve(host, family, socktype, &error_code, &sa)) {
      LT_LOG_THIS("using cache for [%s]", host);
    } else {
      LT_LOG_THIS("querying server for [%s]", host);
      sa_unique_ptr sa_u;
      if (!m_impl->m_custom_server) {
        error_code = resolve_host_system(host, family, socktype, sa_u);
      } else {
        error_code = resolve_host_custom(host, family, socktype, sa_u);
      }

      LT_LOG_THIS("got server result for [%s] [error code: %d (%s)]", host, error_code, error_code == 0 ? "OK" : "KO");

      sa = m_impl->m_cache.add(host, family, socktype, error_code, std::move(sa_u));
    }

    cb(sa, error_code);
    skip = false;
  } else {
    LT_LOG_THIS("skipped [%s]", host);
    skip = true;
  }

  return skip;
}

void
DnsManager::server_set(const sockaddr* sa) {
#ifdef HAVE_RESOLV_H
  if (sa && sa->sa_family != AF_INET) {
      throw input_error("Tried to set a custom dns server that is not ipv4.");
  }

  const int r = res_ninit(&_res);
  if (r != 0) {
    fprintf(stderr, "Failed to res_init, error code: [%d]", r);
    throw internal_error("Failed to res_init.");
  }

  sockaddr_in sin;
  memcpy(&sin, sa, sizeof(sockaddr_in));
  if (sin.sin_port == 0) {
    sin.sin_port = htons(53);
  }

  memcpy(&_res.nsaddr_list[0], &sin, sizeof(sockaddr_in));
  _res.nscount = 1;

  m_impl->m_custom_server = sa;
#else
  throw internal_error("Can't set custom DNS server, it was compiled out.");
#endif
}

}
