// = Requirements: freetype 2.5
#include "ft.h"
#include "image.h"
#include <sstream>
#include <iostream>

static uint32_t HexToInt(const std::string &src)
{
    std::stringstream ss;
    ss << std::hex << src;
    uint32_t codepoint;
    ss >> codepoint;
    return codepoint;
}

const int kDefaultPixelSize = 128;
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
