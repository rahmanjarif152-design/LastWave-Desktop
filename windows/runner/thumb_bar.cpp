#include "thumb_bar.h"

#include <cwchar>
#include <utility>

#include "resource.h"

namespace lastwave {
namespace {

// 0x4C ("L") prefix matches kDeepLinkCopyData in flutter_window.cpp.
constexpr UINT kIdPrev = 0x4C01;
constexpr UINT kIdPlayPause = 0x4C02;
constexpr UINT kIdNext = 0x4C03;

constexpr int kStatusPlaying = 1;
constexpr int kStatusChanging = 3;

// The taskbar flyout follows the SYSTEM theme (not the apps theme).
bool TaskbarIsLight() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  return ::RegGetValueW(
             HKEY_CURRENT_USER,
             L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
             L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
             &size) == ERROR_SUCCESS &&
         value != 0;
}

}  // namespace

ThumbBar::ThumbBar(HWND hwnd, HINSTANCE instance, ButtonSink on_button)
    : hwnd_(hwnd), instance_(instance), on_button_(std::move(on_button)) {
  taskbar_created_msg_ = ::RegisterWindowMessageW(L"TaskbarButtonCreated");
  // Needed if LastWave ever runs elevated: UIPI would drop Explorer's
  // messages otherwise. Return values intentionally ignored.
  ::ChangeWindowMessageFilterEx(hwnd_, taskbar_created_msg_, MSGFLT_ALLOW,
                                nullptr);
  ::ChangeWindowMessageFilterEx(hwnd_, WM_COMMAND, MSGFLT_ALLOW, nullptr);
  icons_ = LoadIconSet();
  // The button may already exist if the window was shown before we hooked
  // the message; normally the window is still hidden here.
  if (::IsWindowVisible(hwnd_)) OnTaskbarButtonCreated();
}

ThumbBar::~ThumbBar() {
  // Buttons die with the window; just release COM and the icon handles.
  taskbar_.Reset();
  DestroyIconSet(icons_);
}

std::optional<LRESULT> ThumbBar::HandleMessage(UINT message, WPARAM wparam,
                                               LPARAM lparam) {
  if (taskbar_created_msg_ != 0 && message == taskbar_created_msg_) {
    OnTaskbarButtonCreated();
    return 0;
  }
  switch (message) {
    case WM_COMMAND:
      if (HIWORD(wparam) == THBN_CLICKED) {
        const char* button = nullptr;
        switch (LOWORD(wparam)) {
          case kIdPrev: button = "previous"; break;
          case kIdPlayPause: button = "toggle"; break;
          case kIdNext: button = "next"; break;
          default: break;
        }
        if (button == nullptr) return std::nullopt;
        if (on_button_) on_button_(button);
        return 0;
      }
      break;
    case WM_DPICHANGED:
      ReloadIconsAndApply();
      break;
    case WM_SETTINGCHANGE:
      if (lparam != 0 &&
          ::wcscmp(reinterpret_cast<LPCWSTR>(lparam), L"ImmersiveColorSet") ==
              0) {
        ReloadIconsAndApply();
      }
      break;
    default:
      break;
  }
  return std::nullopt;
}

void ThumbBar::Update(const TransportState& state) {
  if (state == state_) return;
  const bool caption_changed = state.caption != state_.caption;
  state_ = state;
  if (caption_changed) ApplyCaption();
  Apply();
}

void ThumbBar::OnTaskbarButtonCreated() {
  // Explorer sends this for every new taskbar button (first show, restore
  // from tray, Explorer restart); each is a fresh button, so re-add.
  taskbar_.Reset();
  added_ = false;
  if (FAILED(::CoCreateInstance(CLSID_TaskbarList, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&taskbar_))) ||
      !taskbar_) {
    return;
  }
  if (FAILED(taskbar_->HrInit())) {
    taskbar_.Reset();
    return;
  }
  if (!icons_.play) icons_ = LoadIconSet();
  THUMBBUTTON buttons[3];
  FillButtons(buttons);
  HRESULT hr = taskbar_->ThumbBarAddButtons(hwnd_, 3, buttons);
  // Add fails when buttons already exist for this taskbar button.
  if (FAILED(hr)) hr = taskbar_->ThumbBarUpdateButtons(hwnd_, 3, buttons);
  added_ = SUCCEEDED(hr);
}

