// = Requirements: freetype 2.5, libpng, libicu, libz, libzip2
// = How to compile:
//  % export CXXFLAGS=`pkg-config --cflags freetype2 libpng`
//  % export LDFLAGS=`pkg-config --libs freetype2 libpng`
//  % clang++ -o clfontpng -static $(CXXFLAGS) clfontpng.cc $(LDFLAGS) \
//    -licuuc -lz -lbz2
#include <cassert>
#include <cctype>
#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <stdio.h>
#include <unicode/utf.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

namespace
{

const int kBytesPerPixel = 4; // RGBA
const int kDefaultPixelSize = 128;
const int kSpaceWidth = kDefaultPixelSize / 2;

class Image
{
    uint32_t pos_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<uint8_t> bitmap_;

    Image(int w, int h)
        : width_(w), height_(h), bitmap_(w * h * kBytesPerPixel)
    {
    }

  public:
    static std::shared_ptr<Image> Create(int w, int h)
    {
        return std::shared_ptr<Image>(new Image(w, h));
    }

    uint8_t *Bitmap() { return &bitmap_[0]; }
    const uint32_t Width() const { return width_; }
    const uint32_t Height() const { return height_; }

    bool DrawBitmap(FT_GlyphSlot slot)
    {
        int pixel_mode = slot->bitmap.pixel_mode;
        if (pixel_mode == FT_PIXEL_MODE_BGRA)
            DrawColorBitmap(slot);
        else
            DrawNormalBitmap(slot);
        Advance(slot->advance.x >> 6);
        return true;
    }

    bool Output()
    {
        std::ofstream io("out.ppm", std::ios::binary);
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

        return true;
    }

  private:
    uint8_t *GetDrawPosition(int row)
    {
        uint32_t index = (row * width_ + pos_) * kBytesPerPixel;
        assert(index < bitmap_.size());
        return &bitmap_[index];
    }

    void DrawColorBitmap(FT_GlyphSlot slot)
    {
        uint8_t *src = slot->bitmap.buffer;
        // FIXME: Should use metrics for drawing. (e.g. calculate baseline)
        int yoffset = Height() - slot->bitmap.rows;
        for (uint32_t y = 0; y < slot->bitmap.rows; ++y)
        {
            uint8_t *dest = GetDrawPosition(y + yoffset);
            for (uint32_t x = 0; x < slot->bitmap.width; ++x)
            {
                uint8_t b = *src++, g = *src++, r = *src++, a = *src++;
                *dest++ = r;
                *dest++ = g;
                *dest++ = b;
                *dest++ = a;
            }
        }
    }
    void DrawNormalBitmap(FT_GlyphSlot slot)
    {
        uint8_t *src = slot->bitmap.buffer;
        // FIXME: Same as DrawColorBitmap()
        int yoffset = Height() - slot->bitmap.rows;
        for (uint32_t y = 0; y < slot->bitmap.rows; ++y)
        {
            uint8_t *dest = GetDrawPosition(y + yoffset);
            for (uint32_t x = 0; x < slot->bitmap.width; ++x)
            {
                *dest++ = 255 - *src;
                *dest++ = 255 - *src;
                *dest++ = 255 - *src;
                *dest++ = *src; // Alpha
                ++src;
            }
        }
    }
    void Advance(int dx) { pos_ += dx; }
};

struct Size
{
    uint32_t width = 0;
    uint32_t height = 0;
};

class FontBase
{
  protected:
    int load_flags_ = 0;
    FT_Face face_;
    FontBase(FT_Face face) : face_(face) {}
    ~FontBase()
    {
        FT_Done_Face(face_);
    }

  public:
    FT_GlyphSlot GetGlyph()
    {
        return face_->glyph;
    }

    Size GetGlyphSize()
    {
        Size size;
        size.width = (face_->glyph->advance.x >> 6);
        size.height = (face_->glyph->metrics.height >> 6);
        return size;
    }

