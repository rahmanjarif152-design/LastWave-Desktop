#ifndef RUNNER_THUMB_BAR_H_
#define RUNNER_THUMB_BAR_H_

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <functional>
#include <optional>

#include <memory>

#include "artwork_thumbnail.h"
#include "transport_state.h"

namespace lastwave {

// Taskbar thumbnail toolbar: Previous / Play-Pause / Next under the
// hover preview (Spotify-style; no like button). Mirrors the
// TransportState pushed through the SMTC channel and sends clicks back
// as onButton("previous" | "toggle" | "next"). Best-effort: every COM or
// shell failure leaves the taskbar without buttons, never an error.
class ThumbBar {
 public:
  using ButtonSink = std::function<void(const char* button)>;

  ThumbBar(HWND hwnd, HINSTANCE instance, ButtonSink on_button);
  ~ThumbBar();
  ThumbBar(const ThumbBar&) = delete;
  ThumbBar& operator=(const ThumbBar&) = delete;

  // Called first thing from FlutterWindow::MessageHandler. Returns a
  // value only for messages it consumes (TaskbarButtonCreated, our
  // THBN_CLICKED); observes WM_DPICHANGED / WM_SETTINGCHANGE and
  // returns nullopt so they still reach Flutter and Win32Window.
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam,
                                       LPARAM lparam);

  void Update(const TransportState& state);

 private:
  struct IconSet {
    HICON prev = nullptr;
    HICON play = nullptr;
    HICON pause = nullptr;
    HICON next = nullptr;
  };

  void OnTaskbarButtonCreated();
  void ReloadIconsAndApply();
  IconSet LoadIconSet() const;
  static void DestroyIconSet(IconSet& set);
  void FillButtons(THUMBBUTTON (&buttons)[3]) const;
  void Apply();
  void ApplyCaption();

  HWND hwnd_;
  HINSTANCE instance_;
  ButtonSink on_button_;
  UINT taskbar_created_msg_ = 0;
  Microsoft::WRL::ComPtr<ITaskbarList3> taskbar_;
  bool added_ = false;
  IconSet icons_;
  std::unique_ptr<ArtworkThumbnail> art_;
  TransportState state_;  // last state from Dart; replayed on re-add
};

}  // namespace lastwave

#endif  // RUNNER_THUMB_BAR_H_
