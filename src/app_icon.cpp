#include "app_icon.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ncw
{
namespace
{
std::uint32_t Argb(unsigned char a, unsigned char r, unsigned char g, unsigned char b)
{
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
}

bool PointInRoundedRect(int x, int y, int width, int height, int radius)
{
    if (x < 0 || y < 0 || x >= width || y >= height)
    {
        return false;
    }
    const int left = radius;
    const int right = width - radius - 1;
    const int top = radius;
    const int bottom = height - radius - 1;
    if ((x >= left && x <= right) || (y >= top && y <= bottom))
    {
        return true;
    }

    const int cx = x < left ? left : right;
    const int cy = y < top ? top : bottom;
    const int dx = x - cx;
    const int dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

void FillRectPixels(std::uint32_t *pixels, int width, int height, int left, int top, int right, int bottom,
                    std::uint32_t color)
{
    left = std::max(0, left);
    top = std::max(0, top);
    right = std::min(width, right);
    bottom = std::min(height, bottom);
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            pixels[y * width + x] = color;
        }
    }
}

void DrawDiagonalPixels(std::uint32_t *pixels, int width, int height, int x1, int y1, int x2, int y2, int thickness,
                        std::uint32_t color)
{
    if (y2 <= y1)
    {
        return;
    }
    for (int y = y1; y <= y2; ++y)
    {
        const double t = static_cast<double>(y - y1) / static_cast<double>(y2 - y1);
        const int center_x = static_cast<int>(x1 + (x2 - x1) * t + 0.5);
        FillRectPixels(pixels, width, height, center_x - thickness / 2, y, center_x + (thickness + 1) / 2, y + 1,
                       color);
    }
}
}

HICON CreateGeneratedAppIcon(int width, int height)
{
    width = std::max(16, width);
    height = std::max(16, height);

    BITMAPINFO bitmap_info = {};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void *raw_pixels = nullptr;
    HBITMAP color_bitmap = CreateDIBSection(nullptr, &bitmap_info, DIB_RGB_COLORS, &raw_pixels, nullptr, 0);
    if (color_bitmap == nullptr)
    {
        return nullptr;
    }
    if (raw_pixels == nullptr)
    {
        DeleteObject(color_bitmap);
        return nullptr;
    }

    auto *pixels = static_cast<std::uint32_t *>(raw_pixels);
    std::fill(pixels, pixels + static_cast<std::size_t>(width * height), Argb(0, 0, 0, 0));

    const int min_side = std::min(width, height);
    const int margin = std::max(1, min_side / 10);
    const int radius = std::max(3, min_side / 5);
    const std::uint32_t background = Argb(255, 17, 24, 39);
    const std::uint32_t text = Argb(255, 248, 250, 252);
    const std::uint32_t accent = Argb(255, 20, 184, 166);

    for (int y = margin; y < height - margin; ++y)
    {
        for (int x = margin; x < width - margin; ++x)
        {
            if (PointInRoundedRect(x - margin, y - margin, width - margin * 2, height - margin * 2, radius))
            {
                pixels[y * width + x] = background;
            }
        }
    }

    const int clip_left = width * 31 / 100;
    const int clip_right = width * 69 / 100;
    const int clip_top = height * 13 / 100;
    const int clip_bottom = std::max(clip_top + 2, height * 25 / 100);
    FillRectPixels(pixels, width, height, clip_left, clip_top, clip_right, clip_bottom, accent);

    const int stroke = std::max(2, min_side / 8);
    const int n_top = height * 31 / 100;
    const int n_bottom = height * 77 / 100;
    const int left_bar = width * 30 / 100;
    const int right_bar = width * 66 / 100;
    FillRectPixels(pixels, width, height, left_bar, n_top, left_bar + stroke, n_bottom, text);
    FillRectPixels(pixels, width, height, right_bar, n_top, right_bar + stroke, n_bottom, text);
    DrawDiagonalPixels(pixels, width, height, left_bar + stroke, n_top, right_bar, n_bottom - 1, stroke, text);

    const int mask_stride = ((width + 15) / 16) * 2;
    std::vector<unsigned char> mask_bits(static_cast<std::size_t>(mask_stride * height), 0);
    HBITMAP mask_bitmap = CreateBitmap(width, height, 1, 1, mask_bits.data());
    if (mask_bitmap == nullptr)
    {
        DeleteObject(color_bitmap);
        return nullptr;
    }

    ICONINFO icon_info = {};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color_bitmap;
    icon_info.hbmMask = mask_bitmap;
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(color_bitmap);
    DeleteObject(mask_bitmap);
    return icon;
}
}
