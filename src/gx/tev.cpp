#include "starlet/gx/tev.hpp"

#include "starlet/gx/depth.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace starlet::gx {
namespace {
template<class T> unsigned code(T value) { return static_cast<unsigned>(value); }
std::uint8_t field(std::uint32_t word, unsigned shift, unsigned width) {
    return static_cast<std::uint8_t>((word >> shift) & ((1u << width) - 1));
}
TevStatus failure(TevError error, const char* message) { return {error, message}; }
bool comparison(TevOperation op) { return code(op) >= 8; }
unsigned compare_width(TevOperation op) { return (code(op) >> 1) & 3; }
bool alpha_uses_color(const TevStage& stage) {
    return comparison(stage.alpha.operation) && compare_width(stage.alpha.operation) < 3;
}
bool uses_color(const TevStage& stage, TevColorInput input) {
    return std::find(stage.color.inputs.begin(), stage.color.inputs.end(), input) != stage.color.inputs.end();
}
bool uses_alpha(const TevStage& stage, TevAlphaInput input) {
    const auto first = stage.alpha.inputs.begin() + (alpha_uses_color(stage) ? 2 : 0);
    return std::find(first, stage.alpha.inputs.end(), input) != stage.alpha.inputs.end();
}
bool uses_texture(const TevStage& stage) {
    return uses_color(stage, TevColorInput::texture_rgb) || uses_color(stage, TevColorInput::texture_alpha) ||
           uses_alpha(stage, TevAlphaInput::texture);
}
bool uses_raster(const TevStage& stage) {
    return uses_color(stage, TevColorInput::raster_rgb) || uses_color(stage, TevColorInput::raster_alpha) ||
           uses_alpha(stage, TevAlphaInput::raster);
}
bool valid_konst(const TevKonstSelection& selection, bool alpha) {
    switch (selection.kind) {
    case TevKonstKind::fraction: return selection.index < 8 && selection.component == 0;
    case TevKonstKind::rgb: return !alpha && selection.index < 4 && selection.component == 0;
    case TevKonstKind::component: return selection.index < 4 && selection.component < 4;
    }
    return false;
}
template<class Input> bool valid_equation(const TevEquation<Input>& equation, unsigned inputs) {
    const auto op = code(equation.operation);
    if (op > 15 || (op > 1 && op < 8) || code(equation.bias) > 2 || code(equation.scale) > 3 ||
        code(equation.destination) > 3) return false;
    if (comparison(equation.operation) && (equation.bias != TevBias::zero || equation.scale != TevScale::one)) return false;
    return std::all_of(equation.inputs.begin(), equation.inputs.end(),
                       [inputs](Input input) { return code(input) < inputs; });
}
TevStatus validate(const TevPipeline& pipeline, TevArithmeticModel model) {
    if (model != TevArithmeticModel::fixed8_candidate_v1)
        return failure(TevError::unsupported_arithmetic, "unknown TEV arithmetic model");
    if (pipeline.stage_count == 0 || pipeline.stage_count > 16 || code(pipeline.alpha_logic) > 3 ||
        code(pipeline.alpha_compare[0]) > 7 || code(pipeline.alpha_compare[1]) > 7)
        return failure(TevError::invalid_pipeline, "invalid TEV stage count or alpha test");
    for (unsigned i = 0; i < pipeline.stage_count; ++i) {
        const auto& stage = pipeline.stages[i];
        if (!valid_equation(stage.color, 16) || !valid_equation(stage.alpha, 8) ||
            !valid_konst(stage.color_konst, false) || !valid_konst(stage.alpha_konst, true) ||
            stage.texture_map > 7 || stage.texture_coord > 7 ||
            (stage.raster != TevRaster::color0 && stage.raster != TevRaster::color1 && stage.raster != TevRaster::zero) ||
            std::any_of(stage.texture_swap.begin(), stage.texture_swap.end(), [](unsigned v) { return v > 3; }) ||
            std::any_of(stage.raster_swap.begin(), stage.raster_swap.end(), [](unsigned v) { return v > 3; }))
            return failure(TevError::invalid_pipeline, "invalid TEV stage field");
    }
    if (pipeline.disabled_texture != 0 && pipeline.disabled_texture != 255)
        return failure(TevError::invalid_pipeline, "invalid disabled-lookup texture value");
    if (pipeline.z_texture_op > 2 || pipeline.z_texture_type > 2 || (!pipeline.z_texture_op && pipeline.z_texture_type))
        return failure(TevError::invalid_pipeline, "invalid Z-texture op or type");
    if (pipeline.z_texture_op) {
        const auto& last = pipeline.stages[pipeline.stage_count - 1];
        if (!last.texture_enabled)
            return failure(TevError::unsupported_state, "a Z texture needs a lookup in the last TEV stage");
        if (uses_texture(last))
            return failure(TevError::unsupported_state, "the Z texture occupies the last TEV stage's texture input");
    }
    return {};
}

template<class Input> TevEquation<Input> decode_equation(std::uint32_t word, bool alpha) {
    TevEquation<Input> equation;
    for (unsigned i = 0; i < 4; ++i)
        equation.inputs[i] = static_cast<Input>(field(word, alpha ? 13 - 3 * i : 12 - 4 * i, alpha ? 3 : 4));
    const auto bias = field(word, 16, 2), scale = field(word, 20, 2), subtract = field(word, 18, 1);
    equation.operation = static_cast<TevOperation>(bias == 3 ? 8 + 2 * scale + subtract : subtract);
    equation.bias = bias == 3 ? TevBias::zero : static_cast<TevBias>(bias);
    equation.scale = bias == 3 ? TevScale::one : static_cast<TevScale>(scale);
    equation.clamp = field(word, 19, 1) != 0;
    equation.destination = static_cast<TevRegister>(field(word, 22, 2));
    return equation;
}
bool decode_konst(unsigned selector, bool alpha, TevKonstSelection& selection) {
    if (selector < 8) selection = {TevKonstKind::fraction, static_cast<std::uint8_t>(selector), 0};
    else if (!alpha && selector >= 12 && selector <= 15)
        selection = {TevKonstKind::rgb, static_cast<std::uint8_t>(selector - 12), 0};
    else if (selector >= 16 && selector <= 31)
        selection = {TevKonstKind::component, static_cast<std::uint8_t>(selector & 3), static_cast<std::uint8_t>((selector - 16) / 4)};
    else return false;
    return true;
}
std::int16_t signed11(std::uint32_t value) {
    value &= 2047;
    return static_cast<std::int16_t>(value < 1024 ? static_cast<int>(value) : static_cast<int>(value) - 2048);
}

// Provisional fixed8_candidate_v1: these quantization choices are explicit
// inference, not established GX TEV precision. Keep CPU and GLSL policy together.
struct Fixed8Candidate {
    static int floor_div(int numerator, int denominator) {
        return numerator >= 0 ? numerator / denominator : -((-numerator + denominator - 1) / denominator);
    }
    static int linear(int a, int b, int c, int d, bool subtract, TevBias bias, TevScale scale) {
        const int factor = c + (c >> 7);
        const int mix = a * (256 - factor) + b * factor;
        const int offset = bias == TevBias::add_half ? 128 : (bias == TevBias::subtract_half ? -128 : 0);
        int numerator = (d + offset) * 256 + (subtract ? -mix : mix);
        const int denominator = scale == TevScale::half ? 512 : 256;
        if (scale != TevScale::half) numerator *= 1 << code(scale);
        return floor_div(numerator + denominator / 2, denominator); // nearest, ties toward +infinity.
    }
    static int clamp(int value, bool enabled) { return std::clamp(value, enabled ? 0 : -1024, enabled ? 255 : 1023); }
    static std::uint8_t output(int value) { return static_cast<std::uint8_t>(static_cast<unsigned>(value) & 255); }
    static int fraction(unsigned index) { return std::min(255, 256 - static_cast<int>(index) * 32); }
    static const char* glsl() {
        return R"glsl(
// fixed8_candidate_v1; original-GX quantization remains unverified.
// Signed >> sign-extends (GLSL ES 3.00 5.9), so it is floor division by 256 or 512.
int gxTevLinear(int a, int b, int c, int d, bool sub, int bias, int scale) {
    int factor = c + (c >> 7);
    int mixValue = a * (256 - factor) + b * factor;
    int offset = bias == 1 ? 128 : (bias == 2 ? -128 : 0);
    int n = (d + offset) * 256 + (sub ? -mixValue : mixValue);
    if (scale == 3) return (n + 256) >> 9;
    return (n * (1 << scale) + 128) >> 8;
}
ivec4 gxTevOutput(ivec4 value) { return value & ivec4(255); }
)glsl";
    }
};
using IntColor = std::array<int, 4>;
IntColor widen(const TevColor& value) { return {{value[0], value[1], value[2], value[3]}}; }
IntColor swapped(const TevColor& value, const std::array<std::uint8_t, 4>& swizzle) {
    return {{value[swizzle[0]], value[swizzle[1]], value[swizzle[2]], value[swizzle[3]]}};
}
IntColor constant(const TevKonstSelection& selection, const TevUniforms& uniforms) {
    if (selection.kind == TevKonstKind::rgb) return widen(uniforms.konst[selection.index]);
    const int value = selection.kind == TevKonstKind::fraction ? Fixed8Candidate::fraction(selection.index)
                      : uniforms.konst[selection.index][selection.component];
    return {{value, value, value, value}};
}
IntColor color_input(TevColorInput input, const std::array<IntColor, 4>& registers,
                     const IntColor& texture, const IntColor& raster, const IntColor& konst) {
    const unsigned value = code(input);
    if (value < 8) {
        const auto& reg = registers[value / 2];
        return (value & 1) ? IntColor{{reg[3], reg[3], reg[3], reg[3]}} : reg;
    }
    if (input == TevColorInput::texture_rgb) return texture;
    if (input == TevColorInput::raster_rgb) return raster;
    if (input == TevColorInput::konst) return konst;
    const int scalar = input == TevColorInput::texture_alpha ? texture[3] :
                       input == TevColorInput::raster_alpha ? raster[3] :
                       input == TevColorInput::one ? 255 : input == TevColorInput::half ? 128 : 0;
    return {{scalar, scalar, scalar, scalar}};
}
int alpha_input(TevAlphaInput input, const std::array<IntColor, 4>& registers,
                const IntColor& texture, const IntColor& raster, const IntColor& konst) {
    if (code(input) < 4) return registers[code(input)][3];
    switch (input) {
    case TevAlphaInput::texture: return texture[3];
    case TevAlphaInput::raster: return raster[3];
    case TevAlphaInput::konst: return konst[3];
    default: return 0;
    }
}
bool tev_compare(TevOperation op, const IntColor& a, const IntColor& b, unsigned component) {
    const unsigned width = compare_width(op);
    unsigned left = 0, right = 0;
    if (width == 3) { left = a[component]; right = b[component]; }
    else for (unsigned i = 0; i <= width; ++i) { left |= unsigned(a[i]) << (i * 8); right |= unsigned(b[i]) << (i * 8); }
    return (code(op) & 1) ? left == right : left > right;
}
bool alpha_compare(TevCompare op, int alpha, int reference) {
    switch (op) {
    case TevCompare::never: return false;
    case TevCompare::less: return alpha < reference;
    case TevCompare::equal: return alpha == reference;
    case TevCompare::less_equal: return alpha <= reference;
    case TevCompare::greater: return alpha > reference;
    case TevCompare::not_equal: return alpha != reference;
    case TevCompare::greater_equal: return alpha >= reference;
    case TevCompare::always: return true;
    }
    return false;
}
bool alpha_logic(TevAlphaLogic op, bool a, bool b) {
    switch (op) {
    case TevAlphaLogic::and_: return a && b;
    case TevAlphaLogic::or_: return a || b;
    case TevAlphaLogic::xor_: return a != b;
    case TevAlphaLogic::xnor: return a == b;
    }
    return false;
}

