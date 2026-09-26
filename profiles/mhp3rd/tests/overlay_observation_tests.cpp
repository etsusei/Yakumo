#include "testing/overlay_observation.hpp"
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <iostream>
#include <vector>

namespace {
int failures{};
void check(bool condition, const char *description) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
void put(std::vector<std::uint8_t> &data, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
}
int main() {
    using mhp3rd::testing::read_overlay_identity;
    psprecomp::GuestMemory memory(32u * 1024u * 1024u);
    constexpr auto base = psprecomp::GuestMemory::kPhysicalBase + 64u;
    std::vector<std::uint8_t> image(80, 0);
    image[0]='M'; image[1]='W'; image[2]='o'; image[3]='3';
    put(image, 8, base); put(image, 12, 8); put(image, 16, 8);
    image[32]='t'; image[33]='e'; image[34]='s'; image[35]='t';
    for (unsigned i = 64; i < image.size(); ++i) image[i] = static_cast<std::uint8_t>(i);
    memory.copy_in(base, image);
    const auto original = read_overlay_identity(memory, base);
    check(original && original->identity.name == "test" && original->identity.code_size == 8 &&
          original->identity.image_size == 80 && !original->identity.matched_corpus,
          "header/code identity does not infer corpus trust");
    std::vector<std::uint8_t> after(image.size()); memory.copy_out(base, after);
    check(after == image, "identity capture does not write guest bytes");
    memory.store8(base + 72, 255);
    const auto changed_data = read_overlay_identity(memory, base);
    check(original && changed_data && original->corpus_hash == changed_data->corpus_hash,
          "mutable data is excluded from code identity");
    memory.store8(base + 64, 255);
    const auto changed_code = read_overlay_identity(memory, base);
    check(original && changed_code && original->corpus_hash != changed_code->corpus_hash &&
          original->identity.header_fingerprint == changed_code->identity.header_fingerprint,
          "code changes are detected even when the header is unchanged");
    memory.copy_in(base, image); memory.store8(base + 32, 'x');
    const auto changed_header = read_overlay_identity(memory, base);
    check(original && changed_header && original->corpus_hash != changed_header->corpus_hash,
          "header participates in immutable identity");
    memory.copy_in(base, image); memory.store32(base + 8, base + 1);
    check(!read_overlay_identity(memory, base), "wrong load address rejected");
    memory.copy_in(base, image); memory.store32(base + 12, 8u * 1024u * 1024u + 1u);
    check(!read_overlay_identity(memory, base), "oversized observation rejected before reading code");
    memory.copy_in(base, image); memory.store32(base + 12, 0);
    check(!read_overlay_identity(memory, base), "empty code cannot certify an executable overlay");
    memory.copy_in(base, image); memory.store32(base + 16, 0xffffffffu);
    check(!read_overlay_identity(memory, base), "image length overflow rejected");
    memory.copy_in(base, image); memory.store32(base + 16, 32u * 1024u * 1024u);
    check(!read_overlay_identity(memory, base), "truncated mapped image rejected");
    check(!read_overlay_identity(memory, 0xfffffff0u), "unmapped header rejected");
    memory.copy_in(base, image); memory.store8(base, 0);
    check(!read_overlay_identity(memory, base), "missing magic rejected");
    std::cout << "Overlay observation failures: " << failures << '\n';
    return failures ? 1 : 0;
}
