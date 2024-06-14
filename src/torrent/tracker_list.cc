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

#include <functional>
#include <random>

#include <rak/functional.h>

#include "net/address_list.h"
#include "torrent/utils/log.h"
#include "torrent/utils/option_strings.h"
#include "torrent/download_info.h"
#include "tracker/tracker_dht.h"
#include "tracker/tracker_http.h"
#include "tracker/tracker_udp.h"

#include "globals.h"
#include "exceptions.h"
#include "tracker.h"
#include "tracker_list.h"

#define LT_LOG_TRACKER(log_level, log_fmt, ...)                         \
  lt_log_print_info(LOG_TRACKER_##log_level, info(), "tracker_list", log_fmt, __VA_ARGS__);

namespace torrent {

TrackerList::TrackerList() :
  m_info(NULL),
  m_state(DownloadInfo::STOPPED),

  m_key(0),
  m_numwant(-1) {
}

bool
TrackerList::has_active() const {
  return std::find_if(begin(), end(), std::mem_fn(&Tracker::is_busy)) != end();
}

bool
TrackerList::has_active_not_scrape() const {
  return std::find_if(begin(), end(), std::mem_fn(&Tracker::is_busy_not_scrape)) != end();
}

bool
TrackerList::has_active_in_group(uint32_t group) const {
  return std::find_if(begin_group(group), end_group(group), std::mem_fn(&Tracker::is_busy)) != end_group(group);
}

bool
TrackerList::has_active_not_scrape_in_group(uint32_t group) const {
  return std::find_if(begin_group(group), end_group(group), std::mem_fn(&Tracker::is_busy_not_scrape)) != end_group(group);
}

bool
TrackerList::has_usable() const {
  for (const Tracker *tracker : *this) {
    if (is_usable(tracker)) {
      return true;
    }
  }

  return false;
}

unsigned int
TrackerList::count_active() const {
  return std::count_if(begin(), end(), std::mem_fn(&Tracker::is_busy));
}

unsigned int
TrackerList::count_usable() const {
  unsigned int count = 0;

  for (const Tracker *tracker : *this) {
    if (is_usable(tracker)) {
      ++count;
    }
  }

  return count;
}

void
TrackerList::close_all_excluding(int event_bitmap) {
  for (iterator itr = begin(); itr != end(); itr++) {
    if ((event_bitmap & (1 << (*itr)->latest_event())))
      continue;

    (*itr)->close();
  }
}

void
TrackerList::disown_all_including(int event_bitmap) {
  for (iterator itr = begin(); itr != end(); itr++) {
    if ((event_bitmap & (1 << (*itr)->latest_event())))
      (*itr)->disown();
  }
}

void
TrackerList::clear() {
  std::for_each(begin(), end(), rak::call_delete<Tracker>());
  base_type::clear();
}

void
TrackerList::clear_stats() {
  std::for_each(begin(), end(), std::mem_fn(&Tracker::clear_stats));
}

void
TrackerList::send_state(Tracker* tracker, int new_event) {
  if (!is_usable(tracker) || new_event == Tracker::EVENT_SCRAPE)
    return;

  if (tracker->is_busy()) {
    if (tracker->latest_event() != Tracker::EVENT_SCRAPE)
      return;

    tracker->close();
  }

  LT_LOG_TRACKER(INFO, "sending [%s] to [group: %u] [url: %s]",
    option_as_string(OPTION_TRACKER_EVENT, new_event), tracker->group(), tracker->url().c_str());

  tracker->send_state(new_event);
  tracker->inc_request_counter();
}

void
TrackerList::send_scrape(Tracker* tracker) {
  if (tracker->is_busy() || !is_usable(tracker))
    return;

  if (!(tracker->flags() & Tracker::flag_can_scrape))
    return;

  if (rak::timer::from_seconds(tracker->scrape_time_last()) + rak::timer::from_seconds(10 * 60) > cachedTime )
    return;

  tracker->send_scrape();
  tracker->inc_request_counter();

  LT_LOG_TRACKER(INFO, "sending 'scrape' (group:%u url:%s)",
                 tracker->group(), tracker->url().c_str());
}

TrackerList::iterator
TrackerList::insert(unsigned int group, Tracker* tracker) {
  tracker->set_group(group);
  
  iterator itr = base_type::insert(end_group(group), tracker);

  if (m_slot_tracker_enabled)
    m_slot_tracker_enabled(tracker);

  return itr;
}

// TODO: Use proper flags for insert options.
void
TrackerList::insert_url(unsigned int group, const std::string& url, bool extra_tracker) {
  Tracker* tracker;
  int flags = 0;

  if (extra_tracker)
    flags |= Tracker::flag_extra_tracker;

  if (std::strncmp("http://", url.c_str(), 7) == 0 ||
      std::strncmp("https://", url.c_str(), 8) == 0) {
    tracker = new TrackerHttp(this, url, flags);

  } else if (std::strncmp("udp://", url.c_str(), 6) == 0) {
    tracker = new TrackerUdp(this, url, flags);

  } else if (std::strncmp("dht://", url.c_str(), 6) == 0 && TrackerDht::is_allowed()) {
    tracker = new TrackerDht(this, url, flags);

  } else {
    LT_LOG_TRACKER(WARN, "could find matching tracker protocol (url:%s)", url.c_str());

    if (extra_tracker)
      throw torrent::input_error("could find matching tracker protocol (url:" + url + ")");

    return;
  }
  
  LT_LOG_TRACKER(INFO, "added tracker (group:%i url:%s)", group, url.c_str());
  insert(group, tracker);
}

TrackerList::iterator
TrackerList::find_url(const std::string& url) {
  return std::find_if(begin(), end(), std::bind(std::equal_to<std::string>(), url,
                                                std::bind(&Tracker::url, std::placeholders::_1)));
}

TrackerList::iterator
TrackerList::find_usable(iterator itr) {
  while (itr != end() && !is_usable(*itr))
    ++itr;

  return itr;
}

TrackerList::const_iterator
TrackerList::find_usable(const_iterator itr) const {
  while (itr != end() && !is_usable(*itr))
    ++itr;

  return itr;
}

TrackerList::iterator
TrackerList::find_next_to_request(iterator itr) {
  LT_LOG_TRACKER(DEBUG, "finding next tracker to request (starting at [group: %u] [url: %s])", (*itr)->group(), (*itr)->url().c_str());

  auto can_request = [this](const Tracker* tracker) {
    return this->is_usable(tracker) && tracker->can_request_state();
  };

  for (; itr != end(); ++itr) {
    if (can_request(*itr)) {
      break;
    }
  }

  if (itr != end() && (*itr)->failed_counter() != 0) {
    // found one that has error, try to find a better one
    for (TrackerList::iterator better_candidate = itr + 1; better_candidate != end(); ++better_candidate) {
      if (!can_request(*better_candidate)) {
        continue;
      }

      if ((*better_candidate)->failed_counter() != 0) {
        if ((*better_candidate)->failed_time_next() < (*itr)->failed_time_next()) {
          itr = better_candidate;
        }
      } else {
        if ((*better_candidate)->success_time_next() < (*itr)->failed_time_next()) {
          itr = better_candidate;
        }

        break;
      }
    }
  }

  LT_LOG_TRACKER(DEBUG, "next tracker to request [group: %d] [url: %s])",
    itr != end() ? (*itr)->group() : -1,  itr != end() ? (*itr)->url().c_str() : "");

  return itr;
}

TrackerList::iterator
TrackerList::begin_group(unsigned int group) {
  return std::find_if(begin(), end(), rak::less_equal(group, std::mem_fn(&Tracker::group)));
}

TrackerList::const_iterator
TrackerList::begin_group(unsigned int group) const {
  return std::find_if(begin(), end(), rak::less_equal(group, std::mem_fn(&Tracker::group)));
}

TrackerList::size_type
TrackerList::size_group() const {
  return !empty() ? back()->group() + 1 : 0;
}

void
TrackerList::cycle_group(unsigned int group) {
  iterator itr = begin_group(group);
  iterator prev = itr;

  if (itr == end() || (*itr)->group() != group)
    return;

  while (++itr != end() && (*itr)->group() == group) {
    std::iter_swap(itr, prev);
    prev = itr;
  }
}

TrackerList::iterator
TrackerList::promote(iterator itr) {
  iterator first = begin_group((*itr)->group());

  if (first == end())
    throw internal_error("torrent::TrackerList::promote(...) Could not find beginning of group.");

  std::swap(*first, *itr);
  return first;
}

void
TrackerList::randomize_group_entries() {
  std::random_device rd;
  std::mt19937 g(rd());

  // Random random random.
  iterator itr = begin();
  
  while (itr != end()) {
    iterator tmp = end_group((*itr)->group());
    std::shuffle(itr, tmp, g);

    itr = tmp;
  }
}

void
TrackerList::receive_tracker_enabled_change(Tracker* tracker, Tracker::enabled_status_t previous, Tracker::enabled_status_t current) {
  LT_LOG_TRACKER(DEBUG, "receiving tracker enabled change [old: %d] [new: %d] for [group: %u] [url: %s]",
     static_cast<int>(previous), static_cast<int>(current), tracker->group(), tracker->url().c_str());

  const bool protocol_is_on = Tracker::is_protocol_enabled(tracker->type());
  const bool tracker_was_on = (previous == Tracker::enabled_status_t::on) ||
    ((previous == Tracker::enabled_status_t::undefined) && protocol_is_on);
  const bool tracker_is_on = (current == Tracker::enabled_status_t::on) ||
    ((current == Tracker::enabled_status_t::undefined) && protocol_is_on);

  if (tracker_was_on && (current == Tracker::enabled_status_t::undefined) && !protocol_is_on) {
    tracker->close();
  }

  if (tracker_is_on != tracker_was_on) {
    if (tracker_is_on) {
      if (m_slot_tracker_enabled) {
        m_slot_tracker_enabled(tracker);
      }
    } else {
      if (m_slot_tracker_disabled) {
        m_slot_tracker_disabled(tracker);
      }
    }
  }
}

void
TrackerList::receive_success(Tracker* tb, AddressList* l) {
  iterator itr = find(tb);

  if (itr == end() || tb->is_busy())
    throw internal_error("TrackerList::receive_success(...) called but the iterator is invalid.");

  // Promote the tracker to the front of the group since it was
  // successfull.
  itr = promote(itr);

  l->sort();
  l->erase(std::unique(l->begin(), l->end()), l->end());

  LT_LOG_TRACKER(INFO, "received %u peers (url:%s)", l->size(), tb->url().c_str());

  tb->m_success_time_last = cachedTime.seconds();
  tb->m_success_counter++;
  tb->m_failed_counter = 0;

  tb->m_latest_sum_peers = l->size();
  tb->m_latest_new_peers = m_slot_success(tb, l);
}

void
TrackerList::receive_failed(Tracker* tb, const std::string& msg) {
  iterator itr = find(tb);

  if (itr == end() || tb->is_busy())
    throw internal_error("TrackerList::receive_failed(...) called but the iterator is invalid.");

  LT_LOG_TRACKER(INFO, "failed to connect to tracker (url:%s msg:%s)", tb->url().c_str(), msg.c_str());

  tb->m_failed_time_last = cachedTime.seconds();
  tb->m_failed_counter++;
  m_slot_failed(tb, msg);
}

void
TrackerList::receive_scrape_success(Tracker* tb) {
  iterator itr = find(tb);

  if (itr == end() || tb->is_busy())
    throw internal_error("TrackerList::receive_success(...) called but the iterator is invalid.");

  LT_LOG_TRACKER(INFO, "received scrape from tracker (url:%s)", tb->url().c_str());

  tb->m_scrape_time_last = cachedTime.seconds();
  tb->m_scrape_counter++;

  if (m_slot_scrape_success)
    m_slot_scrape_success(tb);
}

void
TrackerList::receive_scrape_failed(Tracker* tb, const std::string& msg) {
  iterator itr = find(tb);

  if (itr == end() || tb->is_busy())
    throw internal_error("TrackerList::receive_failed(...) called but the iterator is invalid.");

  LT_LOG_TRACKER(INFO, "failed to scrape tracker (url:%s msg:%s)", tb->url().c_str(), msg.c_str());

  if (m_slot_scrape_failed)
    m_slot_scrape_failed(tb, msg);
}

bool
TrackerList::is_usable(const Tracker *tracker) const {
  bool usable = false;

  switch (tracker->get_enabled_status()) {
    case Tracker::enabled_status_t::on:
      usable = tracker->is_usable();
      break;
    case Tracker::enabled_status_t::off:
      usable = false;
      break;
    case Tracker::enabled_status_t::undefined:
      usable = Tracker::is_protocol_enabled(tracker->type()) && tracker->is_usable();
      break;
  }

  LT_LOG_TRACKER(DEBUG, "is usable check [%s] for [group: %u] [url: %s]", usable ? "success" : "fail", tracker->group(), tracker->url().c_str());

  return usable;
}

}