std::vector<std::uint8_t> pipeline_key(const TevPipeline& pipeline, TevArithmeticModel model) {
    std::vector<std::uint8_t> key;
    key.reserve(640);
    auto put = [&](unsigned value) { key.push_back(static_cast<std::uint8_t>(value)); };
    put(1); put(code(model)); put(pipeline.stage_count);
    put(code(pipeline.alpha_compare[0])); put(code(pipeline.alpha_compare[1])); put(code(pipeline.alpha_logic));
    for (unsigned i = 0; i < pipeline.stage_count; ++i) {
        const auto& stage = pipeline.stages[i];
        auto equation = [&](const auto& eq) {
            for (auto input : eq.inputs) put(code(input));
            put(code(eq.operation)); put(code(eq.bias)); put(code(eq.scale)); put(eq.clamp); put(code(eq.destination));
        };
        auto konst = [&](const TevKonstSelection& k) { put(code(k.kind)); put(k.index); put(k.component); };
        equation(stage.color); equation(stage.alpha); konst(stage.color_konst); konst(stage.alpha_konst);
        for (auto swap : stage.texture_swap) put(swap);
        for (auto swap : stage.raster_swap) put(swap);
        // 2: the stage reads its disabled lookup as 0; earlier keys keep their bytes.
        put(stage.texture_enabled ? 1 : uses_texture(stage) && !pipeline.disabled_texture ? 2 : 0);
        put(stage.texture_map); put(stage.texture_coord);
    }
    if (pipeline.z_texture_op) { put('Z'); put(pipeline.z_texture_op); put(pipeline.z_texture_type); }
    return key;
}
std::string swizzle(const std::array<std::uint8_t, 4>& swap) {
    std::string out;
    for (auto component : swap) out += "rgba"[component];
    return out;
}
std::string konst_expression(const TevKonstSelection& selection, bool alpha) {
    if (selection.kind == TevKonstKind::fraction) {
        const auto value = std::to_string(Fixed8Candidate::fraction(selection.index));
        return alpha ? value : "ivec3(" + value + ")";
    }
    const auto reg = "uGxTevKonst[" + std::to_string(selection.index) + "]";
    if (selection.kind == TevKonstKind::rgb) return reg + ".rgb";
    const auto scalar = reg + "." + "rgba"[selection.component];
    return alpha ? scalar : "ivec3(" + scalar + ")";
}
std::string color_expression(TevColorInput input) {
    const unsigned value = code(input);
    if (value < 8) return "r" + std::to_string(value / 2) + ((value & 1) ? ".aaa" : ".rgb");
    switch (input) {
    case TevColorInput::texture_rgb: return "tex.rgb";
    case TevColorInput::texture_alpha: return "tex.aaa";
    case TevColorInput::raster_rgb: return "ras.rgb";
    case TevColorInput::raster_alpha: return "ras.aaa";
    case TevColorInput::one: return "ivec3(255)";
    case TevColorInput::half: return "ivec3(128)";
    case TevColorInput::konst: return "kc";
    default: return "ivec3(0)";
    }
}
std::string alpha_expression(TevAlphaInput input) {
    if (code(input) < 4) return "r" + std::to_string(code(input)) + ".a";
    switch (input) {
    case TevAlphaInput::texture: return "tex.a";
    case TevAlphaInput::raster: return "ras.a";
    case TevAlphaInput::konst: return "ka";
    default: return "0";
    }
}
std::string compare_expression(TevOperation op, const std::string& a, const std::string& b, unsigned component) {
    const unsigned width = compare_width(op);
    auto operand = [&](const std::string& name) {
        if (width == 3) return name + "." + "rgb"[component];
        std::string out = "(" + name + ".r";
        if (width >= 1) out += " | (" + name + ".g << 8)";
        if (width >= 2) out += " | (" + name + ".b << 16)";
        return out + ")";
    };
    return "(" + operand(a) + ((code(op) & 1) ? " == " : " > ") + operand(b) + ")";
}
std::string linear_expression(const std::string& a, const std::string& b, const std::string& c, const std::string& d,
                              TevOperation operation, TevBias bias, TevScale scale) {
    return "gxTevLinear(" + a + ", " + b + ", " + c + ", " + d + ", " +
           (operation == TevOperation::subtract ? "true" : "false") + ", " +
           std::to_string(code(bias)) + ", " + std::to_string(code(scale)) + ")";
}
std::string alpha_comparison_expression(TevCompare op, unsigned index) {
    if (op == TevCompare::always) return "true";
    if (op == TevCompare::never) return "false";
    const char* operators[] = {"", "<", "==", "<=", ">", "!=", ">=", ""};
    return "(alpha " + std::string(operators[code(op)]) + " uGxTevAlphaRef[" + std::to_string(index) + "])";
}
} // namespace