void ThumbBar::ReloadIconsAndApply() {
  IconSet fresh = LoadIconSet();
  if (!fresh.play) {
    DestroyIconSet(fresh);
    return;
  }
  IconSet old = icons_;
  icons_ = fresh;
  Apply();  // shell now holds the new handles
  DestroyIconSet(old);
}

ThumbBar::IconSet ThumbBar::LoadIconSet() const {
  UINT dpi = ::GetDpiForWindow(hwnd_);
  if (dpi == 0) dpi = 96;
  const int px = ::GetSystemMetricsForDpi(SM_CXSMICON, dpi);
  const bool light = TaskbarIsLight();  // light flyout -> dark glyphs
  auto load = [&](int white_id, int dark_id) {
    return static_cast<HICON>(::LoadImageW(
        instance_, MAKEINTRESOURCEW(light ? dark_id : white_id), IMAGE_ICON,
        px, px, LR_DEFAULTCOLOR));
  };
  IconSet set;
  set.prev = load(IDI_THUMB_PREV_WHITE, IDI_THUMB_PREV_DARK);
  set.play = load(IDI_THUMB_PLAY_WHITE, IDI_THUMB_PLAY_DARK);
  set.pause = load(IDI_THUMB_PAUSE_WHITE, IDI_THUMB_PAUSE_DARK);
  set.next = load(IDI_THUMB_NEXT_WHITE, IDI_THUMB_NEXT_DARK);
  return set;
}

void ThumbBar::DestroyIconSet(IconSet& set) {
  for (HICON* icon : {&set.prev, &set.play, &set.pause, &set.next}) {
    if (*icon != nullptr) {
      ::DestroyIcon(*icon);
      *icon = nullptr;
    }
  }
}

void ThumbBar::FillButtons(THUMBBUTTON (&buttons)[3]) const {
  // Buffering ("changing") still shows Pause; only paused/stopped shows Play.
  const bool show_pause =
      state_.status == kStatusPlaying || state_.status == kStatusChanging;
  auto fill = [](THUMBBUTTON& b, UINT id, HICON icon, const wchar_t* tip,
                 bool enabled) {
    b = THUMBBUTTON{};
    b.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    b.iId = id;
    b.hIcon = icon;
    ::wcscpy_s(b.szTip, ARRAYSIZE(b.szTip), tip);
    b.dwFlags = enabled ? THBF_ENABLED : THBF_DISABLED;
  };
  fill(buttons[0], kIdPrev, icons_.prev, L"Previous", state_.canPrev);
  fill(buttons[1], kIdPlayPause, show_pause ? icons_.pause : icons_.play,
       show_pause ? L"Pause" : L"Play", state_.hasTrack);
  fill(buttons[2], kIdNext, icons_.next, L"Next", state_.canNext);
}

void ThumbBar::ApplyCaption() {
  // The taskbar hover preview shows the window title above the thumbnail
  // (Spotify does the same). The app draws its own title bar, so this is
  // otherwise invisible. Falls back to the app name when nothing plays.
  const wchar_t* text =
      state_.caption.empty() ? L"LastWave" : state_.caption.c_str();
  ::SetWindowTextW(hwnd_, text);
}

void ThumbBar::Apply() {
  if (!added_ || !taskbar_) return;
  THUMBBUTTON buttons[3];
  FillButtons(buttons);
  if (FAILED(taskbar_->ThumbBarUpdateButtons(hwnd_, 3, buttons))) {
    added_ = false;  // next TaskbarButtonCreated re-adds
  }
}

}  // namespace lastwave
