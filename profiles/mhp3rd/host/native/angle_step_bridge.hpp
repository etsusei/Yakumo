#pragma once

#include <cstdint>

namespace psprecomp { class Runtime; class GuestMemory; struct AllegrexContext; }

namespace mhp3rd::native {
inline constexpr std::uint32_t kAngleStepAddress = 0x088775acu;
inline constexpr std::uint32_t kAngleStepCodeSize = 100u;
enum class AngleStepMode { Off, Verify, Native };
struct AngleStepStats { std::uint64_t calls{}, verified{}, mismatches{}; };

// A thin ABI adapter. Game resources and callers keep their original layout;
// the math in angle_step.hpp itself has no PSP/runtime dependency.
void apply_angle_step(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &context);
// Refuse unknown code. Call only while no generated function is executing.
[[nodiscard]] bool install_angle_step(psprecomp::Runtime &runtime, AngleStepMode mode);
void configure_angle_step(psprecomp::Runtime &runtime);
[[nodiscard]] AngleStepStats angle_step_stats();
void report_angle_step();
} // namespace mhp3rd::native