TevInputUse tev_input_use(const TevStage& stage) {
    const bool texture = uses_texture(stage);
    return {texture && stage.texture_enabled, static_cast<std::uint8_t>(uses_raster(stage) && stage.raster != TevRaster::zero
        ? 1u << static_cast<unsigned>(stage.raster) : 0), texture && !stage.texture_enabled};
}

bool tev_stage_samples(const TevPipeline& pipeline, unsigned stage) {
    return stage < pipeline.stage_count &&
           ((uses_texture(pipeline.stages[stage]) && pipeline.stages[stage].texture_enabled) ||
            (pipeline.z_texture_op && stage + 1u == pipeline.stage_count));
}

TevUniforms capture_tev_uniforms(const State& state) {
    TevUniforms uniforms;
    for (unsigned i = 0; i < 4; ++i) {
        const auto low = state.tev_register(false, i * 2), high = state.tev_register(false, i * 2 + 1);
        uniforms.registers[i] = {{signed11(low), signed11(high >> 12), signed11(high), signed11(low >> 12)}};
        const auto klow = state.tev_register(true, i * 2), khigh = state.tev_register(true, i * 2 + 1);
        uniforms.konst[i] = {{field(klow, 0, 8), field(khigh, 12, 8), field(khigh, 0, 8), field(klow, 12, 8)}};
    }
    const auto test = state.bp(0xf3);
    uniforms.alpha_reference = {{field(test, 0, 8), field(test, 8, 8)}};
    ZTexture z;
    if (decode_z_texture(state.bp(0xf4), state.bp(0xf5), z)) uniforms.z_texture_bias = z.bias;
    return uniforms;
}

