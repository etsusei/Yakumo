#include "testing/runtime_diagnostics.hpp"
#include "testing/probes.hpp"
#include "perf/frame_stats.hpp"
#include "psprecomp/guest_memory.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace mhp3rd::testing;
int failures{};
void check(bool value, const char *why) {
    if (!value) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}
class Sink final : public JournalSink {
public:
    explicit Sink(std::vector<std::uint8_t> &bytes) : bytes_(bytes) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool flush() override { return true; }
private:
    std::vector<std::uint8_t> &bytes_;
};
void lifecycle(bool supported) {
    std::vector<std::uint8_t> bytes;
    auto recorder = std::make_shared<SessionRecorder>(std::make_unique<Sink>(bytes), Fields{});
    auto observer = std::make_shared<GameObserver>(recorder);
    psprecomp::GuestMemory memory(64 * 1024 * 1024);
    {
        RuntimeDiagnostics diagnostics(observer, memory, supported, kProbeAll);
        mhp3rd::perf::Summary summary{};
        diagnostics.tick(summary);
        for (unsigned i = 0; i < 10; ++i) diagnostics.tick(summary);
        diagnostics.close();
        diagnostics.close();
        summary.valid = true; summary.second = 1;
        diagnostics.tick(summary); // Closed sessions cannot submit or read memory.
    }
    check(recorder->close("done"), "journal closes");
    const auto journal = recover_journal(bytes);
    check(journal.complete() && !journal.loss_seen, "complete diagnostics journal");
    unsigned configurations{}, states{}, probes{}, performance{};
    for (const auto &record : journal.records) {
        configurations += record.payload.find("\"diagnostics.configuration\"") != std::string::npos;
        states += record.payload.find("\"game.state\"") != std::string::npos;
        probes += record.payload.find("\"probe.summary\"") != std::string::npos;
        performance += record.kind == EventKind::Performance;
    }
    check(configurations == 1, "one session configuration");
    check(states == 1, "state sampling is bounded to one host second");
    check(probes == (supported ? 10u : 0u), "supported session emits periodic and final zero-coverage rows");
    check(performance == 0, "close suppresses later performance emission");
}
} // namespace
int main() {
    check(parse_probe_selection("") == 0 && parse_probe_selection("off") == 0, "selection disabled by default");
    check(parse_probe_selection("all") == kProbeAll, "all leaves selectable");
    check(parse_probe_selection("vector,angle") == (kProbeVector | kProbeAngle), "finite case selection");
    for (const char *bad : {"unknown", "angle,angle", "all,angle", "vector,", ",copy", " vector", "OFF"}) {
        bool threw{};
        try { (void)parse_probe_selection(bad); } catch (const std::invalid_argument &) { threw = true; }
        check(threw, "invalid or ambiguous selection rejected");
    }
    lifecycle(false);
    lifecycle(true);
    std::cout << "runtime diagnostics failures=" << failures << '\n';
    return failures ? 1 : 0;
}
