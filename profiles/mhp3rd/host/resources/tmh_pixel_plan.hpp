#pragma once
#include "resources/tmh.hpp"
#include "resources/pixel_decode.hpp"

namespace mhp3rd::resources {

enum class TmhPixelDomain { EncodedRectangle, ModelBuilderCanvas };
struct TmhPixelPlan {
    PixelDecodeSpec decode;
    std::uint32_t image_offset;
    std::optional<std::uint32_t> palette_offset;
    std::size_t image_window_bytes;
    std::size_t image_payload_bytes;
    bool reads_after_image_payload;
};

// Explicitly selects the state emitted by the recovered 0x0889E5C0 builder:
// indexed swizzle, DXT linear, palette mask FF/shift0/offset0. The caller must
// choose a domain and strict versus current-software swizzle policy. This is
// not proof that a particular file reaches that builder or of visible UVs.
[[nodiscard]] TmhPixelPlan plan_model_builder_pixels(const TmhDescriptor &descriptor,
    TmhPixelDomain domain, PixelLayout indexed_layout);

} // namespace mhp3rd::resources