TevCapture capture_tev(const State& state) {
    TevPipeline pipeline;
    auto uniforms = capture_tev_uniforms(state);
    pipeline.stage_count = field(state.bp(0x00), 10, 4) + 1;
    pipeline.disabled_texture = field(state.bp(0x00), 0, 4) ? 255 : 0;
    ZTexture z;
    if (!decode_z_texture(state.bp(0xf4), state.bp(0xf5), z))
        return {failure(TevError::unsupported_state, "reserved Z-texture state"), {}};
    pipeline.z_texture_op = z.op; pipeline.z_texture_type = z.type;
    for (unsigned i = 0; i < pipeline.stage_count; ++i) {
        auto& stage = pipeline.stages[i];
        const auto color = state.bp(0xc0 + i * 2), alpha = state.bp(0xc1 + i * 2);
        stage.color = decode_equation<TevColorInput>(color, false);
        stage.alpha = decode_equation<TevAlphaInput>(alpha, true);
        const auto order = state.bp(0x28 + i / 2) >> ((i & 1) * 12);
        stage.texture_map = field(order, 0, 3); stage.texture_coord = field(order, 3, 3);
        stage.texture_enabled = field(order, 6, 1) != 0;
        const auto raster = field(order, 7, 3);
        if (raster != 0 && raster != 1 && raster != 7)
            return {failure(TevError::unsupported_state, "bump or reserved TEV raster channel is unsupported"), {}};
        stage.raster = static_cast<TevRaster>(raster);
        const auto ksel = state.bp(0xf6 + i / 2);
        if (uses_color(stage, TevColorInput::konst) &&
            !decode_konst(field(ksel, 4 + (i & 1) * 10, 5), false, stage.color_konst))
            return {failure(TevError::unsupported_state, "reserved TEV color konst selector"), {}};
        if (uses_alpha(stage, TevAlphaInput::konst) &&
            !decode_konst(field(ksel, 9 + (i & 1) * 10, 5), true, stage.alpha_konst))
            return {failure(TevError::unsupported_state, "reserved TEV alpha konst selector"), {}};
        auto swap = [&](unsigned table) {
            const auto rg = state.bp(0xf6 + table * 2), ba = state.bp(0xf7 + table * 2);
            return std::array<std::uint8_t, 4>{{field(rg, 0, 2), field(rg, 2, 2), field(ba, 0, 2), field(ba, 2, 2)}};
        };
        stage.raster_swap = swap(field(alpha, 0, 2));
        stage.texture_swap = swap(field(alpha, 2, 2));
    }
    const auto test = state.bp(0xf3);
    pipeline.alpha_compare = {{static_cast<TevCompare>(field(test, 16, 3)), static_cast<TevCompare>(field(test, 19, 3))}};
    pipeline.alpha_logic = static_cast<TevAlphaLogic>(field(test, 22, 2));
    const auto status = validate(pipeline, TevArithmeticModel::fixed8_candidate_v1);
    if (!status) return {status, {}};
    return {{}, std::make_shared<const TevMaterial>(TevMaterial{std::move(pipeline), std::move(uniforms), state.revision()})};
}

