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

// Only support horizontal direction.
class DrawContext
{
  public:
    DrawContext()
        : pos_(0), width_(0), height_(0) {}
    uint8_t *Bitmap() { return &bitmap_[0]; }
    const uint32_t Width() const { return width_; }
    const uint32_t Height() const { return height_; }
    void SetSize(int width, int height)
    {
        width_ = width;
        height_ = height;
        int size = width * height * kBytesPerPixel;
        bitmap_.resize(size);
        bitmap_.assign(size, 0x00);
    }
    void Advance(int dx) { pos_ += dx; }
    uint8_t *GetDrawPosition(int row)
    {
        uint32_t index = (row * width_ + pos_) * kBytesPerPixel;
        assert(index < bitmap_.size());
        return &bitmap_[index];
    }

  private:
    DrawContext(const DrawContext &) = delete;
    DrawContext &operator=(const DrawContext &) = delete;

    uint32_t pos_;
    uint32_t width_;
    uint32_t height_;
    std::vector<uint8_t> bitmap_;
};

struct FaceOptions
{
    int pixel_size;
    int load_flags;
    FT_Render_Mode render_mode;
    FaceOptions()
        : pixel_size(kDefaultPixelSize), load_flags(0), render_mode(FT_RENDER_MODE_NORMAL) {}
};

