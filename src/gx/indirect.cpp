#include "starlet/gx/indirect.hpp"

#include <stdexcept>
#include <string>

namespace starlet::gx {
namespace {
std::int32_t signed11(std::uint32_t value) {
    return static_cast<std::int32_t>(value & 1023) - static_cast<std::int32_t>(value & 1024);
}
std::int32_t floor_div(std::int32_t value, unsigned shift) {
    const std::int32_t divisor = 1 << shift;
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}
}
IndirectCapture capture_indirect(const State& state, const TevPipeline& tev) {
    auto fail = [](const char* message) {
        return IndirectCapture{{TevError::unsupported_state, message}, {}};
    };
    const auto count = (state.bp(0) >> 16) & 7;
    if (count > 4 || !tev.stage_count || tev.stage_count > 16)
        return fail("invalid indirect or TEV stage count");
    auto result = std::make_shared<IndirectSnapshot>();
    result->state_revision = state.revision();
    for (unsigned i = 0; i < tev.stage_count; ++i) {
        const auto word = state.bp(0x10 + i);
        // Lookup format and bias are unused when offset, wrapping and bump alpha are off.
        if (!(word & ~0x7fu)) continue;
        const unsigned lookup = word & 3, wrap_s = (word >> 13) & 7, wrap_t = (word >> 16) & 7;
        unsigned matrix = (word >> 9) & 15;
        if ((word & ~0x7fe73u) || ((word >> 2) & 3) || matrix > 3 || wrap_s == 7 || wrap_t == 7)
            return fail("indirect mapping requires 8-bit static matrices without feedback or bump alpha");
        // A lookup beyond the enabled indirect stages is undefined on the console; it
        // adds no offset here, while the wrap modes still apply.
        if (lookup >= count) matrix = 0;
        if (!tev_stage_samples(tev, i) || (!matrix && !wrap_s && !wrap_t)) continue;
        result->stages[i] = {static_cast<std::uint8_t>(matrix ? lookup : 0), static_cast<std::uint8_t>(matrix),
                             static_cast<std::uint8_t>(matrix ? (word >> 4) & 7 : 0), static_cast<std::uint8_t>(wrap_s),
                             static_cast<std::uint8_t>(wrap_t)};
        if (matrix) result->lookup_mask |= 1u << lookup;
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto order = state.bp(0x27) >> (i * 6);
        const auto scale = state.bp(0x25 + i / 2) >> ((i & 1) * 8);
        result->lookups[i] = {std::uint8_t(order & 7), std::uint8_t((order >> 3) & 7),
                              std::uint8_t(scale & 15), std::uint8_t((scale >> 4) & 15)};
    }
    for (unsigned m = 0; m < 3; ++m) {
        unsigned exponent = 0;
        for (unsigned c = 0; c < 3; ++c) {
            const auto word = state.bp(6 + m * 3 + c);
            result->matrices[m].rows[0][c] = signed11(word);
            result->matrices[m].rows[1][c] = signed11(word >> 11);
            // The scale exponent has five bits: the third column's upper bit (bit 23), which the SDK
            // still writes for exponents above 14, has no effect. Twilight Princess's heat haze relies on it.
            exponent |= ((word >> 22) & (c == 2 ? 1u : 3u)) << (c * 2);
        }
        for (auto& row : result->matrices[m].rows) row[3] = static_cast<int>(exponent) - 17;
    }
    return {{}, std::move(result)};
}

std::array<std::int32_t, 2> evaluate_indirect_offset(const IndirectMatrix& matrix, TevColor sample,
                                                   std::uint8_t bias) {
    if (bias > 7) throw std::invalid_argument("invalid indirect bias mask");
    const std::array<int, 3> input{{int(sample[3]) - ((bias & 1) ? 128 : 0),
        int(sample[2]) - ((bias & 2) ? 128 : 0), int(sample[1]) - ((bias & 4) ? 128 : 0)}};
    std::array<std::int32_t, 2> output{};
    for (unsigned r = 0; r < 2; ++r) {
        const auto& row = matrix.rows[r];
        if (row[3] < -17 || row[3] > 46) throw std::invalid_argument("invalid indirect exponent");
        std::int32_t dot = 0;
        for (unsigned c = 0; c < 3; ++c) {
            if (row[c] < -1024 || row[c] > 1023) throw std::invalid_argument("invalid indirect coefficient");
            dot += row[c] * input[c];
        }
        const auto fixed = floor_div(dot, 3);
        const std::uint32_t scaled = row[3] < 0 ? static_cast<std::uint32_t>(floor_div(fixed, -row[3])) :
            row[3] < 24 ? static_cast<std::uint32_t>(fixed) << row[3] : 0;
        output[r] = static_cast<std::int32_t>(scaled & 0x7fffff) - static_cast<std::int32_t>(scaled & 0x800000);
    }
    return output;
}

std::string indirect_wrap_glsl(const IndirectStage& stage, const std::string& base) {
    if (!stage.wrap_s && !stage.wrap_t) return base;
    const auto lane = [&](unsigned code, const char* component) -> std::string {
        if (!code) return base + "." + component;
        if (code >= 6) return "0";
        return "(" + base + "." + component + "&" + std::to_string((256 >> (code - 1)) * 128 - 1) + ")";
    };
    return "ivec2(" + lane(stage.wrap_s, "x") + "," + lane(stage.wrap_t, "y") + ")";
}

const char* indirect_offset_glsl() {
    return R"(
int gxIndirectRow(ivec3 value, ivec4 row) {
    int fixedValue = (row.x*value.x + row.y*value.y + row.z*value.z) >> 3;
    uint scaled = row.w < 0 ? uint(fixedValue >> (-row.w)) :
        (row.w < 24 ? uint(fixedValue) << row.w : 0u);
    return int(scaled & 0x7fffffu) - int(scaled & 0x800000u);
}
ivec2 gxIndirectOffset(ivec4 sampleValue, ivec4 rowS, ivec4 rowT, ivec3 bias) {
    ivec3 value = sampleValue.abg - bias;
    return ivec2(gxIndirectRow(value,rowS), gxIndirectRow(value,rowT));
}
)";
}
} // namespace starlet::gx
