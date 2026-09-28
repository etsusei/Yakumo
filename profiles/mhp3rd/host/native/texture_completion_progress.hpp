#pragma once

#include <cstdint>

namespace mhp3rd::native {

enum class CompletionStep : std::uint8_t {
    InlineClassified,
    CopyCall,
    CopyReturned,
    HelperReturned,
    PolicyAccepted,
    WorkerRequestCall,
    WorkerRequestReturned,
    MainWaitEntered,
    WorkerStarted,
    TransformCall,
    TransformReturned,
    VerbatimObserved,
    DigestCall,
    DigestReturned,
    DigestSkipped,
    WorkerAckCall,
    WorkerAckReturned,
    MainWaitReturned,
    RetirementCall,
    RetirementEntered,
    RetirementReturned,
    UnsupportedCopyRoute,
    Abort,
    Cancel,
};

enum class CompletionStage : std::uint8_t {
    ReadExact,
    InlineSelected,
    CopyRunning,
    CopyFinished,
    HelperFinished,
    PolicyFinished,
    WorkerArmed,
    WorkerRunning,
    TransformRunning,
    DataReady,
    DigestRunning,
    DigestReady,
    AckRunning,
    WorkerAcknowledged,
    WorkerJoined,
    RetirementPending,
    Retiring,
    Complete,
    Unsupported,
    Aborted,
    Cancelled,
    Invalid,
};

struct CompletionProgress {
    CompletionStage stage{CompletionStage::ReadExact};
    bool needs_transform{};
    bool needs_digest{};
    bool request_returned{};
    bool wait_entered{};

    explicit CompletionProgress(bool transform = false, bool digest = false) noexcept
        : needs_transform(transform), needs_digest(digest) {}

    [[nodiscard]] bool complete() const noexcept {
        return stage == CompletionStage::Complete;
    }
    [[nodiscard]] bool closed() const noexcept {
        return stage >= CompletionStage::Complete;
    }

    [[nodiscard]] bool accept(CompletionStep step) noexcept {
        if (closed()) return false;
        if (step == CompletionStep::Abort) {
            stage = CompletionStage::Aborted;
            return true;
        }
        if (step == CompletionStep::Cancel) {
            stage = CompletionStage::Cancelled;
            return true;
        }
        if (step == CompletionStep::UnsupportedCopyRoute) {
            if (stage != CompletionStage::ReadExact) return reject();
            stage = CompletionStage::Unsupported;
            return true;
        }
        if (step == CompletionStep::WorkerRequestReturned) {
            if (!worker_interval() || request_returned) return reject();
            request_returned = true;
            return true;
        }
        if (step == CompletionStep::MainWaitEntered) {
            if (!worker_interval() || wait_entered) return reject();
            wait_entered = true;
            return true;
        }
        switch (step) {
        case CompletionStep::InlineClassified:
            return move(CompletionStage::ReadExact, CompletionStage::InlineSelected);
        case CompletionStep::CopyCall:
            return move(CompletionStage::InlineSelected, CompletionStage::CopyRunning);
        case CompletionStep::CopyReturned:
            return move(CompletionStage::CopyRunning, CompletionStage::CopyFinished);
        case CompletionStep::HelperReturned:
            return move(CompletionStage::CopyFinished, CompletionStage::HelperFinished);
        case CompletionStep::PolicyAccepted:
            return move(CompletionStage::HelperFinished, CompletionStage::PolicyFinished);
        case CompletionStep::WorkerRequestCall:
            return move(CompletionStage::PolicyFinished, CompletionStage::WorkerArmed);
        case CompletionStep::WorkerStarted:
            return move(CompletionStage::WorkerArmed, CompletionStage::WorkerRunning);
        case CompletionStep::TransformCall:
            if (!needs_transform) return reject();
            return move(CompletionStage::WorkerRunning, CompletionStage::TransformRunning);
        case CompletionStep::TransformReturned:
            return move(CompletionStage::TransformRunning, CompletionStage::DataReady);
        case CompletionStep::VerbatimObserved:
            if (needs_transform) return reject();
            return move(CompletionStage::WorkerRunning, CompletionStage::DataReady);
        case CompletionStep::DigestCall:
            if (!needs_digest) return reject();
            return move(CompletionStage::DataReady, CompletionStage::DigestRunning);
        case CompletionStep::DigestReturned:
            return move(CompletionStage::DigestRunning, CompletionStage::DigestReady);
        case CompletionStep::DigestSkipped:
            if (needs_digest) return reject();
            return move(CompletionStage::DataReady, CompletionStage::DigestReady);
        case CompletionStep::WorkerAckCall:
            return move(CompletionStage::DigestReady, CompletionStage::AckRunning);
        case CompletionStep::WorkerAckReturned:
            return move(CompletionStage::AckRunning, CompletionStage::WorkerAcknowledged);
        case CompletionStep::MainWaitReturned:
            if (!request_returned || !wait_entered) return reject();
            return move(CompletionStage::WorkerAcknowledged, CompletionStage::WorkerJoined);
        case CompletionStep::RetirementCall:
            return move(CompletionStage::WorkerJoined, CompletionStage::RetirementPending);
        case CompletionStep::RetirementEntered:
            return move(CompletionStage::RetirementPending, CompletionStage::Retiring);
        case CompletionStep::RetirementReturned:
            return move(CompletionStage::Retiring, CompletionStage::Complete);
        default:
            return reject();
        }
    }

    // These helpers represent return/accept boundaries after the tracker has
    // validated the separate call receipt. They deliberately do not synthesize
    // the missing CompletionStep values; direct users of accept() retain the
    // stricter explicit-call contract above.
    [[nodiscard]] bool observe_inline_classified() noexcept {
        return move(CompletionStage::ReadExact, CompletionStage::InlineSelected);
    }

    [[nodiscard]] bool observe_copy_return() noexcept {
        return move(CompletionStage::InlineSelected, CompletionStage::CopyFinished);
    }

    [[nodiscard]] bool observe_worker_accept() noexcept {
        return move(CompletionStage::WorkerArmed, CompletionStage::WorkerRunning);
    }

    [[nodiscard]] bool observe_worker_ack_return() noexcept {
        return move(CompletionStage::DigestReady, CompletionStage::WorkerAcknowledged);
    }

    [[nodiscard]] bool observe_main_wait_return() noexcept {
        return move(CompletionStage::WorkerAcknowledged, CompletionStage::WorkerJoined);
    }

    [[nodiscard]] bool observe_retirement_call() noexcept {
        return move(CompletionStage::WorkerJoined, CompletionStage::RetirementPending);
    }

private:
    [[nodiscard]] bool worker_interval() const noexcept {
        return stage >= CompletionStage::WorkerArmed &&
               stage <= CompletionStage::WorkerAcknowledged;
    }
    [[nodiscard]] bool reject() noexcept {
        stage = CompletionStage::Invalid;
        return false;
    }
    [[nodiscard]] bool move(CompletionStage expected, CompletionStage next) noexcept {
        if (stage != expected) return reject();
        stage = next;
        return true;
    }
};

} // namespace mhp3rd::native
