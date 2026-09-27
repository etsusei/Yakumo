#pragma once

#include <stdexcept>
#include <string_view>

namespace mhp3rd::gpu {
inline constexpr const char *kTextureDecodeSwitch = "MHP3RD_PORTABLE_TEXTURE_DECODE";
inline constexpr const char *kTextureDecodeSchema = "yakumo-texture-decode-v1";
enum class TextureDecodeMode { Off, Verify, Native };

inline TextureDecodeMode parse_texture_decode_mode(const char *value) {
    const std::string_view text = value ? value : "off";
    if (text == "off") return TextureDecodeMode::Off;
    if (text == "verify") return TextureDecodeMode::Verify;
    if (text == "native") return TextureDecodeMode::Native;
    throw std::invalid_argument("Invalid MHP3RD_PORTABLE_TEXTURE_DECODE; expected off, verify or native");
}
inline const char *texture_decode_mode_name(TextureDecodeMode mode) {
    switch (mode) {
    case TextureDecodeMode::Off: return "off";
    case TextureDecodeMode::Verify: return "verify";
    case TextureDecodeMode::Native: return "native";
    }
    throw std::invalid_argument("Unknown portable texture decode mode");
}
inline void require_texture_decode_policy(TextureDecodeMode mode, bool baseline_sealed,
                                          bool renderer_compiled) {
    (void)texture_decode_mode_name(mode);
    if (mode != TextureDecodeMode::Off && (baseline_sealed || !renderer_compiled))
        throw std::invalid_argument("Portable texture decoding requires a candidate renderer build");
}
} // namespace mhp3rd::gpu
