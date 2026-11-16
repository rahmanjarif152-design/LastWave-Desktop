#ifndef RUNNER_TRANSPORT_STATE_H_
#define RUNNER_TRANSPORT_STATE_H_

#include <string>

namespace lastwave {

// Transport snapshot shared by native surfaces that mirror the player
// (SMTC, taskbar thumbnail toolbar). Status codes match
// lib/features/smtc/smtc_service.dart: 0 stopped, 1 playing, 2 paused,
// 3 changing (buffering while playing).
struct TransportState {
  int status = 0;
  bool hasTrack = false;
  bool canNext = false;
  bool canPrev = false;
  // "Artist - Title" of the current track, empty when nothing is loaded.
  // Shown as the taskbar thumbnail caption (the window title).
  std::wstring caption;

  bool operator==(const TransportState& o) const {
    return status == o.status && hasTrack == o.hasTrack &&
           canNext == o.canNext && canPrev == o.canPrev &&
           caption == o.caption;
  }
  bool operator!=(const TransportState& o) const { return !(*this == o); }
};

}  // namespace lastwave

#endif  // RUNNER_TRANSPORT_STATE_H_
