# Windows Taskbar Thumbnail Media Buttons: Implementation Plan

> **For agentic workers:** Work through this plan one task at a time, in order. Steps use checkbox (`- [ ]`) syntax for tracking. Do not skip the verification steps. Read every file named under "Files" before you edit it.

**Goal:** When the user hovers LastWave's taskbar button on Windows, the thumbnail preview shows three media buttons under it, the way Spotify does: **Previous · Play/Pause · Next**. There is **no** like/"+" button. Play/Pause swaps its icon and tooltip with playback state. Buttons are greyed out when there is nothing to act on. The feature ships in the normal `lastwave_<ver>_windows-x86_64-setup.exe` installer with **no installer changes**, because the icons are compiled into `lastwave_desktop.exe`.

**Architecture:** This is a Windows-native feature built on the shell's *thumbnail toolbar* API (`ITaskbarList3::ThumbBarAddButtons` / `ThumbBarUpdateButtons`). A new C++ class `lastwave::ThumbBar` in `windows/runner/` owns the buttons. It does **not** open a new Dart channel. It reuses the existing `lastwave/smtc` method channel:

- **State in:** Dart already pushes an `update` payload on every track, status or seek change. That payload carries `status`, `hasTrack`, `canNext` and `canPrev` (see `lib/features/smtc/smtc_service.dart` → `smtcUpdateFor`). `SmtcChannel` gains a *transport observer* hook that hands those four fields to `ThumbBar`.
- **Clicks out:** A thumbnail button click arrives as `WM_COMMAND` (`HIWORD == THBN_CLICKED`) on the main window. `FlutterWindow` routes it to `ThumbBar`, which calls `SmtcChannel::SendButton("previous" | "toggle" | "next")`. That sends the same `onButton` call that SMTC media keys already send, and `SmtcService._onNativeCall` already handles `previous`, `toggle` and `next`.

So **no Dart logic changes are required.** The only Dart edit is a doc comment.

**Tech Stack:** C++17 Win32 + COM (`shobjidl.h`, `ITaskbarList3`), Flutter Windows runner (CMake), `.ico` resources compiled through `Runner.rc`, and a Python 3 + Pillow dev-only script that generates the icons. The release installer is Inno Setup 6 (`windows/installer.iss`), built by CI on `v*` tags.

**Repository:** `https://github.com/Clash-Projects/LastWave-Desktop` (Flutter). Note: `Clash-Projects/LastWave-Native` is the **Android** app and is not touched by this plan.

---

## Global Constraints

- Create a new branch `feat/windows-thumbbar-media-buttons` from `origin/main`. Make one commit per task, in the repo's conventional style (`feat(windows): …`). Do **not** push or open a PR unless the user explicitly asks. If the user approves pushing and has no write access to `Clash-Projects`, push to their fork.
- Make changes to Windows code only, except for one doc comment in `smtc_service.dart` (Task 6). Do not change linux/, macos/, the Inno script or the CI workflow.
- Every failure must degrade silently, like the SMTC channel does. COM or shell errors must never block startup or crash the app. No exceptions, and no `assert` that fires in release.
- Bind **only the main window HWND**, never the hidden BotGuard WebView.
- All UI strings are English. The app and installer are English-only today.
- Keep the code style of the existing runner files: 2-space indent, `namespace lastwave`, explanatory comments that say *why*.
- The sandbox is Linux, so the C++ cannot be compiled there. The `build-windows` CI job (Task 7) is the compile gate. Write the code carefully against the Win32 signatures quoted in this plan.

## Review Focus (what a reviewer must be able to confirm)

1. Hide-to-tray followed by restore, and an Explorer restart, both bring the buttons back with the **current** state, because `TaskbarButtonCreated` is handled on every arrival (Task 4).
2. If SMTC fails to initialise (for example on Windows "N" editions without the Media Feature Pack), the thumb bar still updates. The observer is invoked in `SmtcChannel::Impl::Handle()` **before** `ApplyUpdate()`, which returns early when `smtc_` is null (Task 3).
3. During buffering (`status == 3`, "Changing"), the button shows **Pause**. Only Paused or Stopped shows **Play** (Task 4).
4. Tray-menu `WM_COMMAND`s are not swallowed. Only `HIWORD(wparam) == THBN_CLICKED` combined with one of our three IDs is consumed (Task 5).
5. Teardown order: `thumb_bar_` is reset **before** `smtc_channel_`, and the observer is cleared first, so no dangling `this` survives (Task 5).
6. Icons are readable on both dark and light taskbars, and they reload on theme change and on DPI change (Task 4).

