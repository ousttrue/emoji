#pragma once
#include <cctype>
#include <vector>
#include <assert.h>
#include <fstream>

class Image
{
    const int kBytesPerPixel = 4; // RGBA
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<uint8_t> bitmap_;

  public:
    Image(int w, int h)
        : width_(w), height_(h), bitmap_(w * h * kBytesPerPixel)
    {
    }

    uint8_t *Bitmap() { return &bitmap_[0]; }
    const uint32_t Width() const { return width_; }
    const uint32_t Height() const { return height_; }

    uint8_t *GetDrawPosition(int x, int row)
    {
        uint32_t index = (row * width_ + x) * kBytesPerPixel;
        assert(index < bitmap_.size());
        return &bitmap_[index];
    }

    void WritePPM(const std::string &path)
    {
        std::ofstream io(path, std::ios::binary);
        io << "P6\n";
        io << Width() << " " << Height() << "\n";
        io << "255\n";

        auto p = (const char *)Bitmap();
        for (uint32_t y = 0; y < Height(); ++y)
        {
            for (uint32_t x = 0; x < Width(); ++x, p += 4)
            {
                io.write(p, 3);
            }
        }
    }
};
