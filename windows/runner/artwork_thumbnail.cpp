#include "artwork_thumbnail.h"

#include <dwmapi.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <utility>

namespace lastwave {
namespace {

using Microsoft::WRL::ComPtr;

// Worker -> UI thread: lparam owns a heap Result.
constexpr UINT kMsgArtReady = WM_APP + 0x4C;
constexpr size_t kMaxDownloadBytes = 8u * 1024u * 1024u;
constexpr UINT kMaxDimension = 4096;
constexpr uint32_t kBackdrop = 0xFF121212;  // opaque near-black bars

struct Result {
  unsigned generation = 0;
  ArtworkThumbnail::Image image;
};

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int len = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (len <= 1) return {};
  std::wstring out(static_cast<size_t>(len - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
  return out;
}

bool IsHttpUrl(const std::string& url) {
  return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

class InetHandle {
 public:
  InetHandle() = default;
  explicit InetHandle(HINTERNET h) : h_(h) {}
  ~InetHandle() {
    if (h_ != nullptr) ::WinHttpCloseHandle(h_);
  }
  InetHandle(const InetHandle&) = delete;
  InetHandle& operator=(const InetHandle&) = delete;
  HINTERNET get() const { return h_; }
  explicit operator bool() const { return h_ != nullptr; }

 private:
  HINTERNET h_ = nullptr;
};

bool Download(const std::string& url, std::vector<uint8_t>* out) {
  const std::wstring wurl = Widen(url);
  if (wurl.empty()) return false;

  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwSchemeLength = static_cast<DWORD>(-1);
  uc.dwHostNameLength = static_cast<DWORD>(-1);
  uc.dwUrlPathLength = static_cast<DWORD>(-1);
  uc.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0,
                         &uc)) {
    return false;
  }
  const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  // Path and query are contiguous in the source string.
  const std::wstring path(uc.lpszUrlPath,
                          uc.dwUrlPathLength + uc.dwExtraInfoLength);
  const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

  InetHandle session(::WinHttpOpen(L"LastWave",
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) return false;
  ::WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000);
  InetHandle connection(
      ::WinHttpConnect(session.get(), host.c_str(), uc.nPort, 0));
  if (!connection) return false;
  InetHandle request(::WinHttpOpenRequest(
      connection.get(), L"GET", path.empty() ? L"/" : path.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      secure ? WINHTTP_FLAG_SECURE : 0));
  if (!request) return false;
  if (!::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request.get(), nullptr)) {
    return false;
  }
  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!::WinHttpQueryHeaders(
          request.get(),
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
          WINHTTP_NO_HEADER_INDEX) ||
      status != 200) {
    return false;
  }
  for (;;) {
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request.get(), &available)) return false;
    if (available == 0) break;
    if (out->size() + available > kMaxDownloadBytes) return false;
    const size_t at = out->size();
    out->resize(at + available);
    DWORD read = 0;
    if (!::WinHttpReadData(request.get(), out->data() + at, available,
                           &read)) {
      return false;
    }
    out->resize(at + read);
  }
  return !out->empty();
}

bool Decode(const std::vector<uint8_t>& bytes,
            ArtworkThumbnail::Image* image) {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
      !factory) {
    return false;
  }
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(
          const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())))) {
    return false;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(
          stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
    return false;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) return false;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return false;
  }
  UINT w = 0;
  UINT h = 0;
  if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 ||
      w > kMaxDimension || h > kMaxDimension) {
    return false;
  }
  image->width = static_cast<int>(w);
  image->height = static_cast<int>(h);
  image->bgra.resize(static_cast<size_t>(w) * h * 4);
  return SUCCEEDED(converter->CopyPixels(
      nullptr, w * 4, static_cast<UINT>(image->bgra.size()),
      image->bgra.data()));
}

void Worker(HWND hwnd, std::shared_ptr<void> keepalive,
            const std::atomic<bool>* alive, unsigned generation,
            std::string url) {
  const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  auto result = std::make_unique<Result>();
  result->generation = generation;
  std::vector<uint8_t> bytes;
  const bool ok = alive->load() && Download(url, &bytes) &&
                  alive->load() && Decode(bytes, &result->image);
  if (SUCCEEDED(co)) ::CoUninitialize();
  if (!ok || !alive->load()) return;
  // On success the UI thread owns the Result; if the window is gone the
  // post fails and we free it here.
  if (::PostMessageW(hwnd, kMsgArtReady, 0,
                     reinterpret_cast<LPARAM>(result.get()))) {
    result.release();
  }
}

}  // namespace

ArtworkThumbnail::ArtworkThumbnail(HWND hwnd)
    : hwnd_(hwnd), shared_(std::make_shared<Shared>()) {}

ArtworkThumbnail::~ArtworkThumbnail() {
  shared_->alive.store(false);
  // Detached workers hold shared_ via keepalive, so the flag outlives us.
  // Only restore the preview while the window still exists.
  if (iconic_ && ::IsWindow(hwnd_)) SetIconic(false);
}

void ArtworkThumbnail::SetUrl(const std::string& url) {
  if (url == url_) return;
  url_ = url;
  ++generation_;
  image_.reset();
  SetIconic(false);  // never show the previous song's poster
  if (!IsHttpUrl(url)) return;
  std::thread(Worker, hwnd_, std::static_pointer_cast<void>(shared_),
              &shared_->alive, generation_, url)
      .detach();
}

