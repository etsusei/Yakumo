// Reuse the bounded original owner/load fixture for the separate G1b-read
// executable. The compile definition selects a read-only prefix gate and does
// not run the full copy/transform/retirement pump.
#include "texture_lifetime_oracle.cpp"
