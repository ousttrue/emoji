// = Requirements: freetype 2.5
#include <cassert>
#include <cctype>
#include <iostream>
#include <sstream>
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

const int kBytesPerPixel = 4; // RGBA
const int kDefaultPixelSize = 128;

class Image
{
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

    virtual int Draw(const std::shared_ptr<Image> &image, int x) = 0;
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

    int Draw(const std::shared_ptr<Image> &image, int x) override
    {
        auto slot = face_->glyph;
        int pixel_mode = slot->bitmap.pixel_mode;
        assert(pixel_mode == FT_PIXEL_MODE_BGRA);
        auto src = slot->bitmap.buffer;
        // FIXME: Should use metrics for drawing. (e.g. calculate baseline)
        int yoffset = image->Height() - slot->bitmap.rows;
        for (uint32_t y = 0; y < slot->bitmap.rows; ++y)
        {
            auto dest = image->GetDrawPosition(x, y + yoffset);
            for (uint32_t x = 0; x < slot->bitmap.width; ++x)
            {
                uint8_t b = *src++, g = *src++, r = *src++, a = *src++;
                *dest++ = r;
                *dest++ = g;
                *dest++ = b;
                *dest++ = a;
            }
        }
        return slot->advance.x >> 6;
    }
};

class NormalFont : public FontBase
{
  public:
    NormalFont(FT_Face face, int pixel_size)
        : FontBase(face)
    {
        FT_Set_Pixel_Sizes(face_, 0, pixel_size);
    }

    int Draw(const std::shared_ptr<Image> &image, int x) override
    {
        auto slot = face_->glyph;
        int pixel_mode = slot->bitmap.pixel_mode;
        assert(pixel_mode != FT_PIXEL_MODE_BGRA);
        auto src = slot->bitmap.buffer;
        // FIXME: Same as DrawColorBitmap()
        int yoffset = image->Height() - slot->bitmap.rows;
        for (uint32_t y = 0; y < slot->bitmap.rows; ++y)
        {
            uint8_t *dest = image->GetDrawPosition(x, y + yoffset);
            for (uint32_t x = 0; x < slot->bitmap.width; ++x)
            {
                *dest++ = 255 - *src;
                *dest++ = 255 - *src;
                *dest++ = 255 - *src;
                *dest++ = *src; // Alpha
                ++src;
            }
        }
        return slot->advance.x >> 6;
    }
};

static bool IsColorEmojiFont(FT_Face face)
{
    static const uint32_t tag = FT_MAKE_TAG('C', 'B', 'D', 'T');
    unsigned long length = 0;
    FT_Load_Sfnt_Table(face, tag, 0, nullptr, &length);
    return length != 0;
}

static std::shared_ptr<FontBase> CreateFont(FT_Library ft, const std::string &font_file, int pixel_size)
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

        return std::shared_ptr<FontBase>(new EmojiFont(face, pixel_size));
    }
    else
    {
        return std::shared_ptr<FontBase>(new NormalFont(face, pixel_size));
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

class FT
{
    FT_Library ft_ = nullptr;
    std::vector<std::shared_ptr<FontBase>> faces_;

  public:
    FT() { FT_Init_FreeType(&ft_); }
    ~FT()
    {
        faces_.clear();
        FT_Done_FreeType(ft_);
    }

    void AddFont(const std::string &fontfile, int pixel_size)
    {
        faces_.push_back(CreateFont(ft_, fontfile, pixel_size));
    }

    std::shared_ptr<Image> RenderToImage(uint32_t codepoint)
    {
        return RenderToImage(&codepoint, 1);
    }

    std::shared_ptr<Image> RenderToImage(const uint32_t *codepoints, size_t count)
    {
        static const uint32_t kSpace = 0x20;
        const int kSpaceWidth = kDefaultPixelSize / 2;

        uint32_t width = 0;
        uint32_t height = 0;
        for (int i = 0; i < count; ++i)
        {
            auto codepoint = codepoints[i];
            if (codepoint == kSpace)
            {
                width += kSpaceWidth;
            }
            else
            {
                for (auto &face : faces_)
                {
                    // try all fonts
                    if (face->RenderGlyph(codepoint))
                    {
                        auto size = face->GetGlyphSize();
                        width += size.width;
                        height = std::max(height, size.height);
                        break;
                    }
                }
            }
        }

        auto image = std::make_shared<Image>(width, height);
        int x = 0;
        for (int i = 0; i < count; ++i)
        {
            auto codepoint = codepoints[i];
            for (auto &face : faces_)
            {
                // try all fonts
                if (face->RenderGlyph(codepoint))
                {
                    x += face->Draw(image, x);
                    break;
                }
            }
        }

        return image;
    }
};

static uint32_t HexToInt(const std::string &src)
{
    std::stringstream ss;
    ss << std::hex << src;
    uint32_t codepoint;
    ss >> codepoint;
    return codepoint;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cout
            << "Usage: emoji font1.ttf [font2.ttf ...] codepoint" << std::endl
            << "Example: emoji font1.ttf [font2.ttf ...] 1F600" << std::endl;
        std::exit(2);
    }

    FT ft;
    int i = 1;
    for (; i < argc - 1; ++i)
    {
        ft.AddFont(argv[i], kDefaultPixelSize);
    }

    auto image = ft.RenderToImage(HexToInt(argv[i]));
    image->WritePPM("out.ppm");

    return 0;
}
