#include "native/texture_completion_progress.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

using mhp3rd::native::CompletionProgress;
using mhp3rd::native::CompletionStage;
using mhp3rd::native::CompletionStep;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

} // namespace

int main() {
    CompletionProgress verbatim;
    require(verbatim.accept(CompletionStep::InlineClassified), "verbatim classifier");
    require(verbatim.accept(CompletionStep::CopyCall), "verbatim copy call");
    require(verbatim.accept(CompletionStep::CopyReturned), "verbatim copy return");
    require(verbatim.accept(CompletionStep::HelperReturned), "verbatim helper return");
    require(verbatim.accept(CompletionStep::PolicyAccepted), "verbatim policy");
    require(verbatim.accept(CompletionStep::WorkerRequestCall), "verbatim request call");
    require(verbatim.accept(CompletionStep::WorkerRequestReturned), "verbatim request return");
    require(verbatim.accept(CompletionStep::MainWaitEntered), "verbatim wait enter");
    require(verbatim.accept(CompletionStep::WorkerStarted), "verbatim worker start");
    require(verbatim.accept(CompletionStep::VerbatimObserved), "verbatim branch");
    require(verbatim.accept(CompletionStep::DigestSkipped), "verbatim digest skip");
    require(verbatim.accept(CompletionStep::WorkerAckCall), "verbatim ack call");
    require(verbatim.accept(CompletionStep::WorkerAckReturned), "verbatim ack return");
    require(verbatim.accept(CompletionStep::MainWaitReturned), "verbatim wait return");
    require(verbatim.accept(CompletionStep::RetirementCall), "verbatim retirement call");
    require(verbatim.accept(CompletionStep::RetirementEntered), "verbatim retirement enter");
    require(verbatim.accept(CompletionStep::RetirementReturned), "verbatim retirement return");
    require(verbatim.complete(), "verbatim completion");

    CompletionProgress transformed(true, true);
    require(transformed.accept(CompletionStep::InlineClassified), "transformed classifier");
    require(transformed.accept(CompletionStep::CopyCall), "transformed copy call");
    require(transformed.accept(CompletionStep::CopyReturned), "transformed copy return");
    require(transformed.accept(CompletionStep::HelperReturned), "transformed helper return");
    require(transformed.accept(CompletionStep::PolicyAccepted), "transformed policy");
    require(transformed.accept(CompletionStep::WorkerRequestCall), "transformed request call");
    require(transformed.accept(CompletionStep::WorkerRequestReturned), "transformed request return");
    require(transformed.accept(CompletionStep::MainWaitEntered), "transformed wait enter");
    require(transformed.accept(CompletionStep::WorkerStarted), "transformed worker start");
    require(transformed.accept(CompletionStep::TransformCall), "transformed transform call");
    require(transformed.accept(CompletionStep::TransformReturned), "transformed transform return");
    require(transformed.accept(CompletionStep::DigestCall), "transformed digest call");
    require(transformed.accept(CompletionStep::DigestReturned), "transformed digest return");
    require(transformed.accept(CompletionStep::WorkerAckCall), "transformed ack call");
    require(transformed.accept(CompletionStep::WorkerAckReturned), "transformed ack return");
    require(transformed.accept(CompletionStep::MainWaitReturned), "transformed wait return");
    require(transformed.accept(CompletionStep::RetirementCall), "transformed retirement call");
    require(transformed.accept(CompletionStep::RetirementEntered), "transformed retirement enter");
    require(transformed.accept(CompletionStep::RetirementReturned), "transformed retirement return");
    require(transformed.stage == CompletionStage::Complete, "transformed completion");

    CompletionProgress unsupported;
    require(unsupported.accept(CompletionStep::UnsupportedCopyRoute), "unsupported route");
    require(unsupported.stage == CompletionStage::Unsupported, "unsupported stage");
    require(!unsupported.accept(CompletionStep::RetirementReturned), "unsupported terminal");

    CompletionProgress invalid;
    require(!invalid.accept(CompletionStep::WorkerAckCall), "invalid sequence");
    require(invalid.stage == CompletionStage::Invalid, "invalid stage");

    // A return receipt cannot manufacture its call edge. The tracker uses the
    // observation helpers only after validating a separate call receipt.
    CompletionProgress missing_call;
    require(missing_call.accept(CompletionStep::InlineClassified), "missing classifier");
    require(!missing_call.accept(CompletionStep::CopyReturned), "missing copy call accepted");
    require(missing_call.stage == CompletionStage::Invalid, "missing call stage");

    CompletionProgress observed;
    require(observed.observe_inline_classified(), "observed classifier");
    require(observed.observe_copy_return(), "observed copy return");
    require(observed.accept(CompletionStep::HelperReturned), "observed helper return");
    require(observed.accept(CompletionStep::PolicyAccepted), "observed policy");
    require(observed.accept(CompletionStep::WorkerRequestCall), "observed request call");
    require(observed.observe_worker_accept(), "observed worker accept");
    require(observed.accept(CompletionStep::VerbatimObserved), "observed verbatim");
    require(observed.accept(CompletionStep::DigestSkipped), "observed digest skip");
    require(observed.observe_worker_ack_return(), "observed ack return");
    require(observed.observe_main_wait_return(), "observed wait return");
    require(observed.observe_retirement_call(), "observed retirement call");
    require(observed.accept(CompletionStep::RetirementEntered), "observed retirement entry");
    require(observed.accept(CompletionStep::RetirementReturned), "observed retirement return");
    require(observed.complete(), "observed completion");

    CompletionProgress duplicate;
    require(duplicate.observe_inline_classified(), "duplicate classifier first");
    require(!duplicate.observe_inline_classified(), "duplicate classifier accepted");
    require(duplicate.stage == CompletionStage::Invalid, "duplicate classifier stage");

    CompletionProgress wrong_worker;
    require(wrong_worker.accept(CompletionStep::InlineClassified), "wrong worker classifier");
    require(wrong_worker.accept(CompletionStep::CopyCall), "wrong worker copy call");
    require(wrong_worker.accept(CompletionStep::CopyReturned), "wrong worker copy return");
    require(wrong_worker.accept(CompletionStep::HelperReturned), "wrong worker helper");
    require(wrong_worker.accept(CompletionStep::PolicyAccepted), "wrong worker policy");
    require(wrong_worker.accept(CompletionStep::WorkerRequestCall), "wrong worker request");
    require(!wrong_worker.accept(CompletionStep::TransformCall),
            "worker work accepted before worker start");

    CompletionProgress cancelled;
    require(cancelled.accept(CompletionStep::Cancel), "cancellation receipt");
    require(cancelled.closed(), "cancellation closes operation");
    require(!cancelled.accept(CompletionStep::RetirementReturned),
            "late retirement accepted after cancellation");
    return 0;
}