void ArtworkThumbnail::SetIconic(bool on) {
  if (on == iconic_) return;
  iconic_ = on;
  const BOOL value = on ? TRUE : FALSE;
  if (on) {
    ::DwmSetWindowAttribute(hwnd_, DWMWA_HAS_ICONIC_BITMAP, &value,
                            sizeof(value));
    ::DwmSetWindowAttribute(hwnd_, DWMWA_FORCE_ICONIC_REPRESENTATION, &value,
                            sizeof(value));
  } else {
    ::DwmSetWindowAttribute(hwnd_, DWMWA_FORCE_ICONIC_REPRESENTATION, &value,
                            sizeof(value));
    ::DwmSetWindowAttribute(hwnd_, DWMWA_HAS_ICONIC_BITMAP, &value,
                            sizeof(value));
  }
}

std::optional<LRESULT> ArtworkThumbnail::HandleMessage(UINT message,
                                                       WPARAM /*wparam*/,
                                                       LPARAM lparam) {
  switch (message) {
    case kMsgArtReady: {
      std::unique_ptr<Result> result(reinterpret_cast<Result*>(lparam));
      if (result && result->generation == generation_) {
        image_ = std::make_unique<Image>(std::move(result->image));
        SetIconic(true);
        ::DwmInvalidateIconicBitmaps(hwnd_);
      }
      return 0;
    }
    case WM_DWMSENDICONICTHUMBNAIL: {
      if (!image_) return std::nullopt;
      const int max_w = static_cast<int>(HIWORD(lparam));
      const int max_h = static_cast<int>(LOWORD(lparam));
      if (HBITMAP bitmap = Render(max_w, max_h)) {
        ::DwmSetIconicThumbnail(hwnd_, bitmap, 0);
        ::DeleteObject(bitmap);
      }
      return 0;
    }
    case WM_DWMSENDICONICLIVEPREVIEWBITMAP: {
      if (!image_) return std::nullopt;
      RECT rc{};
      ::GetClientRect(hwnd_, &rc);
      if (HBITMAP bitmap = Render(rc.right - rc.left, rc.bottom - rc.top)) {
        ::DwmSetIconicLivePreviewBitmap(hwnd_, bitmap, nullptr, 0);
        ::DeleteObject(bitmap);
      }
      return 0;
    }
    default:
      return std::nullopt;
  }
}

HBITMAP ArtworkThumbnail::Render(int canvas_w, int canvas_h) const {
  if (!image_ || canvas_w <= 0 || canvas_h <= 0) return nullptr;

  BITMAPINFO dst_info{};
  dst_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  dst_info.bmiHeader.biWidth = canvas_w;
  dst_info.bmiHeader.biHeight = -canvas_h;  // top-down
  dst_info.bmiHeader.biPlanes = 1;
  dst_info.bmiHeader.biBitCount = 32;
  dst_info.bmiHeader.biCompression = BI_RGB;
  void* dst_bits = nullptr;
  HBITMAP dst = ::CreateDIBSection(nullptr, &dst_info, DIB_RGB_COLORS,
                                   &dst_bits, nullptr, 0);
  if (dst == nullptr || dst_bits == nullptr) {
    if (dst != nullptr) ::DeleteObject(dst);
    return nullptr;
  }
  std::fill_n(static_cast<uint32_t*>(dst_bits),
              static_cast<size_t>(canvas_w) * canvas_h, kBackdrop);

  BITMAPINFO src_info = dst_info;
  src_info.bmiHeader.biWidth = image_->width;
  src_info.bmiHeader.biHeight = -image_->height;
  void* src_bits = nullptr;
  HBITMAP src = ::CreateDIBSection(nullptr, &src_info, DIB_RGB_COLORS,
                                   &src_bits, nullptr, 0);
  if (src == nullptr || src_bits == nullptr) {
    if (src != nullptr) ::DeleteObject(src);
    ::DeleteObject(dst);
    return nullptr;
  }
  std::memcpy(src_bits, image_->bgra.data(), image_->bgra.size());

  // Fit the poster inside the canvas, centred, aspect preserved.
  const double scale =
      std::min(static_cast<double>(canvas_w) / image_->width,
               static_cast<double>(canvas_h) / image_->height);
  const int draw_w = std::max(1, static_cast<int>(image_->width * scale));
  const int draw_h = std::max(1, static_cast<int>(image_->height * scale));
  const int x = (canvas_w - draw_w) / 2;
  const int y = (canvas_h - draw_h) / 2;

  HDC dst_dc = ::CreateCompatibleDC(nullptr);
  HDC src_dc = ::CreateCompatibleDC(nullptr);
  if (dst_dc != nullptr && src_dc != nullptr) {
    HGDIOBJ old_dst = ::SelectObject(dst_dc, dst);
    HGDIOBJ old_src = ::SelectObject(src_dc, src);
    ::SetStretchBltMode(dst_dc, HALFTONE);
    ::SetBrushOrgEx(dst_dc, 0, 0, nullptr);
    ::StretchBlt(dst_dc, x, y, draw_w, draw_h, src_dc, 0, 0, image_->width,
                 image_->height, SRCCOPY);
    ::SelectObject(dst_dc, old_dst);
    ::SelectObject(src_dc, old_src);
  }
  if (src_dc != nullptr) ::DeleteDC(src_dc);
  if (dst_dc != nullptr) ::DeleteDC(dst_dc);
  ::DeleteObject(src);
  return dst;
}

}  // namespace lastwave