---

### Task 1: Generate the thumbnail icons

**Files:**
- Create: `tool/thumbbar_icons.py` (dev-only generator; CI never runs it)
- Create (generated, committed as binaries): `windows/runner/resources/thumb_{prev,play,pause,next}_{white,dark}.ico` (8 files)

**Why two colours:** Windows draws the thumbnail flyout dark when the *system* theme is dark and light when it is light. It never recolours toolbar icons. White glyphs disappear on a light flyout. `ThumbBar` picks the set at runtime (Task 4).

**Sizes:** `SM_CXSMICON` at 100/125/150/175/200/225/250/300 % DPI = **16, 20, 24, 28, 32, 36, 40, 48**. Embed every size so Windows never has to rescale a blurry bitmap.

- [ ] Step 1: Write `tool/thumbbar_icons.py` (run with `python3 tool/thumbbar_icons.py` from the repo root; needs Pillow ≥ 10):

```python
#!/usr/bin/env python3
"""Generates the Windows taskbar thumbnail-toolbar icons.

Dev-only: the .ico outputs are committed and compiled into the exe via
windows/runner/Runner.rc. Re-run only when changing the glyph design.
Glyphs are drawn on a 16-unit design grid, supersampled 8x per size and
downscaled with LANCZOS so every embedded size is crisp, not resampled.
"""
from pathlib import Path
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 28, 32, 36, 40, 48]
SS = 8  # supersample factor
OUT = Path(__file__).resolve().parent.parent / "windows" / "runner" / "resources"
COLORS = {"white": (255, 255, 255), "dark": (28, 28, 28)}

# Shapes on a 16x16 grid: ("poly", [(x, y), ...]) or ("rect", (x0, y0, x1, y1)).
NEXT = [("poly", [(2.5, 2.5), (2.5, 13.5), (10.5, 8.0)]),
        ("rect", (11.0, 2.5, 13.5, 13.5))]
GLYPHS = {
    "play":  [("poly", [(4.5, 2.5), (4.5, 13.5), (13.5, 8.0)])],
    "pause": [("rect", (3.5, 2.5, 6.5, 13.5)), ("rect", (9.5, 2.5, 12.5, 13.5))],
    "next":  NEXT,
    # Previous = horizontal mirror of Next.
    "prev":  [(k, [(16 - x, y) for x, y in v]) if k == "poly"
              else (k, (16 - v[2], v[1], 16 - v[0], v[3])) for k, v in NEXT],
}


def render(shapes, size, rgb):
    big = size * SS
    scale = big / 16.0
    # Transparent pixels carry the glyph colour so LANCZOS leaves no dark fringe.
    img = Image.new("RGBA", (big, big), rgb + (0,))
    d = ImageDraw.Draw(img)
    for kind, v in shapes:
        if kind == "poly":
            d.polygon([(x * scale, y * scale) for x, y in v], fill=rgb + (255,))
        else:
            x0, y0, x1, y1 = (c * scale for c in v)
            d.rounded_rectangle((x0, y0, x1, y1), radius=0.75 * scale, fill=rgb + (255,))
    return img.resize((size, size), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, shapes in GLYPHS.items():
        for tone, rgb in COLORS.items():
            frames = [render(shapes, s, rgb) for s in SIZES]
            path = OUT / f"thumb_{name}_{tone}.ico"
            frames[-1].save(path, format="ICO",
                            sizes=[(s, s) for s in SIZES],
                            append_images=frames[:-1])
            got = sorted(Image.open(path).ico.sizes())
            assert got == [(s, s) for s in SIZES], (path, got)
            print("wrote", path.relative_to(OUT.parent.parent.parent), got[0], "..", got[-1])


if __name__ == "__main__":
    main()
```

