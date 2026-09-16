#pragma once
// A CPU bitmap drawn with GDI: the avatar window's text panel is rendered
// here (system fonts, so Japanese works through font linking) and the pixels
// are handed to the GPU as a raw buffer. Rows are top-down, 32-bit BGRA.
#include <windows.h>

#include <cstdint>
#include <string>

namespace aii {

struct Color {
  uint8_t r = 0, g = 0, b = 0;
};

class GdiCanvas {
 public:
  GdiCanvas(int width, int height);
  ~GdiCanvas();
  GdiCanvas(const GdiCanvas&) = delete;
  GdiCanvas& operator=(const GdiCanvas&) = delete;

  int width() const { return w_; }
  int height() const { return h_; }
  const uint8_t* pixels() const { return static_cast<const uint8_t*>(bits_); }
  size_t bytes() const { return static_cast<size_t>(w_) * h_ * 4; }

  void clear(Color c);
  void fill_rect(int x, int y, int w, int h, Color c);
  void frame_rect(int x, int y, int w, int h, Color c);  // one-pixel outline

  // Word-wrapped UTF-8 text inside the rect; DT_* flags add alignment etc.
  // Returns the height the text used.
  int text(int x, int y, int w, int h, const std::string& utf8, int px, Color c, bool bold = false,
           unsigned flags = 0);
  // Height the text would use when wrapped to `w`.
  int measure(int w, const std::string& utf8, int px, bool bold = false);

 private:
  void select_font(int px, bool bold);

  HDC dc_ = nullptr;
  HBITMAP bmp_ = nullptr;
  HGDIOBJ old_bmp_ = nullptr;
  void* bits_ = nullptr;
  int w_ = 0, h_ = 0;
  HFONT font_ = nullptr;
  int font_px_ = 0;
  bool font_bold_ = false;
};

}  // namespace aii
