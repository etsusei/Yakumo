#pragma once

#include "mhp3rd_profile.hpp"

#include "psprecomp/runtime.hpp"
#include <string_view>

namespace mhp3rd {

// Loads the overlay libraries and installs the dispatch-miss hook that
// recognises the overlay currently in a slot and registers its recompiled
// corpus. The libraries are read from MHP3RD_OVERLAY_DIR, or from overlays/
// next to the executable. With MHP3RD_DUMP_OVERLAYS set, an unknown overlay is
// written out instead so it can be recompiled.
void install_overlay_support(psprecomp::Runtime &runtime);

// Drops the corpus of any slot whose contents no longer match it. The guest
// flushes the instruction cache right after loading an overlay, which is when
// this is called; the next jump into the slot then installs the right corpus.
void revalidate_overlays(psprecomp::Runtime &runtime);

// The guest loaded code: a slot whose image matched no corpus is searched
// again at the next call into it.
void forget_unmatched_overlays();

// Read-only identities at a guest instruction-cache invalidation. Epochs are
// conservative validation boundaries, including identical-image reloads.
void observe_overlay_code_epoch(psprecomp::Runtime &runtime, std::string_view reason) noexcept;

} // namespace mhp3rd