- [ ] Step 2: Run it (`pip install pillow` first if needed). Expect 8 `wrote …` lines, each listing the 8 sizes, and no assertion errors.
- [ ] Step 3: Visually spot-check. Composite `thumb_play_white.ico` (48 px frame) on a dark background and `thumb_play_dark.ico` on a light one, save a PNG to the scratchpad, and look at it. The glyphs should be centred, the triangles should point the right way (prev ◀|, next |▶), and the edges should be clean.
- [ ] Step 4: Commit: `feat(windows): add taskbar thumbnail toolbar icons + generator`.

---

### Task 2: Register the icons as resources

**Files:**
- Modify: `windows/runner/resource.h`
- Modify: `windows/runner/Runner.rc` (the `// Icon` section, around line 55)

- [ ] Step 1: In `resource.h`, add these after `IDI_APP_ICON 101`:

```c
#define IDI_THUMB_PREV_WHITE            201
#define IDI_THUMB_PLAY_WHITE            202
#define IDI_THUMB_PAUSE_WHITE           203
#define IDI_THUMB_NEXT_WHITE            204
#define IDI_THUMB_PREV_DARK             205
#define IDI_THUMB_PLAY_DARK             206
#define IDI_THUMB_PAUSE_DARK            207
#define IDI_THUMB_NEXT_DARK             208
```

Bump `_APS_NEXT_RESOURCE_VALUE` to `209`.

- [ ] Step 2: In `Runner.rc`, add these **after** the existing `IDI_APP_ICON` line. The app icon must stay the lowest ID and be listed first, or the exe/taskbar icon changes.

```
IDI_THUMB_PREV_WHITE    ICON                    "resources\\thumb_prev_white.ico"
IDI_THUMB_PLAY_WHITE    ICON                    "resources\\thumb_play_white.ico"
IDI_THUMB_PAUSE_WHITE   ICON                    "resources\\thumb_pause_white.ico"
IDI_THUMB_NEXT_WHITE    ICON                    "resources\\thumb_next_white.ico"
IDI_THUMB_PREV_DARK     ICON                    "resources\\thumb_prev_dark.ico"
IDI_THUMB_PLAY_DARK     ICON                    "resources\\thumb_play_dark.ico"
IDI_THUMB_PAUSE_DARK    ICON                    "resources\\thumb_pause_dark.ico"
IDI_THUMB_NEXT_DARK     ICON                    "resources\\thumb_next_dark.ico"
```

Watch the encoding: `Runner.rc` is compiled by `rc.exe`. Keep its existing encoding and line endings, and do not add a BOM. Use a byte-preserving edit.

- [ ] Step 3: Commit: `feat(windows): embed thumbnail toolbar icons in Runner.rc`.

---

### Task 3: Transport-state hook on `SmtcChannel`

**Files:**
- Create: `windows/runner/transport_state.h`
- Modify: `windows/runner/smtc_channel.h`, `windows/runner/smtc_channel.cpp`

**Interfaces produced:**
- `struct lastwave::TransportState { int status; bool hasTrack; bool canNext; bool canPrev; bool operator==(...) }`
- `void SmtcChannel::SetTransportObserver(std::function<void(const TransportState&)>)`
- `void SmtcChannel::SendButton(const std::string& name)` sends `onButton(name)` to Dart.

- [ ] Step 1: Create `transport_state.h`:

```cpp
#ifndef RUNNER_TRANSPORT_STATE_H_
#define RUNNER_TRANSPORT_STATE_H_

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

  bool operator==(const TransportState& o) const {
    return status == o.status && hasTrack == o.hasTrack &&
           canNext == o.canNext && canPrev == o.canPrev;
  }
  bool operator!=(const TransportState& o) const { return !(*this == o); }
};

}  // namespace lastwave

#endif  // RUNNER_TRANSPORT_STATE_H_
```

- [ ] Step 2: In `smtc_channel.h`, add `#include <functional>`, `#include <string>` and `#include "transport_state.h"`. Add these public members:

```cpp
  // Receives every Dart `update` payload's transport fields, even when
  // SMTC itself failed to initialise (taskbar thumb bar keeps working).
  void SetTransportObserver(std::function<void(const TransportState&)> observer);
  // Forwards a native transport press (e.g. a taskbar thumbnail button)
  // to Dart as `onButton(name)` - same path as SMTC/media keys.
  void SendButton(const std::string& name);
```

