#pragma once
#include <vector>
#include <memory>

class FontBase;
class FTImpl;
class Image;
class FT
{
    FTImpl *impl_ = nullptr;
    std::vector<std::shared_ptr<FontBase>> faces_;

  public:
    FT();
    ~FT();

    void AddFont(const std::string &fontfile, int pixel_size);

    std::shared_ptr<Image> RenderToImage(uint32_t codepoint)
    {
        return RenderToImage(&codepoint, 1);
    }

    std::shared_ptr<Image> RenderToImage(const uint32_t *codepoints, size_t count);
};