class FreeTypeFace
{
  public:
    FreeTypeFace(const std::string &font_file)
        : font_file_(font_file), options_(), face_(nullptr)
    {
        error_ = FT_New_Face(gFtLibrary, font_file_.c_str(), 0, &face_);
        if (error_)
        {
            face_ = nullptr;
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
    FreeTypeFace(FreeTypeFace &&rhs)
        : font_file_(rhs.font_file_), options_(rhs.options_), face_(rhs.face_), error_(rhs.error_)
    {
        rhs.face_ = nullptr;
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
    bool DrawCodepoint(DrawContext &context, uint32_t codepoint)
    {
        if (!RenderGlyph(codepoint))
            return false;
        printf("U+%08X -> %s\n", codepoint, font_file_.c_str());
        return DrawBitmap(context, face_->glyph);
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
        error_ = FT_Load_Glyph(face_, glyph_index, options_.load_flags);
        if (error_)
            return false;
        error_ = FT_Render_Glyph(face_->glyph, options_.render_mode);
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
        error_ = FT_Set_Pixel_Sizes(face_, 0, options_.pixel_size);
    }
    void SetupColorFont()
    {
        options_.load_flags |= FT_LOAD_COLOR;

        if (face_->num_fixed_sizes == 0)
            return;
        int best_match = 0;
        int diff = std::abs(options_.pixel_size - face_->available_sizes[0].width);
        for (int i = 1; i < face_->num_fixed_sizes; ++i)
        {
            int ndiff =
                std::abs(options_.pixel_size - face_->available_sizes[i].width);
            if (ndiff < diff)
            {
                best_match = i;
                diff = ndiff;
            }
        }
        error_ = FT_Select_Size(face_, best_match);
    }
    bool DrawBitmap(DrawContext &context, FT_GlyphSlot slot)
    {
        int pixel_mode = slot->bitmap.pixel_mode;
        if (pixel_mode == FT_PIXEL_MODE_BGRA)
            DrawColorBitmap(context, slot);
        else
            DrawNormalBitmap(context, slot);
        context.Advance(slot->advance.x >> 6);
        return true;
    }
    void DrawColorBitmap(DrawContext &context, FT_GlyphSlot slot)
    {
        uint8_t *src = slot->bitmap.buffer;
        // FIXME: Should use metrics for drawing. (e.g. calculate baseline)
        int yoffset = context.Height() - slot->bitmap.rows;
        for (int y = 0; y < slot->bitmap.rows; ++y)
        {
            uint8_t *dest = context.GetDrawPosition(y + yoffset);
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
    void DrawNormalBitmap(DrawContext &context, FT_GlyphSlot slot)
    {
        uint8_t *src = slot->bitmap.buffer;
        // FIXME: Same as DrawColorBitmap()
        int yoffset = context.Height() - slot->bitmap.rows;
        for (int y = 0; y < slot->bitmap.rows; ++y)
        {
            uint8_t *dest = context.GetDrawPosition(y + yoffset);
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

    std::string font_file_;
    FaceOptions options_;
    FT_Face face_;
    int error_;
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
    void DrawCodepoint(DrawContext &context, uint32_t codepoint)
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

class PngWriter
{
  public:
    PngWriter(const std::string &outfile)
        : outfile_(outfile), png_(nullptr), info_(nullptr)
    {
        fp_ = fopen(outfile_.c_str(), "wb");
        if (!fp_)
        {
            std::cerr << "Failed to open: " << outfile_ << std::endl;
            Cleanup();
            return;
        }
        png_ = png_create_write_struct(
            PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png_)
        {
            std::cerr << "Failed to create PNG file" << std::endl;
            Cleanup();
            return;
        }
        info_ = png_create_info_struct(png_);
        if (!info_)
        {
            std::cerr << "Failed to create PNG file" << std::endl;
            Cleanup();
            return;
        }
    }
    ~PngWriter() { Cleanup(); }
    bool Write(uint8_t *rgba, int width, int height)
    {
        static const int kDepth = 8;
        if (!png_)
        {
            std::cerr << "Writer is not initialized" << std::endl;
            return false;
        }
        if (setjmp(png_jmpbuf(png_)))
        {
            std::cerr << "Failed to write PNG" << std::endl;
            Cleanup();
            return false;
        }
        png_set_IHDR(png_, info_, width, height, kDepth,
                     PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_init_io(png_, fp_);
        png_byte **row_pointers =
            static_cast<png_byte **>(png_malloc(png_, height * sizeof(png_byte *)));
        uint8_t *src = rgba;
        for (int y = 0; y < height; ++y)
        {
            png_byte *row =
                static_cast<png_byte *>(png_malloc(png_, width * kBytesPerPixel));
            row_pointers[y] = row;
            for (int x = 0; x < width; ++x)
            {
                *row++ = *src++;
                *row++ = *src++;
                *row++ = *src++;
                *row++ = *src++;
            }
            assert(row - row_pointers[y] == width * kBytesPerPixel);
        }
        assert(src - rgba == width * height * kBytesPerPixel);
        png_set_rows(png_, info_, row_pointers);
        png_write_png(png_, info_, PNG_TRANSFORM_IDENTITY, 0);
        for (int y = 0; y < height; y++)
            png_free(png_, row_pointers[y]);
        png_free(png_, row_pointers);
        Cleanup();
        return true;
    }

  private:
    PngWriter(const PngWriter &) = delete;
    PngWriter operator=(const PngWriter &) = delete;
    void Cleanup()
    {
        if (fp_)
        {
            fclose(fp_);
        }
        if (png_)
            png_destroy_write_struct(&png_, &info_);
        fp_ = nullptr;
        png_ = nullptr;
        info_ = nullptr;
    }

    std::string outfile_;
    FILE *fp_;
    png_structp png_;
    png_infop info_;
    char *rgba_;
    uint32_t width_;
    uint32_t height_;
};

class App
{
  public:
    void AddFont(const std::string &font_file) { font_list_.AddFont(font_file); }
    bool SetText(const char *text) { return UTF8ToCodepoint(text); }
    bool Execute()
    {
        CalculateImageSize();
        Draw();
        return Output();
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
    void CalculateImageSize()
    {
        uint32_t width = 0, height = 0;
        for (auto c : codepoints_)
            font_list_.CalculateBox(c, width, height);
        printf("width: %u, height: %u\n", width, height);
        draw_context_.SetSize(width, height);
    }
    void Draw()
    {
        for (auto c : codepoints_)
            font_list_.DrawCodepoint(draw_context_, c);
    }
    bool Output()
    {
        /*
        PngWriter writer(kDefaultOutputFile);
        return writer.Write(draw_context_.Bitmap(),
                            draw_context_.Width(),
                            draw_context_.Height());
        */

        std::ofstream io("out.ppm", std::ios::binary);
        io << "P6\n";
        io << draw_context_.Width() << " " << draw_context_.Height() << "\n";
        io << "255\n";

        auto p = (const char *)draw_context_.Bitmap();
        for (int y = 0; y < draw_context_.Height(); ++y)
        {
            for (int x = 0; x < draw_context_.Width(); ++x, p += 4)
            {
                io.write(p, 3);
            }
        }

        return true;
    }

    std::vector<uint32_t> codepoints_;
    FontList font_list_;
    DrawContext draw_context_;
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
