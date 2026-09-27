#include "native/vector_metrics.hpp"

#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
using namespace mhp3rd::native;
using Vector = std::array<std::uint32_t, 3>;
void require(bool condition) {
    if (!condition) throw std::runtime_error("Vector metric core regression");
}
std::uint32_t bits(float x) { return std::bit_cast<std::uint32_t>(x); }
void exact_geometry() {
    const Vector a{bits(3), bits(4), bits(12)}, b{bits(6), bits(8), bits(24)};
    require(vector_metric(VectorMetric::Norm, a).scalar == bits(13));
    require(vector_metric(VectorMetric::NormSquared, a).scalar == bits(169));
    require(vector_metric(VectorMetric::Distance, a, b).scalar == bits(13));
    const auto squared = vector_metric(VectorMetric::DistanceSquared, a, b);
    require(squared.scalar == bits(169));
    require(squared.components == Vector{bits(-3), bits(-4), bits(-12)});
    require(vector_metric(VectorMetric::Norm, a).components == a);
    require(vector_metric(VectorMetric::Distance, a, a).scalar == 0u);
    const Vector zero{0x80000000u, 0u, 0x80000000u};
    require(vector_metric(VectorMetric::NormSquared, zero).scalar == 0u);
    require(vector_metric(VectorMetric::NormSquared, zero).components == zero);
    require(vector_metric(VectorMetric::Norm, {0x7f7fffffu, 0u, 0u}).scalar == 0x7f800000u);
    require(vector_metric(VectorMetric::Norm, {1u, 0u, 0u}).scalar == 0u);
}
void original_aot_witnesses() {
    // Seeded synthetic vectors captured by the production-object oracle. Each
    // expected result differs from the bounded interpreter's result; deriving
    // it through the new implementation would hide a changed FMA order.
    require(vector_metric(VectorMetric::Norm,
        {0x44632982u, 0xc44084f3u, 0x445273b0u}).scalar == 0x44b650dfu);
    require(vector_metric(VectorMetric::NormSquared,
        {0xc43800a6u, 0x441ab6fau, 0x42c85050u}).scalar == 0x4964348cu);
    require(vector_metric(VectorMetric::Distance,
        {0xc409b593u, 0x442829aeu, 0xc3a2d2feu},
        {0x4457d800u, 0xc33cdc88u, 0xc2d0bed0u}).scalar == 0x44d0d5aeu);
    require(vector_metric(VectorMetric::DistanceSquared,
        {0x43bec5bcu, 0x432e93d8u, 0x43ec55f4u},
        {0x436a06a8u, 0x43a2cec0u, 0xc29bde70u}).scalar == 0x48a9cba3u);
}
} // namespace

int main() {
    try {
        exact_geometry();
        original_aot_witnesses();
        std::cout << "vector metrics: exact geometry and four AOT rounding witnesses passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
