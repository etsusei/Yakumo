#include "hle/control_delivery.hpp"
#include "testing/game_observers.hpp"
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
namespace {
using namespace mhp3rd::testing;
int failures{};
void check(bool condition, const char *message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct Sink : JournalSink {
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    explicit Sink(std::shared_ptr<std::vector<std::uint8_t>> data) : bytes(std::move(data)) {}
    bool write(std::span<const std::uint8_t> data) override {
        bytes->insert(bytes->end(), data.begin(), data.end()); return true;
    }
    bool flush() override { return true; }
};
}
int main() {
    psprecomp::GuestMemory memory;
    constexpr auto base = psprecomp::GuestMemory::kPhysicalBase + 128u;
    const mhp3rd::ControlSample sample{0x10203040u, 1, 255, 128, 200};
    const std::array<std::uint8_t, 16> expected{0x34,0x12,0,0,0x40,0x30,0x20,0x10,1,255,128,200,0,0,0,0};
    std::array<std::uint8_t, 40> initial{}; initial.fill(0xa5);
    memory.copy_in(base - 4, initial);
    mhp3rd::deliver_control_buffer(memory, base | 0x40000000u, 2, 0x100001234ull, 7, sample);
    std::array<std::uint8_t, 40> disabled{}; memory.copy_out(base - 4, disabled);
    check(std::equal(expected.begin(), expected.end(), disabled.begin() + 4) &&
          std::equal(expected.begin(), expected.end(), disabled.begin() + 20),
          "HD sample bytes preserve time truncation, final axes and reserved padding");
    check(disabled[0] == 0xa5 && disabled[3] == 0xa5 && disabled[36] == 0xa5 && disabled[39] == 0xa5,
          "controller writes preserve canaries");
    auto bytes = std::make_shared<std::vector<std::uint8_t>>();
    auto recorder = std::make_shared<SessionRecorder>(std::make_unique<Sink>(bytes), Fields{});
    auto observer = std::make_shared<GameObserver>(recorder); set_active_observer(observer);
    observer->domain(InputDomain::Game);
    memory.copy_in(base - 4, initial);
    mhp3rd::deliver_control_buffer(memory, base | 0x40000000u, 2, 0x100001234ull, 7, sample);
    std::array<std::uint8_t, 40> enabled{}; memory.copy_out(base - 4, enabled);
    check(enabled == disabled, "enabled observation does not alter delivered bytes");
    check(observer->timeline().control_read_ordinal == 1, "one call records one successful delivery ordinal");
    try {
        mhp3rd::deliver_control_buffer(memory, psprecomp::GuestMemory::kPhysicalBase + memory.size() - 16,
                                      2, 1, 8, sample);
        check(false, "unmapped second sample must fail");
    } catch (const std::exception &) {}
    check(observer->timeline().control_read_ordinal == 1, "partial delivery is not recorded as successful input");
    set_active_observer({});
    check(recorder->close("synthetic_delivery"), "synthetic recorder closes");
    const auto journal = recover_journal(*bytes);
    check(journal.complete(), "observed delivery has complete journal framing");
    unsigned pads = 0;
    for (const auto &record : journal.records) {
        if (record.payload.find("\"event\":\"input.pad\"") == std::string::npos) continue;
        ++pads;
        check(record.payload.find("\"sample_count\":2") != std::string::npos &&
              record.payload.find("\"right_y\":200") != std::string::npos &&
              record.payload.find("\"virtual_us\":4294971956") != std::string::npos,
              "record observes the exact supplied final tuple and full virtual clock");
    }
    check(pads == 1, "only successful post-write delivery is logged");
    std::cout << "Controller delivery failures: " << failures << '\n';
    return failures ? 1 : 0;
}