TevEvaluation evaluate_tev(const TevPipeline& pipeline, const TevUniforms& uniforms,
                           const TevInputs& inputs, TevArithmeticModel model) {
    TevEvaluation result;
    result.status = validate(pipeline, model);
    if (!result.status) return result;
    std::array<IntColor, 4> registers{};
    for (unsigned i = 0; i < 4; ++i) for (unsigned c = 0; c < 4; ++c) {
        const int value = uniforms.registers[i][c];
        if (value < -1024 || value > 1023) {
            result.status = failure(TevError::invalid_uniform, "TEV register value exceeds signed11 range");
            return result;
        }
        registers[i][c] = value;
    }
    for (unsigned i = 0; i < pipeline.stage_count; ++i) {
        const auto& stage = pipeline.stages[i];
        if (uses_texture(stage) && stage.texture_enabled && !(inputs.texture_valid & (1u << i))) {
            result.status = failure(TevError::missing_input, "required TEV texture sample is absent"); return result;
        }
        if (uses_raster(stage) && stage.raster != TevRaster::zero && !(inputs.raster_valid & (1u << code(stage.raster)))) {
            result.status = failure(TevError::missing_input, "required TEV raster color is absent"); return result;
        }
        const int disabled = pipeline.disabled_texture;
        const IntColor texture = stage.texture_enabled ? swapped(inputs.texture[i], stage.texture_swap)
                                                       : IntColor{{disabled, disabled, disabled, disabled}};
        const IntColor raster = stage.raster == TevRaster::zero ? IntColor{} : swapped(inputs.raster[code(stage.raster)], stage.raster_swap);
        const auto kc = constant(stage.color_konst, uniforms), ka = constant(stage.alpha_konst, uniforms);
        std::array<IntColor, 4> color{};
        std::array<int, 4> alpha{};
        for (unsigned input = 0; input < 4; ++input) {
            color[input] = color_input(stage.color.inputs[input], registers, texture, raster, kc);
            alpha[input] = alpha_input(stage.alpha.inputs[input], registers, texture, raster, ka);
            if (input < 3) {
                for (auto& component : color[input]) component = static_cast<unsigned>(component) & 255;
                alpha[input] = static_cast<unsigned>(alpha[input]) & 255;
            }
        }
        IntColor rgb{};
        for (unsigned c = 0; c < 3; ++c) {
            const auto& eq = stage.color;
            const int value = comparison(eq.operation)
                ? color[3][c] + (tev_compare(eq.operation, color[0], color[1], c) ? color[2][c] : 0)
                : Fixed8Candidate::linear(color[0][c], color[1][c], color[2][c], color[3][c],
                                          eq.operation == TevOperation::subtract, eq.bias, eq.scale);
            rgb[c] = Fixed8Candidate::clamp(value, eq.clamp);
        }
        const auto& eq = stage.alpha;
        int value;
        if (comparison(eq.operation)) {
            const bool pass = alpha_uses_color(stage) ? tev_compare(eq.operation, color[0], color[1], 0)
                : ((code(eq.operation) & 1) ? alpha[0] == alpha[1] : alpha[0] > alpha[1]);
            value = alpha[3] + (pass ? alpha[2] : 0);
        } else value = Fixed8Candidate::linear(alpha[0], alpha[1], alpha[2], alpha[3],
                                               eq.operation == TevOperation::subtract, eq.bias, eq.scale);
        // Both equations consume the incoming registers before either destination is updated.
        for (unsigned c = 0; c < 3; ++c) registers[code(stage.color.destination)][c] = rgb[c];
        registers[code(stage.alpha.destination)][3] = Fixed8Candidate::clamp(value, eq.clamp);
    }
    for (unsigned i = 0; i < 4; ++i) for (unsigned c = 0; c < 4; ++c)
        result.registers[i][c] = static_cast<std::int16_t>(registers[i][c]);
    // The last stage's results leave the TEV, whichever registers it wrote.
    const auto& last = pipeline.stages[pipeline.stage_count - 1];
    for (unsigned c = 0; c < 3; ++c) result.color[c] = Fixed8Candidate::output(registers[code(last.color.destination)][c]);
    result.color[3] = Fixed8Candidate::output(registers[code(last.alpha.destination)][3]);
    result.alpha_pass = alpha_logic(pipeline.alpha_logic,
        alpha_compare(pipeline.alpha_compare[0], result.color[3], uniforms.alpha_reference[0]),
        alpha_compare(pipeline.alpha_compare[1], result.color[3], uniforms.alpha_reference[1]));
    return result;
}