    bool RenderGlyph(uint32_t codepoint)
    {
        auto glyph_index = FT_Get_Char_Index(face_, codepoint);
        if (glyph_index == 0)
        {
            return false;
        }

        auto error = FT_Load_Glyph(face_, glyph_index, load_flags_);
        if (error)
        {
            return false;
        }

        error = FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL);
        if (error)
        {
            return false;
        }

        return true;
    }
};

class EmojiFont : public FontBase
{
  public:
    EmojiFont(FT_Face face, int pixel_size)
        : FontBase(face)
    {
        load_flags_ |= FT_LOAD_COLOR;
        // search nearest font size
        int best_match = 0;
        int diff = std::abs(pixel_size - face_->available_sizes[0].width);
        for (int i = 1; i < face_->num_fixed_sizes; ++i)
        {
            auto current =
                std::abs(pixel_size - face_->available_sizes[i].width);
            if (current < diff)
            {
                best_match = i;
                diff = current;
            }
        }

        FT_Select_Size(face_, best_match);
    }
};

class NormalFont : public FontBase
{
    FT_Face face_;

  public:
    NormalFont(FT_Face face, int pixel_size)
        : FontBase(face)
    {
        FT_Set_Pixel_Sizes(face_, 0, pixel_size);
    }
};

static bool IsColorEmojiFont(FT_Face face)
{
    static const uint32_t tag = FT_MAKE_TAG('C', 'B', 'D', 'T');
    unsigned long length = 0;
    FT_Load_Sfnt_Table(face, tag, 0, nullptr, &length);
    return length != 0;
}

static std::shared_ptr<FontBase> CreateFont(FT_Library ft, const std::string &font_file)
{
    FT_Face face;
    auto error = FT_New_Face(ft, font_file.c_str(), 0, &face);
    if (error)
    {
        return nullptr;
    }

    if (IsColorEmojiFont(face))
    {
        if (face->num_fixed_sizes == 0)
        {
            return nullptr;
        }

        return std::shared_ptr<FontBase>(new EmojiFont(face, kDefaultPixelSize));
    }
    else
    {
        return std::shared_ptr<FontBase>(new NormalFont(face, kDefaultPixelSize));
    }
}

static std::vector<uint32_t> UTF8ToCodepoint(const std::string &_text)
{
    std::vector<uint32_t> codepoints;
    auto text = _text.c_str();
    int i = 0;
    auto length = _text.size();
    while (i < length)
    {
        int c;
        U8_NEXT(text, i, length, c);
        if (c < 0)
        {
            std::cerr << "Invalid input text" << std::endl;
            std::exit(2);
        }
        codepoints.push_back(c);
    }
    return codepoints;
}

} // namespace

class FT
{
  public:
    FT_Library ft_ = nullptr;

    FT()
    {
        FT_Init_FreeType(&ft_);
    }

    ~FT()
    {
        FT_Done_FreeType(ft_);
    }
};

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cout
            << "Usage: clfontpng font1.ttf [font2.ttf ...] text"
            << std::endl;
        std::exit(2);
    }

    FT ft;
    std::vector<std::shared_ptr<FontBase>> faces;
    for (int i = 1; i < argc - 1; ++i)
    {
        faces.push_back(CreateFont(ft.ft_, argv[i]));
    }

    auto codepoints = UTF8ToCodepoint("👧🏽");

    uint32_t width = 0;
    uint32_t height = 0;
    for (auto codepoint : codepoints)
    {
        static const uint32_t kSpace = 0x20;
        if (codepoint == kSpace)
        {
            width += kSpaceWidth;
        }
        else
        {
            for (auto &face : faces)
            {
                if (face->RenderGlyph(codepoint))
                {
                    auto size = face->GetGlyphSize();
                    width += size.width;
                    height = std::max(height, size.height);
                }
            }
        }
    }

    auto image = Image::Create(width, height);
    for (auto codepoint : codepoints)
    {
        for (auto &face : faces)
        {
            if (face->RenderGlyph(codepoint))
            {
                if (!image->DrawBitmap(face->GetGlyph()))
                {
                    return 3;
                }
            }
        }
        std::cerr << "Missing glyph for codepoint: " << codepoint << std::endl;
    }
    auto success = image->Output();

    return success ? 0 : 1;
}
