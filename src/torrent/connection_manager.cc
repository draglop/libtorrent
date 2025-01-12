// libTorrent - BitTorrent library
// Copyright (C) 2005-2011, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// In addition, as a special exception, the copyright holders give
// permission to link the code of portions of this program with the
// OpenSSL library under certain conditions as described in each
// individual source file, and distribute linked combinations
// including the two.
//
// You must obey the GNU General Public License in all respects for
// all of the code used other than OpenSSL.  If you modify file(s)
// with this exception, you may extend this exception to your version
// of the file(s), but you are not obligated to do so.  If you do not
// wish to do so, delete this exception statement from your version.
// If you delete this exception statement from all source files in the
// program, then also delete it here.
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#include "config.h"

#include <sys/types.h>

#include <rak/address_info.h>
#include <rak/socket_address.h>

#include "net/listen.h"

#include "connection_manager.h"
#include "error.h"
#include "exceptions.h"
#include "globals.h"
#include "manager.h"

namespace torrent {

ConnectionManager::ConnectionManager() :
  m_dnsManager(*this),
  m_dnsCacheTimeoutMs(1000 * 60 * 5),
  m_size(0),
  m_maxSize(0),

  m_priority(iptos_throughput),
  m_sendBufferSize(0),
  m_receiveBufferSize(0),
  m_encryptionOptions(encryption_none),

  m_listen(new Listen),
  m_listen_port(0),
  m_listen_backlog(SOMAXCONN),

  m_protocols_enabled(0xFFFFFFFF),

  m_block_ipv4(false),
  m_block_ipv6(false),
  m_prefer_ipv6(false),

  m_network_active(true) {

  m_bindAddress = (new rak::socket_address())->c_sockaddr();
  m_localAddress = (new rak::socket_address())->c_sockaddr();
  m_proxyAddress = (new rak::socket_address())->c_sockaddr();

  rak::socket_address::cast_from(m_bindAddress)->clear();
  rak::socket_address::cast_from(m_localAddress)->clear();
  rak::socket_address::cast_from(m_proxyAddress)->clear();

  m_slot_resolver = std::bind(&DnsManager::resolve,
                              &m_dnsManager,
                              std::placeholders::_1,
                              std::placeholders::_2,
                              std::placeholders::_3,
                              std::placeholders::_4);

  m_dnsCacheTimer.slot() = std::bind(&ConnectionManager::dns_cache_clear, this);

  dns_cache_clear_schedule();
}

ConnectionManager::~ConnectionManager() {
  priority_queue_erase(&taskScheduler, &m_dnsCacheTimer);

  delete m_listen;

  delete m_bindAddress;
  delete m_localAddress;
  delete m_proxyAddress;
}

bool
ConnectionManager::can_connect() const {
  return m_size < m_maxSize;
}

void
ConnectionManager::set_send_buffer_size(uint32_t s) {
  m_sendBufferSize = s;
}

void
ConnectionManager::set_receive_buffer_size(uint32_t s) {
  m_receiveBufferSize = s;
}

void
ConnectionManager::set_encryption_options(uint32_t options) {
#ifdef USE_OPENSSL
  m_encryptionOptions = options;
#else
  throw input_error("Compiled without encryption support.");
#endif
}

void
ConnectionManager::set_bind_address(const sockaddr* sa) {
  const rak::socket_address* rsa = rak::socket_address::cast_from(sa);

  if (rsa->family() != rak::socket_address::af_inet)
    throw input_error("Tried to set a bind address that is not an af_inet address.");

  rak::socket_address::cast_from(m_bindAddress)->copy(*rsa, rsa->length());
}

void
ConnectionManager::set_local_address(const sockaddr* sa) {
  const rak::socket_address* rsa = rak::socket_address::cast_from(sa);

  if (rsa->family() != rak::socket_address::af_inet)
    throw input_error("Tried to set a local address that is not an af_inet address.");

  rak::socket_address::cast_from(m_localAddress)->copy(*rsa, rsa->length());
}

void
ConnectionManager::set_proxy_address(const sockaddr* sa) {
  const rak::socket_address* rsa = rak::socket_address::cast_from(sa);

  if (rsa->family() != rak::socket_address::af_inet)
    throw input_error("Tried to set a proxy address that is not an af_inet address.");

  rak::socket_address::cast_from(m_proxyAddress)->copy(*rsa, rsa->length());
}

uint32_t
ConnectionManager::filter(const sockaddr* sa) {
  if (!m_slot_filter)
    return 1;
  else
    return m_slot_filter(sa);
}

bool
ConnectionManager::listen_open(port_type begin, port_type end) {
  if (!m_listen->open(begin, end, m_listen_backlog, rak::socket_address::cast_from(m_bindAddress)))
    return false;

  m_listen_port = m_listen->port();

  return true;
}

void
ConnectionManager::listen_close() {
  m_listen->close();
}

void
ConnectionManager::set_listen_backlog(int v) {
  if (v < 1 || v >= (1 << 16))
    throw input_error("backlog value out of bounds");

  if (m_listen->is_open())
    throw input_error("backlog value must be set before listen port is opened");

  m_listen_backlog = v;
}

void
ConnectionManager::dns_cache_clear() {
  m_dnsManager.cache_clear();
  dns_cache_clear_schedule();
}

void
ConnectionManager::dns_cache_clear_intervale_ms_set(uint32_t ms) {
  m_dnsCacheTimeoutMs = ms;
  dns_cache_clear();
}

void
ConnectionManager::dns_cache_clear_schedule() {
  priority_queue_erase(&taskScheduler, &m_dnsCacheTimer);
  if (m_dnsCacheTimeoutMs > 0) {
    const rak::timer next_timeout = cachedTime + rak::timer::from_milliseconds(m_dnsCacheTimeoutMs);
    priority_queue_insert(&taskScheduler, &m_dnsCacheTimer, next_timeout);
  }
}

void
ConnectionManager::dns_server_set(const sockaddr* sa) {
  m_dnsManager.server_set(sa);
}

bool
ConnectionManager::protocol_enabled_get(protocol_t protocol) const {
  return m_protocols_enabled & static_cast<uint32_t>(protocol);
}

void
ConnectionManager::protocol_enabled_set(protocol_t protocol, bool enabled) {
  if (enabled) {
    m_protocols_enabled |= static_cast<uint32_t>(protocol);
  } else {
    m_protocols_enabled &= ~static_cast<uint32_t>(protocol);
  }
}

}
