#include "image.hpp"

#include "dx_common.hpp"

#include <wincodec.h>

#include <algorithm>
#include <bit>

namespace {

constexpr std::uint32_t kChannels = 4;

// Halves an image with a 2x2 box filter.
//
// The average is taken over the stored bytes, not over linear light. That is
// wrong in the way every unmanaged mip chain is wrong -- a checkerboard of black
// and white averages to 0.5 rather than to the 0.73 that matches its perceived
// brightness -- and it is wrong consistently with the rest of this renderer,
// which never converts to linear at all. Fixing it belongs with the sRGB pass
// that fixes the back buffer, the flat colours and the lighting all together.
std::vector<std::byte> Downsample(std::span<const std::byte> source, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t& out_width,
                                  std::uint32_t& out_height) {
    out_width = std::max(1u, width / 2);
    out_height = std::max(1u, height / 2);

    std::vector<std::byte> result(static_cast<size_t>(out_width) * out_height * kChannels);

    for (std::uint32_t y = 0; y < out_height; ++y) {
        // An odd dimension leaves the last row or column without a partner, so
        // it is paired with itself rather than read past the end.
        const std::uint32_t y0 = y * 2;
        const std::uint32_t y1 = std::min(y0 + 1, height - 1);

        for (std::uint32_t x = 0; x < out_width; ++x) {
            const std::uint32_t x0 = x * 2;
            const std::uint32_t x1 = std::min(x0 + 1, width - 1);

            for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
                auto at = [&](std::uint32_t px, std::uint32_t py) {
                    const size_t index =
                        (static_cast<size_t>(py) * width + px) * kChannels + channel;
                    return static_cast<std::uint32_t>(source[index]);
                };
                const std::uint32_t sum = at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1);
                result[(static_cast<size_t>(y) * out_width + x) * kChannels + channel] =
                    static_cast<std::byte>((sum + 2) / 4);
            }
        }
    }

    return result;
}

} // namespace

Image DecodeImage(std::span<const std::byte> encoded) {
    if (encoded.empty()) {
        throw std::runtime_error("Cannot decode an empty image");
    }

    ComPtr<IWICImagingFactory> factory;
    ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory)),
                  "CoCreateInstance(WICImagingFactory)");

    // The stream neither copies nor takes ownership: it reads straight out of
    // the glTF buffer, which outlives this call.
    ComPtr<IWICStream> stream;
    ThrowIfFailed(factory->CreateStream(&stream), "WICImagingFactory::CreateStream");
    ThrowIfFailed(stream->InitializeFromMemory(
                      reinterpret_cast<WICInProcPointer>(const_cast<std::byte*>(encoded.data())),
                      static_cast<DWORD>(encoded.size())),
                  "WICStream::InitializeFromMemory");

    ComPtr<IWICBitmapDecoder> decoder;
    ThrowIfFailed(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                   WICDecodeMetadataCacheOnDemand, &decoder),
                  "WICImagingFactory::CreateDecoderFromStream");

    ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(decoder->GetFrame(0, &frame), "WICBitmapDecoder::GetFrame");

    // Whatever the file held -- palettised, greyscale, 24-bit -- comes out as
    // four bytes per pixel in R, G, B, A order, which is DXGI_FORMAT_R8G8B8A8.
    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(factory->CreateFormatConverter(&converter),
                  "WICImagingFactory::CreateFormatConverter");
    ThrowIfFailed(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom),
                  "WICFormatConverter::Initialize");

    Image image{};
    ThrowIfFailed(converter->GetSize(&image.width, &image.height), "WICFormatConverter::GetSize");
    if (image.width == 0 || image.height == 0) {
        throw std::runtime_error("Decoded image has no pixels");
    }

    const std::uint32_t stride = image.width * kChannels;
    std::vector<std::byte> base(static_cast<size_t>(stride) * image.height);
    ThrowIfFailed(converter->CopyPixels(nullptr, stride, static_cast<UINT>(base.size()),
                                        reinterpret_cast<BYTE*>(base.data())),
                  "WICFormatConverter::CopyPixels");

    // A full chain down to 1x1: the longer side halves until it runs out, which
    // takes one more level than its highest set bit.
    image.levels.reserve(std::bit_width(std::max(image.width, image.height)));
    image.levels.push_back(std::move(base));

    std::uint32_t width = image.width;
    std::uint32_t height = image.height;
    while (width > 1 || height > 1) {
        std::uint32_t next_width = 0;
        std::uint32_t next_height = 0;
        image.levels.push_back(
            Downsample(image.levels.back(), width, height, next_width, next_height));
        width = next_width;
        height = next_height;
    }

    return image;
}
