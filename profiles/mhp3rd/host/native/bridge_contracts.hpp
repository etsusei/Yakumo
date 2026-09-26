#pragma once

#include "native/contracts.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace mhp3rd::native {

[[nodiscard]] inline bool same_context(const psprecomp::AllegrexContext &a,
                                       const psprecomp::AllegrexContext &b) noexcept {
    return a.gpr == b.gpr && a.hi == b.hi && a.lo == b.lo && a.pc == b.pc && a.fcr31 == b.fcr31 &&
           a.vfpu_ctrl == b.vfpu_ctrl && std::memcmp(a.fpr.data(), b.fpr.data(), sizeof(a.fpr)) == 0 &&
           std::memcmp(a.vfpu.data(), b.vfpu.data(), sizeof(a.vfpu)) == 0;
}

template <std::size_t CodeSize>
[[nodiscard]] bool matches_code_fingerprint(const psprecomp::GuestMemory &memory,
                                            std::uint32_t address, std::string_view expected_sha256) {
    std::array<std::uint8_t, CodeSize> code{};
    if (!memory.contains(address, code.size())) return false;
    memory.copy_out(address, code);
    return psprecomp::sha256_bytes(code) == expected_sha256;
}

// Each interpreter slice executes one instruction, including the return's
// delay slot. Check the return before the slice so a return address inside the
// leaf cannot make us stop before executing that slot.
[[nodiscard]] inline bool run_bounded_reference(psprecomp::Runtime &runtime,
                                                psprecomp::AllegrexContext &context,
                                                std::uint32_t address, std::uint32_t code_size,
                                                std::uint32_t return_offset, unsigned max_slices,
                                                const char *failure_reason) {
    const std::uint32_t return_pc = context.gpr[31];
    for (unsigned slice = 0; slice < max_slices; ++slice) {
        if (context.pc < address || context.pc >= address + code_size) break;
        const bool returning = context.pc == address + return_offset;
        const auto exit = psprecomp::interpret_allegrex(runtime, context, 1);
        if (exit == psprecomp::InterpreterExit::Stopped || exit == psprecomp::InterpreterExit::Unreachable)
            break;
        if (returning && context.pc == return_pc) return true;
    }
    runtime.stop(failure_reason);
    return false;
}

// A small byte model for verification without speculative guest writes. Capture
// every word that a leaf may read or write before simulating it. Physical keys
// preserve RAM aliases and the four mirrors of EDRAM. This models GuestMemory;
// it does not assert hardware or MMIO behavior outside its mapped regions.
template <std::size_t Capacity>
class MemoryShadow {
public:
    [[nodiscard]] bool capture_word(const psprecomp::GuestMemory &memory, std::uint32_t address) {
        if (!memory.contains(address, 4u)) return false;
        for (std::uint32_t i = 0; i < 4u; ++i) {
            const std::uint32_t guest_address = address + i;
            const std::uint32_t key = physical_key(guest_address);
            if (find(key) != count_) continue;
            if (count_ == Capacity) return false;
            bytes_[count_++] = {key, guest_address, memory.load8(guest_address)};
        }
        return true;
    }

    [[nodiscard]] std::uint16_t load16(std::uint32_t address) const {
        return static_cast<std::uint16_t>(byte_at(address)) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(byte_at(address + 1u)) << 8u);
    }

    [[nodiscard]] std::uint32_t load32(std::uint32_t address) const {
        std::uint32_t value = 0u;
        for (std::uint32_t i = 0; i < 4u; ++i)
            value |= static_cast<std::uint32_t>(byte_at(address + i)) << (8u * i);
        return value;
    }

    void store32(std::uint32_t address, std::uint32_t value) {
        for (std::uint32_t i = 0; i < 4u; ++i) {
            const std::size_t index = find(physical_key(address + i));
            if (index == count_) throw std::logic_error("Native memory shadow missed a written byte");
            bytes_[index].value = static_cast<std::uint8_t>(value >> (8u * i));
        }
    }

    [[nodiscard]] bool matches(const psprecomp::GuestMemory &memory) const {
        for (std::size_t i = 0; i < count_; ++i)
            if (memory.load8(bytes_[i].guest_address) != bytes_[i].value) return false;
        return true;
    }

private:
    struct Byte { std::uint32_t key{}, guest_address{}; std::uint8_t value{}; };
    std::array<Byte, Capacity> bytes_{};
    std::size_t count_{};

    [[nodiscard]] static std::uint32_t physical_key(std::uint32_t address) noexcept {
        const std::uint32_t canonical = psprecomp::GuestMemory::canonical(address);
        if (canonical >= psprecomp::GuestMemory::kVramPhysicalBase &&
            canonical < psprecomp::GuestMemory::kVramPhysicalBase + psprecomp::GuestMemory::kVramAddressSpan)
            return psprecomp::GuestMemory::kVramPhysicalBase +
                   ((canonical - psprecomp::GuestMemory::kVramPhysicalBase) &
                    (psprecomp::GuestMemory::kVramSize - 1u));
        return canonical;
    }

    [[nodiscard]] std::size_t find(std::uint32_t key) const noexcept {
        for (std::size_t i = 0; i < count_; ++i) if (bytes_[i].key == key) return i;
        return count_;
    }

    [[nodiscard]] std::uint8_t byte_at(std::uint32_t address) const {
        const std::size_t index = find(physical_key(address));
        if (index == count_) throw std::logic_error("Native memory shadow missed a read byte");
        return bytes_[index].value;
    }
};

} // namespace mhp3rd::native
