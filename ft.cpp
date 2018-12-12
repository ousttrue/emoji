#include "ft.h"
#include "image.h"
#include <cctype>
#include <algorithm>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

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

class FTImpl
{
  public:
    FT_Library ft_ = nullptr;
    FTImpl() { FT_Init_FreeType(&ft_); }
    ~FTImpl()
    {
        FT_Done_FreeType(ft_);
    }
};

FT::FT()
    : impl_(new FTImpl)
{
}

FT::~FT()
{
    faces_.clear();
    delete impl_;
}

void FT::AddFont(const std::string &fontfile, int pixel_size)
{
    faces_.push_back(CreateFont(impl_->ft_, fontfile, pixel_size));
}

std::shared_ptr<Image> FT::RenderToImage(const uint32_t *codepoints, size_t count)
{
    //static const uint32_t kSpace = 0x20;
    //const int kSpaceWidth = kDefaultPixelSize / 2;

    uint32_t width = 0;
    uint32_t height = 0;
    for (int i = 0; i < count; ++i)
    {
        auto codepoint = codepoints[i];
        /*
        if (codepoint == kSpace)
        {
            //width += kSpaceWidth;
        }
        else
        */
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
