#ifndef RUNNER_ARTWORK_THUMBNAIL_H_
#define RUNNER_ARTWORK_THUMBNAIL_H_

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lastwave {

// Replaces the taskbar hover preview of the main window with the current
// song's poster (album art) using DWM's custom "iconic" thumbnail API.
// The art is downloaded (WinHTTP) and decoded (WIC) on a worker thread;
// the UI thread only renders and hands bitmaps to DWM. When there is no
// art (or a download fails) the normal live window preview is restored.
// Best-effort: every failure falls back to the live preview, never errors.
class ArtworkThumbnail {
 public:
  explicit ArtworkThumbnail(HWND hwnd);
  ~ArtworkThumbnail();
  ArtworkThumbnail(const ArtworkThumbnail&) = delete;
  ArtworkThumbnail& operator=(const ArtworkThumbnail&) = delete;

  // Current track's artwork URL; empty (or non-http) restores the live
  // preview. Cheap to call repeatedly with the same value.
  void SetUrl(const std::string& url);

  // Consumes the worker completion and the DWM thumbnail requests.
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam,
                                       LPARAM lparam);

  struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;  // premultiplied BGRA, width*height*4
  };

 private:
  struct Shared {
    std::atomic<bool> alive{true};
  };

  void SetIconic(bool on);
  HBITMAP Render(int canvas_w, int canvas_h) const;

  HWND hwnd_;
  std::string url_;
  unsigned generation_ = 0;
  std::shared_ptr<Shared> shared_;
  std::unique_ptr<Image> image_;
  bool iconic_ = false;
};

}  // namespace lastwave

#endif  // RUNNER_ARTWORK_THUMBNAIL_H_
