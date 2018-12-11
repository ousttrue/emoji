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

#include <unicode/umachine.h>
#include <unicode/utf.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#define PNG_SKIP_SETJMP_CHECK
#include <png.h>

namespace
{

const char *kDefaultOutputFile = "out.png";
const int kBytesPerPixel = 4; // RGBA
const int kDefaultPixelSize = 128;
const int kSpaceWidth = kDefaultPixelSize / 2;

FT_Library gFtLibrary;

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
        for (int y = 0; y < Height(); ++y)
        {
            for (int x = 0; x < Width(); ++x, p += 4)
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
        for (int y = 0; y < slot->bitmap.rows; ++y)
        {
            uint8_t *dest = GetDrawPosition(y + yoffset);
            for (int x = 0; x < slot->bitmap.width; ++x)
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
        for (int y = 0; y < slot->bitmap.rows; ++y)
        {
            uint8_t *dest = GetDrawPosition(y + yoffset);
            for (int x = 0; x < slot->bitmap.width; ++x)
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

class FreeTypeFace
{
    std::string font_file_;
    int pixel_size = kDefaultPixelSize;
    int load_flags = 0;
    FT_Render_Mode render_mode = FT_RENDER_MODE_NORMAL;
    FT_Face face_ = nullptr;
    int error_;

  public:
    FreeTypeFace(const std::string &font_file)
        : font_file_(font_file)
    {
        error_ = FT_New_Face(gFtLibrary, font_file_.c_str(), 0, &face_);
        if (error_)
        {
            return;
        }
        if (IsColorEmojiFont())
            SetupColorFont();
        else
            SetupNormalFont();
    }
    ~FreeTypeFace()
    {
        if (face_)
            FT_Done_Face(face_);
    }
    bool CalculateBox(uint32_t codepoint, uint32_t &width, uint32_t &height)
    {
        if (!RenderGlyph(codepoint))
            return false;
        width += (face_->glyph->advance.x >> 6);
        height = std::max(
            height, static_cast<uint32_t>(face_->glyph->metrics.height >> 6));
        return true;
    }

    bool DrawCodepoint(const std::shared_ptr<Image> &context, uint32_t codepoint)
    {
        if (!RenderGlyph(codepoint))
            return false;
        //printf("U+%08X -> %s\n", codepoint, font_file_.c_str());
        return context->DrawBitmap(face_->glyph);
    }

    int Error() const { return error_; }

  private:
    FreeTypeFace(const FreeTypeFace &) = delete;
    FreeTypeFace &operator=(const FreeTypeFace &) = delete;

    bool RenderGlyph(uint32_t codepoint)
    {
        if (!face_)
            return false;
        uint32_t glyph_index = FT_Get_Char_Index(face_, codepoint);
        if (glyph_index == 0)
            return false;
        error_ = FT_Load_Glyph(face_, glyph_index, load_flags);
        if (error_)
            return false;
        error_ = FT_Render_Glyph(face_->glyph, render_mode);
        if (error_)
            return false;
        return true;
    }
    bool IsColorEmojiFont()
    {
        static const uint32_t tag = FT_MAKE_TAG('C', 'B', 'D', 'T');
        unsigned long length = 0;
        FT_Load_Sfnt_Table(face_, tag, 0, nullptr, &length);
        if (length)
        {
            std::cout << font_file_ << " is color font" << std::endl;
            return true;
        }
        return false;
    }
    void SetupNormalFont()
    {
        error_ = FT_Set_Pixel_Sizes(face_, 0, pixel_size);
    }
    void SetupColorFont()
    {
        load_flags |= FT_LOAD_COLOR;

        if (face_->num_fixed_sizes == 0)
            return;
        int best_match = 0;
        int diff = std::abs(pixel_size - face_->available_sizes[0].width);
        for (int i = 1; i < face_->num_fixed_sizes; ++i)
        {
            int ndiff =
                std::abs(pixel_size - face_->available_sizes[i].width);
            if (ndiff < diff)
            {
                best_match = i;
                diff = ndiff;
            }
        }
        error_ = FT_Select_Size(face_, best_match);
    }
};

class FontList
{
    typedef std::vector<std::unique_ptr<FreeTypeFace>> FaceList;

  public:
    FontList() {}

    void AddFont(const std::string &font_file)
    {
        auto face = std::unique_ptr<FreeTypeFace>(new FreeTypeFace(font_file));
        face_list_.push_back(std::move(face));
    }
    void CalculateBox(uint32_t codepoint, uint32_t &width, uint32_t &height)
    {
        static const uint32_t kSpace = 0x20;
        if (codepoint == kSpace)
        {
            width += kSpaceWidth;
        }
        else
        {
            for (auto &face : face_list_)
            {
                if (face->CalculateBox(codepoint, width, height))
                    return;
            }
        }
    }
    void DrawCodepoint(std::shared_ptr<Image> &context, uint32_t codepoint)
    {
        for (auto &face : face_list_)
        {
            if (face->DrawCodepoint(context, codepoint))
                return;
        }
        std::cerr << "Missing glyph for codepoint: " << codepoint << std::endl;
    }

  private:
    FontList(const FontList &) = delete;
    FontList &operator=(const FontList &) = delete;
    FaceList face_list_;
};

class App
{
  public:
    void AddFont(const std::string &font_file) { font_list_.AddFont(font_file); }
    bool SetText(const char *text) { return UTF8ToCodepoint(text); }
    bool Execute()
    {
        draw_context_ = CalculateImageSize();
        Draw();
        return draw_context_->Output();
    }

  private:
    bool UTF8ToCodepoint(const std::string &_text)
    {
        auto text = _text.c_str();
        int32_t i = 0, length = strlen(text), c;
        while (i < length)
        {
            U8_NEXT(text, i, length, c);
            if (c < 0)
            {
                std::cerr << "Invalid input text" << std::endl;
                return false;
            }
            //codepoints_.push_back(0x231B);
            codepoints_.push_back(c);
        }
        return true;
    }
    std::shared_ptr<Image> CalculateImageSize()
    {
        uint32_t width = 0, height = 0;
        for (auto c : codepoints_)
            font_list_.CalculateBox(c, width, height);
        printf("width: %u, height: %u\n", width, height);
        return Image::Create(width, height);
    }
    void Draw()
    {
        for (auto c : codepoints_)
            font_list_.DrawCodepoint(draw_context_, c);
    }

    std::vector<uint32_t> codepoints_;
    FontList font_list_;
    std::shared_ptr<Image> draw_context_;
};

bool Init()
{
    int error = FT_Init_FreeType(&gFtLibrary);
    if (error)
    {
        std::cerr << "Failed to initialize freetype" << std::endl;
        return error;
    }
    return error == 0;
}

void Finish()
{
    FT_Done_FreeType(gFtLibrary);
}

void Usage()
{
    std::cout
        << "Usage: clfontpng font1.ttf [font2.ttf ...] text"
        << std::endl;
    std::exit(1);
}

bool ParseArgs(App &app, int argc, char **argv)
{
    if (argc < 2)
        return false;
    for (int i = 1; i < argc - 1; ++i)
        app.AddFont(argv[i]);
    //return app.SetText(argv[argc - 1]);
    //return app.SetText("🌊");
    return app.SetText("👧🏽");
}

bool Start(int argc, char **argv)
{
    App app;
    if (!ParseArgs(app, argc, argv))
        Usage();
    return app.Execute();
}

} // namespace

int main(int argc, char **argv)
{
    if (!Init())
        std::exit(1);
    bool success = Start(argc, argv);
    Finish();
    return success ? 0 : 1;
}