TevShader generate_tev_shader(const TevPipeline& pipeline, TevArithmeticModel model, bool include_source) {
    TevShader result;
    result.status = validate(pipeline, model);
    if (!result.status) return result;
    result.key = pipeline_key(pipeline, model);
    if (!include_source) return result;
    std::ostringstream glsl;
    glsl << "uniform highp ivec4 uGxTevRegisters[4];\nuniform highp ivec4 uGxTevKonst[4];\n"
            "uniform highp ivec2 uGxTevAlphaRef;\n" << Fixed8Candidate::glsl();
    glsl << "ivec4 gxTevEvaluate(in ivec4 textures[16], in ivec4 raster[2]) {\n";
    for (unsigned i = 0; i < 4; ++i) glsl << "    ivec4 r" << i << " = uGxTevRegisters[" << i << "];\n";
    for (unsigned i = 0; i < pipeline.stage_count; ++i) {
        const auto& stage = pipeline.stages[i];
        glsl << "    { // stage " << i << "\n        ivec4 tex = ";
        if (stage.texture_enabled || !uses_texture(stage)) glsl << "textures[" << i << "]." << swizzle(stage.texture_swap) << ";\n";
        else glsl << "ivec4(" << unsigned(pipeline.disabled_texture) << ");\n";
        glsl << "        ivec4 ras = " << (stage.raster == TevRaster::zero ? "ivec4(0)"
            : "raster[" + std::to_string(code(stage.raster)) + "]." + swizzle(stage.raster_swap)) << ";\n";
        glsl << "        ivec3 kc = " << konst_expression(stage.color_konst, false) << ";\n";
        glsl << "        int ka = " << konst_expression(stage.alpha_konst, true) << ";\n";
        for (unsigned input = 0; input < 4; ++input) {
            glsl << "        ivec3 c" << "abcd"[input] << " = " << color_expression(stage.color.inputs[input]);
            if (input < 3) glsl << " & ivec3(255)";
            glsl << ";\n        int a" << "abcd"[input] << " = " << alpha_expression(stage.alpha.inputs[input]);
            if (input < 3) glsl << " & 255";
            glsl << ";\n";
        }
        glsl << "        ivec3 nextColor;\n";
        for (unsigned component = 0; component < 3; ++component) {
            const std::string suffix = std::string(".") + "rgb"[component];
            const auto& eq = stage.color;
            const auto expression = comparison(eq.operation) ? "cd" + suffix + " + (" + compare_expression(eq.operation, "ca", "cb", component) +
                " ? cc" + suffix + " : 0)" : linear_expression("ca" + suffix, "cb" + suffix, "cc" + suffix, "cd" + suffix, eq.operation, eq.bias, eq.scale);
            glsl << "        nextColor" << suffix << " = clamp(" << expression << ", " << (eq.clamp ? 0 : -1024) << ", " << (eq.clamp ? 255 : 1023) << ");\n";
        }
        const auto& eq = stage.alpha;
        std::string expression;
        if (comparison(eq.operation)) {
            const auto condition = alpha_uses_color(stage) ? compare_expression(eq.operation, "ca", "cb", 0)
                : std::string("(aa ") + ((code(eq.operation) & 1) ? "==" : ">") + " ab)";
            expression = "ad + (" + condition + " ? ac : 0)";
        } else expression = linear_expression("aa", "ab", "ac", "ad", eq.operation, eq.bias, eq.scale);
        glsl << "        int nextAlpha = clamp(" << expression << ", " << (eq.clamp ? 0 : -1024) << ", " << (eq.clamp ? 255 : 1023) << ");\n";
        glsl << "        r" << code(stage.color.destination) << ".rgb = nextColor;\n        r" << code(eq.destination) << ".a = nextAlpha;\n    }\n";
    }
    const auto& last = pipeline.stages[pipeline.stage_count - 1];
    glsl << "    return gxTevOutput(ivec4(r" << code(last.color.destination) << ".rgb, r" << code(last.alpha.destination)
         << ".a));\n}\nbool gxTevAlphaPass(int alpha) {\n    return ";
    const char* logic[] = {" && ", " || ", " != ", " == "};
    glsl << alpha_comparison_expression(pipeline.alpha_compare[0], 0) << logic[code(pipeline.alpha_logic)]
         << alpha_comparison_expression(pipeline.alpha_compare[1], 1) << ";\n}\n";
    result.functions = glsl.str();
    result.fragment = "#version 310 es\nprecision highp float;\nprecision highp int;\n" + result.functions +
        "uniform highp ivec4 uGxTevTexture[16];\nuniform highp ivec4 uGxTevRaster[2];\n"
        "layout(location = 0) out vec4 gxFragmentColor;\nvoid main() {\n"
        "    ivec4 color = gxTevEvaluate(uGxTevTexture, uGxTevRaster);\n"
        "    if (!gxTevAlphaPass(color.a)) discard;\n    gxFragmentColor = vec4(color) / 255.0;\n}\n";
    return result;
}
} // namespace starlet::gx