- [ ] Step 3: In `smtc_channel.cpp`:
  - Add `std::function<void(const TransportState&)> observer_;` to `Impl`, plus matching `SetObserver(...)` and `Send(const std::string&)` methods. `Send` does `if (channel_) channel_->InvokeMethod("onButton", std::make_unique<EncodableValue>(name));`.
  - In `Impl::Handle()`, once `args` has been validated and **before** `ApplyUpdate(*args)`, build and dispatch the state:

    ```cpp
    if (observer_) {
      TransportState s;
      s.status = IntArg32(*args, "status", kStatusStopped);
      s.hasTrack = BoolArg(*args, "hasTrack", false);
      s.canNext = BoolArg(*args, "canNext", false);
      s.canPrev = BoolArg(*args, "canPrev", false);
      observer_(s);
    }
    ```

    This must sit *before* `ApplyUpdate`, because `ApplyUpdate` returns immediately when `smtc_` is null (Review Focus #2).
  - Implement the two public `SmtcChannel` methods as thin forwards to `impl_`.
- [ ] Step 4: Make no other behaviour changes to SMTC.
- [ ] Step 5: Commit: `feat(windows): expose SMTC transport state + button send to the runner`.

---

### Task 4: `ThumbBar`, the thumbnail toolbar controller

**Files:**
- Create: `windows/runner/thumb_bar.h`, `windows/runner/thumb_bar.cpp`
- Modify: `windows/runner/CMakeLists.txt`: add `"thumb_bar.cpp"` to `add_executable` after `"smtc_channel.cpp"`. The libraries it needs (`ole32`, `uuid`, `shell32`) are already linked. Do not add `comctl32` (`LoadImageW` is in user32).

**Header contract:**

```cpp
#ifndef RUNNER_THUMB_BAR_H_
#define RUNNER_THUMB_BAR_H_

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <functional>
#include <optional>

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
  struct IconSet { HICON prev = nullptr, play = nullptr, pause = nullptr, next = nullptr; };

  void OnTaskbarButtonCreated();
  void ReloadIconsAndApply();
  IconSet LoadIconSet() const;
  static void DestroyIconSet(IconSet& set);
  void FillButtons(THUMBBUTTON (&buttons)[3]) const;
  void Apply();

  HWND hwnd_;
  HINSTANCE instance_;
  ButtonSink on_button_;
  UINT taskbar_created_msg_ = 0;
  Microsoft::WRL::ComPtr<ITaskbarList3> taskbar_;
  bool added_ = false;
  IconSet icons_;
  TransportState state_;  // last state from Dart; replayed on re-add
};

}  // namespace lastwave

#endif  // RUNNER_THUMB_BAR_H_
```

**Implementation requirements (`thumb_bar.cpp`):**

- [ ] Constants: button IDs `kIdPrev = 0x4C01`, `kIdPlayPause = 0x4C02`, `kIdNext = 0x4C03` (the `0x4C` "L" prefix matches `kDeepLinkCopyData`). Buttons are always laid out in the order prev, play/pause, next.
- [ ] Constructor:
  1. `taskbar_created_msg_ = RegisterWindowMessageW(L"TaskbarButtonCreated");`
  2. `ChangeWindowMessageFilterEx(hwnd_, taskbar_created_msg_, MSGFLT_ALLOW, nullptr);` and the same call for `WM_COMMAND`. This is required if LastWave ever runs elevated, because otherwise UIPI drops Explorer's messages. Ignore the return values.
  3. `icons_ = LoadIconSet();`
  4. If `IsWindowVisible(hwnd_)`, call `OnTaskbarButtonCreated()` right away. This guards against the button existing before the observer was wired. Normally the window is still hidden here, because `FlutterWindow` shows it on the first frame.
- [ ] `HandleMessage`:
  - `message == taskbar_created_msg_` → `OnTaskbarButtonCreated(); return 0;`
  - `message == WM_COMMAND && HIWORD(wparam) == THBN_CLICKED`: switch on `LOWORD(wparam)`. `kIdPrev` → `on_button_("previous")`, `kIdPlayPause` → `on_button_("toggle")`, `kIdNext` → `on_button_("next")`, then `return 0;`. Any other ID → `return std::nullopt;`.
  - `WM_DPICHANGED` → `ReloadIconsAndApply(); return std::nullopt;`
  - `WM_SETTINGCHANGE` where `lparam != 0` and `wcscmp(reinterpret_cast<LPCWSTR>(lparam), L"ImmersiveColorSet") == 0` → `ReloadIconsAndApply(); return std::nullopt;`
  - Everything else → `std::nullopt`.
- [ ] `OnTaskbarButtonCreated()`: Explorer sends this **every** time a taskbar button is created for the window: at first show, on restore from hide-to-tray, and after an Explorer restart. Each one is a fresh button, so re-add every time:

  ```cpp
  taskbar_.Reset();
  added_ = false;
  if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&taskbar_))) || !taskbar_) return;
  if (FAILED(taskbar_->HrInit())) { taskbar_.Reset(); return; }
  if (!icons_.play) icons_ = LoadIconSet();
  THUMBBUTTON buttons[3];
  FillButtons(buttons);
  HRESULT hr = taskbar_->ThumbBarAddButtons(hwnd_, 3, buttons);
  // Same taskbar button seen twice (e.g. the constructor's visible-window
  // path raced the message): Add fails once buttons exist - Update instead.
  if (FAILED(hr)) hr = taskbar_->ThumbBarUpdateButtons(hwnd_, 3, buttons);
  added_ = SUCCEEDED(hr);
  ```

  COM is already initialised as STA in `main.cpp` (`CoInitializeEx(COINIT_APARTMENTTHREADED)`), on the same UI thread.
- [ ] `Update(state)`: `if (state == state_) return; state_ = state; Apply();`
- [ ] `Apply()`: `if (!added_ || !taskbar_) return;` then fill the buttons and call `ThumbBarUpdateButtons`. If that fails, set `added_ = false`; the next `TaskbarButtonCreated` re-adds them.
- [ ] `FillButtons`: for each button, set `dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS` and `iId` from the constants.
  - `showPause = state_.status == 1 /*playing*/ || state_.status == 3 /*changing*/` (Review Focus #3).
  - Prev: `hIcon = icons_.prev`, tip `L"Previous"`, `dwFlags = state_.canPrev ? THBF_ENABLED : THBF_DISABLED`.
  - Play/Pause: `hIcon = showPause ? icons_.pause : icons_.play`, tip `showPause ? L"Pause" : L"Play"`, `dwFlags = state_.hasTrack ? THBF_ENABLED : THBF_DISABLED`.
  - Next: `hIcon = icons_.next`, tip `L"Next"`, `dwFlags = state_.canNext ? THBF_ENABLED : THBF_DISABLED`.
  - Copy tooltips with `wcscpy_s(b.szTip, ARRAYSIZE(b.szTip), tip)`. Do **not** set `THBF_DISMISSONCLICK`, so the flyout stays open after a click, like Spotify. Zero-initialise each `THUMBBUTTON` (`THUMBBUTTON b{};`).
  - The initial `state_` is default (stopped, no track), so all three buttons start disabled and the middle one shows Play.
- [ ] `LoadIconSet()`:
  - `const UINT dpi = GetDpiForWindow(hwnd_);` (use 96 if it returns 0), then `const int px = GetSystemMetricsForDpi(SM_CXSMICON, dpi);`.
  - Light-flyout detection reads the **system** theme, not the apps theme, because the taskbar follows the system theme:

    ```cpp
    DWORD v = 0, cb = sizeof(v);
    const bool light_taskbar = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS && v != 0;
    ```

    A light taskbar uses the `*_DARK` resource IDs. A dark taskbar, or a failed read, uses `*_WHITE`.
  - Load each icon with `static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(id), IMAGE_ICON, px, px, LR_DEFAULTCOLOR))`. Do not pass `LR_SHARED`: these handles are owned and must be destroyed.
  - Include `resource.h`.
- [ ] `ReloadIconsAndApply()`: load the new set first, then swap it in, call `Apply()` so the shell holds the new handles, and only then `DestroyIconSet(old)`. Never destroy icons the taskbar may still be drawing.
- [ ] Destructor: `taskbar_.Reset(); DestroyIconSet(icons_);`. Do not remove the buttons; the shell drops them with the window.
- [ ] `DestroyIconSet`: `DestroyIcon` every non-null handle and null it out.
- [ ] Commit: `feat(windows): taskbar thumbnail toolbar controller (prev / play-pause / next)`.

---

### Task 5: Wire `ThumbBar` into `FlutterWindow`

**Files:**
- Modify: `windows/runner/flutter_window.h`, `windows/runner/flutter_window.cpp`

- [ ] Step 1: In the header, forward-declare `class ThumbBar;` inside `namespace lastwave`. Add `std::unique_ptr<lastwave::ThumbBar> thumb_bar_;` **after** `smtc_channel_`, so declaration order matches dependency order.
- [ ] Step 2: In `flutter_window.cpp`, `#include "thumb_bar.h"`. In `OnCreate()`, immediately after `smtc_channel_ = std::make_unique<…>(…)`, add:

```cpp
  // Taskbar thumbnail toolbar (prev / play-pause / next under the hover
  // preview). Driven by the same Dart `update` payload as SMTC; clicks go
  // back through the SMTC channel as onButton, so Dart needs no changes.
  thumb_bar_ = std::make_unique<lastwave::ThumbBar>(
      GetHandle(), ::GetModuleHandleW(nullptr),
      [this](const char* button) {
        if (smtc_channel_ && !destroying_) smtc_channel_->SendButton(button);
      });
  smtc_channel_->SetTransportObserver(
      [this](const lastwave::TransportState& state) {
        if (thumb_bar_) thumb_bar_->Update(state);
      });
```

- [ ] Step 3: In `OnDestroy()`, **before** `smtc_channel_.reset();`, add:

```cpp
  if (smtc_channel_) smtc_channel_->SetTransportObserver(nullptr);
  thumb_bar_.reset();
```

- [ ] Step 4: In `MessageHandler`, as the **very first** block, before the existing `HandleTopLevelWindowProc` forwarding, add:

```cpp
  // Thumb bar first: it consumes only TaskbarButtonCreated and its own
  // THBN_CLICKED ids; theme/DPI messages are observed and fall through.
  if (thumb_bar_ && !destroying_) {
    if (auto handled = thumb_bar_->HandleMessage(message, wparam, lparam)) {
      return *handled;
    }
  }
```

  This sits before the plugin delegates because `window_manager` and `tray_manager` hook the top-level window proc and could return a result for `WM_COMMAND` or `WM_SETTINGCHANGE` before we see it.
- [ ] Step 5: Commit: `feat(windows): mount taskbar thumbnail toolbar on the main window`.

---

### Task 6: Dart doc touch-up (no logic change)

**Files:**
- Modify: `lib/features/smtc/smtc_service.dart`

- [ ] Step 1: In the `SmtcService` class doc comment, add one sentence: on Windows the same `update` payload also drives the taskbar thumbnail toolbar (`windows/runner/thumb_bar.cpp`), and its clicks arrive as `onButton` `previous` / `toggle` / `next`.
- [ ] Step 2: Do **not** change `smtcUpdateFor`. The payload keys `status`, `hasTrack`, `canNext` and `canPrev` are now a contract with `thumb_bar.cpp`; the existing `test/smtc_service_test.dart` already asserts them.
- [ ] Step 3: If Flutter is available in the sandbox, run `flutter analyze lib/features/smtc/smtc_service.dart` and `flutter test test/smtc_service_test.dart`. Both must pass. If Flutter is not installed, say so in the final report rather than claiming they pass.
- [ ] Step 4: Commit: `docs(smtc): note taskbar thumb bar shares the SMTC payload`.

---

### Task 7: Compile gate (CI) and installer

The Linux sandbox cannot build Windows C++. The repo's `.github/workflows/desktop.yml` job **`build-windows`** (`windows-latest`, `flutter build windows --release`) is the compiler.

- [ ] Step 1: Self-review against the Win32 signatures before pushing:
  - `ITaskbarList3::ThumbBarAddButtons(HWND, UINT, LPTHUMBBUTTON)` and `ThumbBarUpdateButtons(HWND, UINT, LPTHUMBBUTTON)`
  - `THUMBBUTTON{ dwMask, iId, iBitmap, hIcon, szTip[260], dwFlags }`
  - `THBN_CLICKED` (0x1800) comes from `shobjidl.h`. `GetSystemMetricsForDpi` and `GetDpiForWindow` need Windows 10 1607+ headers, which `windows-latest` has.
  - `NOMINMAX` is already defined by the runner CMake.
  - The files must be UTF-8 **without** BOM. The existing `flutter_window.cpp` and `smtc_channel.cpp` start with a BOM; leave those as they are.
- [ ] Step 2: **Ask the user before pushing.** Once approved, push the branch, then trigger CI either by running `gh workflow run desktop.yml --ref feat/windows-thumbbar-media-buttons` (the workflow has `workflow_dispatch`) or by opening a PR to `main` if the user asks for one. Note that `paths-ignore: '**.md'` means a docs-only push will not trigger the workflow. This branch has C++ changes, so it will.
- [ ] Step 3: Watch the `Build Windows Bundle` job. Fix any compile errors, re-push, and repeat until it is green. Report the run URL.
- [ ] Step 4: Installer: **no changes to `windows/installer.iss`.** Its `[Files]` entry copies the whole `build\windows\x64\runner\Release\*`, and the icons live inside `lastwave_desktop.exe`. The setup `.exe` is produced only on `v*` tags. To get one without a release, build it locally on Windows:

```bash
flutter build windows --release
```

```bash
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" windows\installer.iss
```

  The output is `build\windows\installer\lastwave_1.0.0-dev_windows-x86_64-setup.exe`. Downloading the `build-windows` CI artifact (the raw Release folder) is also enough to test the exe without installing.

---

### Task 8: Manual acceptance test (Windows 10 and 11, done by the user)

Hand this checklist to the user in the final report. The agent cannot run it.

1. Install the setup exe and launch LastWave. Hover the taskbar icon: three buttons appear under the preview, all greyed out, with Play in the middle. There is no like/"+" button.
2. Play a song. The middle button becomes **Pause** (tooltip "Pause"), and Prev and Next become enabled.
3. Click Pause, then Play: playback pauses and resumes, and the icon swaps. The flyout stays open.
4. Click Next and Previous: the track changes, and the in-app player, the SMTC volume flyout and the thumb bar all agree.
5. Seek in the app, or let a track buffer: the button stays Pause while buffering.
6. Close to tray (the hide-to-tray preference), then restore from the tray. The buttons come back in the correct state.
7. Restart Explorer (Task Manager → Windows Explorer → Restart). The buttons come back.
8. Switch Settings → Personalization → Colors → "Choose your mode" between Dark and Light. The glyphs switch between white and dark and stay readable.
9. Move the window between monitors with different scaling, such as 100 % and 150 %. The icons stay crisp.
10. Hardware media keys and the volume-flyout controls still work (regression check for SMTC).
11. The tray icon menu items still work (regression check for `WM_COMMAND` routing).

---

## Out of scope / explicitly not doing

- A like/"+" button, as the user requested.
- A settings toggle to hide the buttons. This can be added later through a `thumbBar: bool` field in the `update` payload if wanted.
- Taskbar progress bar (`SetProgressValue`) or overlay icons.
- Localised tooltips (the app has no l10n today).
- Linux and macOS equivalents. MPRIS and Now Playing already cover them.
- Any change to `Clash-Projects/LastWave-Native` (the Android app).

## Files touched (summary)

| File | Change |
|---|---|
| `tool/thumbbar_icons.py` | new: icon generator (dev-only) |
| `windows/runner/resources/thumb_*_{white,dark}.ico` | new: 8 generated icons |
| `windows/runner/resource.h` | 8 new icon IDs |
| `windows/runner/Runner.rc` | 8 ICON entries |
| `windows/runner/transport_state.h` | new: shared state struct |
| `windows/runner/smtc_channel.{h,cpp}` | observer hook + `SendButton` |
| `windows/runner/thumb_bar.{h,cpp}` | new: thumbnail toolbar controller |
| `windows/runner/CMakeLists.txt` | add `thumb_bar.cpp` |
| `windows/runner/flutter_window.{h,cpp}` | construct, route messages, tear down |
| `lib/features/smtc/smtc_service.dart` | doc comment only |
