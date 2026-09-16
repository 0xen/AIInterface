#include "ui/gdi_canvas.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace aii {
namespace {

std::wstring wide(const std::string& utf8) {
  if (utf8.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
  return w;
}

uint32_t pack(Color c) { return (0xFFu << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b; }

}  // namespace

GdiCanvas::GdiCanvas(int width, int height) : w_(width), h_(height) {
  dc_ = CreateCompatibleDC(nullptr);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
  old_bmp_ = SelectObject(dc_, bmp_);
  SetBkMode(dc_, TRANSPARENT);
  clear({0, 0, 0});
}

GdiCanvas::~GdiCanvas() {
  if (dc_ && old_bmp_) SelectObject(dc_, old_bmp_);
  if (font_) DeleteObject(font_);
  if (bmp_) DeleteObject(bmp_);
  if (dc_) DeleteDC(dc_);
}

void GdiCanvas::clear(Color c) { fill_rect(0, 0, w_, h_, c); }

void GdiCanvas::fill_rect(int x, int y, int w, int h, Color c) {
  const int x0 = std::max(0, x), y0 = std::max(0, y);
  const int x1 = std::min(w_, x + w), y1 = std::min(h_, y + h);
  if (x0 >= x1 || y0 >= y1) return;
  const uint32_t v = pack(c);
  auto* px = static_cast<uint32_t*>(bits_);
  for (int yy = y0; yy < y1; ++yy) {
    std::fill(px + (size_t)yy * w_ + x0, px + (size_t)yy * w_ + x1, v);
  }
}

void GdiCanvas::frame_rect(int x, int y, int w, int h, Color c) {
  fill_rect(x, y, w, 1, c);
  fill_rect(x, y + h - 1, w, 1, c);
  fill_rect(x, y, 1, h, c);
  fill_rect(x + w - 1, y, 1, h, c);
}

void GdiCanvas::select_font(int px, bool bold) {
  if (font_ && font_px_ == px && font_bold_ == bold) return;
  if (font_) DeleteObject(font_);
  // Segoe UI; Japanese glyphs arrive through GDI font linking (Yu Gothic UI).
  font_ = CreateFontW(-px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                      L"Segoe UI");
  SelectObject(dc_, font_);
  font_px_ = px;
  font_bold_ = bold;
}

int GdiCanvas::text(int x, int y, int w, int h, const std::string& utf8, int px, Color c, bool bold,
                    unsigned flags) {
  select_font(px, bold);
  SetTextColor(dc_, RGB(c.r, c.g, c.b));
  RECT r{x, y, x + w, y + h};
  std::wstring ws = wide(utf8);
  return DrawTextW(dc_, ws.c_str(), (int)ws.size(), &r, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | flags);
}

int GdiCanvas::measure(int w, const std::string& utf8, int px, bool bold) {
  select_font(px, bold);
  RECT r{0, 0, w, 0};
  std::wstring ws = wide(utf8);
  DrawTextW(dc_, ws.c_str(), (int)ws.size(), &r, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_CALCRECT);
  return r.bottom;
}

}  // namespace aii
