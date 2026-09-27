#include "resources/tmh_pixel_plan.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {
using namespace mhp3rd::resources;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
TmhDescriptor fixture(std::uint16_t width, std::uint16_t height,
                      std::uint32_t format, std::size_t payload) {
    return {64u, format, width, height, 4096u, 3u, 256,
        SharedBytes::take(std::vector<std::uint8_t>(payload)),
        SharedBytes::take(std::vector<std::uint8_t>(1024u))};
}
template<class F> void rejects(F fn) {
    try { fn(); } catch (const std::invalid_argument &) { return; }
    throw std::runtime_error("Unsupported builder context was accepted");
}
}

int main() {
    try {
        // These cases exercise the non-power-of-two dimensions found in the
        // original resource inventory; no original payload is embedded here.
        for (auto [width, height, canvas_width, canvas_height, payload, window] : {
                std::tuple{132u, 64u, 256u, 64u, 4224u, 4224u},
                std::tuple{68u, 68u, 128u, 128u, 2320u, 4352u},
                std::tuple{160u, 168u, 256u, 256u, 13440u, 20480u}}) {
            const auto descriptor = fixture(width, height, 4u, payload);
            const auto encoded = plan_model_builder_pixels(descriptor,
                TmhPixelDomain::EncodedRectangle, PixelLayout::LegacySwizzleCompatibility);
            const auto canvas = plan_model_builder_pixels(descriptor,
                TmhPixelDomain::ModelBuilderCanvas, PixelLayout::LegacySwizzleCompatibility);
            require(encoded.decode.width == width && encoded.decode.height == height,
                "Encoded extent changed");
            require(encoded.image_window_bytes == width / 2u * height,
                "Encoded stride window differs");
            require(canvas.decode.width == canvas_width && canvas.decode.height == canvas_height,
                "Builder power-of-two extent differs");
            require(canvas.decode.stride_pixels == width && canvas.image_window_bytes == window,
                "Builder rounded the row stride or lost the exact window");
            require(canvas.image_payload_bytes == payload &&
                canvas.reads_after_image_payload == (window > payload), "Payload boundary lost");
            require(canvas.image_offset == 64u && canvas.palette_offset == 4096u &&
                canvas.decode.palette_mask == 255u && canvas.decode.palette_shift == 0u &&
                canvas.decode.palette_offset == 0u, "Builder palette or source coordinates differ");
        }
        auto direct = fixture(7u, 5u, 8u, 32u);
        direct.palette.reset(); direct.palette_offset.reset();
        const auto dxt = plan_model_builder_pixels(direct, TmhPixelDomain::ModelBuilderCanvas,
                                                   PixelLayout::Swizzled16x8);
        require(dxt.decode.format == PixelFormat::Dxt1 && dxt.decode.layout == PixelLayout::Linear &&
                dxt.image_window_bytes == 32u && !dxt.palette_offset, "DXT builder profile differs");
        auto indexed = fixture(32u, 8u, 5u, 256u);
        indexed.palette_format = 1u;
        const auto clut8 = plan_model_builder_pixels(indexed, TmhPixelDomain::EncodedRectangle,
                                                     PixelLayout::Swizzled16x8);
        require(clut8.decode.format == PixelFormat::Clut8 && clut8.image_window_bytes == 256u &&
                clut8.decode.palette_format == PaletteFormat::Rgba5551, "CLUT8 palette profile differs");
        const auto check_invalid = [](const TmhDescriptor &descriptor) {
            (void)plan_model_builder_pixels(descriptor, TmhPixelDomain::ModelBuilderCanvas,
                                           PixelLayout::LegacySwizzleCompatibility);
        };
        auto invalid = indexed; invalid.width = 0u;
        rejects([&] { check_invalid(invalid); });
        invalid = indexed; invalid.height = 1025u;
        rejects([&] { check_invalid(invalid); });
        invalid = indexed; invalid.image_format = 6u;
        rejects([&] { check_invalid(invalid); });
        invalid = indexed; invalid.palette_format = 0u;
        rejects([&] { check_invalid(invalid); });
        rejects([&] { (void)plan_model_builder_pixels(indexed,
            TmhPixelDomain::ModelBuilderCanvas, PixelLayout::Unspecified); });
        rejects([&] { (void)plan_model_builder_pixels(indexed,
            static_cast<TmhPixelDomain>(99), PixelLayout::Swizzled16x8); });
        std::cout << "tmh_pixel_plan_tests: passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
