#include "resources/tmh_pixel_plan.hpp"
#include <bit>
#include <stdexcept>

namespace mhp3rd::resources {
TmhPixelPlan plan_model_builder_pixels(const TmhDescriptor &descriptor,
        TmhPixelDomain domain, PixelLayout indexed_layout) {
    // The observed original lookup table is certified only through 1024.
    if (descriptor.width == 0u || descriptor.height == 0u ||
        descriptor.width > 1024u || descriptor.height > 1024u)
        throw std::invalid_argument("TMH builder dimensions exceed the verified table");
    if (domain != TmhPixelDomain::EncodedRectangle && domain != TmhPixelDomain::ModelBuilderCanvas)
        throw std::invalid_argument("Unknown TMH pixel domain");
    if (indexed_layout != PixelLayout::Swizzled16x8 &&
        indexed_layout != PixelLayout::LegacySwizzleCompatibility)
        throw std::invalid_argument("TMH builder requires an explicit swizzle policy");
    PixelDecodeSpec spec;
    spec.width = descriptor.width;
    spec.height = descriptor.height;
    if (domain == TmhPixelDomain::ModelBuilderCanvas) {
        spec.width = 1u << std::bit_width(spec.width - 1u);
        spec.height = 1u << std::bit_width(spec.height - 1u);
    }
    spec.stride_pixels = descriptor.width;
    spec.palette_shift = 0u; spec.palette_mask = 255u; spec.palette_offset = 0u;
    std::size_t window{};
    switch (descriptor.image_format) {
    case 4u:
    case 5u:
        spec.format = descriptor.image_format == 4u ? PixelFormat::Clut4 : PixelFormat::Clut8;
        spec.layout = indexed_layout;
        if (descriptor.palette_format == 1u) spec.palette_format = PaletteFormat::Rgba5551;
        else if (descriptor.palette_format == 3u) spec.palette_format = PaletteFormat::Rgba8888;
        else throw std::invalid_argument("Unsupported TMH palette profile");
        window = std::size_t(spec.stride_pixels) * (descriptor.image_format == 4u ? 4u : 8u) / 8u * spec.height;
        break;
    case 8u:
        spec.format = PixelFormat::Dxt1;
        spec.layout = PixelLayout::Linear;
        window = std::size_t((spec.width + 3u) / 4u) * ((spec.height + 3u) / 4u) * 8u;
        break;
    default: throw std::invalid_argument("Unsupported TMH image profile");
    }
    return {spec, descriptor.image_offset, descriptor.palette_offset, window,
            descriptor.image.bytes().size(), window > descriptor.image.bytes().size()};
}
} // namespace mhp3rd::resources
