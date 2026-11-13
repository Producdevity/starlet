#pragma once

#include "starlet/gx/vertex.hpp"

namespace starlet::gx {

using TransformVector = std::array<float, 4>;
enum class TransformDiffuse : std::uint8_t { none, signed_, clamped };
enum class TransformAttenuation : std::uint8_t { none, spot, specular };
struct TransformChannel {
    bool enabled = false;
    bool material_vertex = false;
    bool ambient_vertex = false;
    std::uint8_t light_mask = 0;
    TransformDiffuse diffuse = TransformDiffuse::none;
    TransformAttenuation attenuation = TransformAttenuation::none;
};
struct TransformTexGen {
    std::uint8_t type = 0;
    std::uint8_t source_row = 0;
    bool input_abc1 = false;
    bool projected = false;
    bool normalize = false;
    std::uint8_t post_row = 0;
    // Emboss (type 1): the earlier texgen offset and the light whose direction offsets it.
    std::uint8_t emboss_source = 0;
    std::uint8_t emboss_light = 0;
};
struct TransformPipeline {
    // RGB0, RGB1, A0, A1, matching XF 0x100e..0x1011.
    std::array<TransformChannel, 4> channels{};
    std::array<TransformTexGen, 8> texgens{};
    std::uint8_t color_count = 0;
    std::uint8_t texgen_count = 0;
    std::uint8_t computed_colors = 0;
    std::uint8_t raster_colors = 3;
    std::uint8_t normal_components = 0;
    std::uint8_t vertex_colors = 0;
    std::uint8_t vertex_texcoords = 0;
    bool orthographic = false;
    bool dual_texture = false;
    bool clipping_disabled = false;
    // No normal attribute: lighting and texgens read the XF's latched vectors
    // (TransformUniforms::latched_normal) instead.
    bool latched_normal = false;
};
struct TransformLight {
    TransformVector color{}; // RGBA in 0..255.
    TransformVector angle{};
    TransformVector distance{};
    TransformVector position{};
    TransformVector direction{}; // Already negated by GXInitLightDir.
};
// All fields are vec4, with 80-byte light stride. No native pointer, GX State, texture
// memory or vertex storage is retained. Three std140 blocks of generate_transform_glsl
// lie in it at offsets every GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT divides: GXMatrices in its
// full form (position then normal rows), GXPost and GXTransform (projection to
// latched_normal). The viewport is for the CPU evaluator only.
struct alignas(16) TransformUniforms {
    std::array<TransformVector, 64> position{};
    std::array<TransformVector, 32> normal{};
    std::array<TransformVector, 64> post{};
    std::array<TransformVector, 2> projection{}; // Six coefficients in contiguous lanes.
    std::array<TransformVector, 2> ambient{}; // RGBA in 0..255.
    std::array<TransformVector, 2> material{};
    std::array<TransformLight, 8> lights{};
    // Normal, binormal and tangent last received by the XF; see latest_normal.
    std::array<TransformVector, 3> latched_normal{};
    std::array<TransformVector, 2> viewport{}; // sx,sy,sz,ox / oy,oz,0,0.
};
// GXMatrices: the rows a draw's matrix indices address, transform_matrix_rows vec4s. Its
// full form is TransformUniforms' position rows (0-63) then its normal rows (64-95).
inline constexpr unsigned transform_matrix_rows = 96;
inline constexpr std::size_t transform_matrix_bytes = transform_matrix_rows * 16;
inline constexpr std::size_t transform_post_offset = 96 * 16, transform_post_bytes = 64 * 16;
inline constexpr std::size_t transform_shared_offset = 160 * 16, transform_shared_bytes = 49 * 16;
// The GXMatrices row of the normal matrix in the full form, for position matrix row pn.
inline constexpr unsigned transform_normal_row(unsigned pn) { return 64 + pn % 32; }
struct TransformSnapshot {
    const TransformPipeline pipeline;
    const TransformUniforms uniforms;
    const std::uint64_t state_revision;
};
enum class TransformError {
    none, state_mismatch, unsupported_state, missing_input, invalid_matrix,
    invalid_input, unsupported_arithmetic
};
struct TransformStatus {
    TransformError error = TransformError::none;
    std::string message;
    explicit operator bool() const { return error == TransformError::none; }
};
struct TransformCapture {
    TransformStatus status;
    std::shared_ptr<const TransformSnapshot> snapshot;
    explicit operator bool() const { return static_cast<bool>(status); }
};
// Capture synchronously with vertices in CommandSink::draw. Copies only live XF
// matrix rows/lights into fixed-size storage (unused rows zero); checks layout,
// matrix bounds and finite live inputs without doing vertex math. Disabled
// vertices are ignored. No GX state is borrowed. Caller bounds aggregate
// retained snapshots. Allocation may throw.
// Reserved clip modes, matrix rows crossing memory end and
// non-finite live inputs are explicit errors. Value-dependent arithmetic
// validity and clip-disable equivalence require the diagnostic validator or
// the renderer's input contract.
// No last-vertex latch emulation: required absent attributes are rejected.
// raster_colors selects TEV consumers; color texgens retain their own dependencies.
TransformCapture capture_transform(const State&, const VertexSnapshot&, std::uint8_t raster_colors = 3);
// The pipeline half of capture_transform: XF registers and layout presence only,
// with the same rejection rules and no vertex, matrix or light value checks.
// latched_components (3 or 9) says the XF holds vectors from an earlier vertex; a layout
// without normals that needs them then reads those.
TransformStatus capture_transform_pipeline(const State&, const VertexLayout&, std::uint8_t raster_colors,
                                           TransformPipeline&, std::uint8_t latched_components = 0);
// Whether the pipeline reads object-space inputs besides the position: a lighting normal,
// an emboss offset, or a texgen from position, normal, binormal or tangent.
bool transform_reads_object_space(const TransformPipeline&);
// The vertex colour (0 or 1) that channel's vertex material and ambient read; 2: white.
unsigned transform_vertex_color_source(const TransformPipeline&, unsigned channel);
// Batched pipelines: every unlit channel with a register material reads it from the vertex
// colour instead. Returns those channels as bits RGB0, RGB1, A0, A1; when any, vertex colour c
// then feeds channel c and both are present.
std::uint8_t transform_vertex_materials(TransformPipeline&);
// The channels (bits RGB0, RGB1, A0, A1) whose material is the XF register, read as u_material.
std::uint8_t transform_register_materials(const TransformPipeline&);
// Whether the vertex stage reads the normal matrix: for lighting or an emboss texgen.
bool transform_reads_normal_matrix(const TransformPipeline&);
// The post-transform rows the vertex stage reads, as bits; nonzero declares GXPost.
std::uint64_t transform_post_rows(const TransformPipeline&);
// Writes into u's GXTransform fields the lanes the pipeline's vertex stage reads, from the
// XF, except latched_normal; every other lane keeps its value. Blocks that differ only in
// unread lanes then compare equal once both are built over the same previous block.
void direct_transform_shared(const State&, const TransformPipeline&, TransformUniforms&);
// Every XF matrix row, light and register value in the uniform layout, unvalidated.
// Rows a draw consumes equal capture_transform's; the GPU evaluates the values as given.
// Every byte of the block is written, unused lanes as zero, so it may be reused unreset.
void direct_transform_uniforms(const State&, TransformUniforms&);

// Float32 numerical model derived from GX equations, not original-hardware
// precision proof. Normals are normalized for lighting; texgen uses raw input.
// Light accumulation truncates the sum to signed integer before ambient/clamp;
// material multiplication stays float until the raster/TEV boundary. GPU FMA,
// rsqrt and original-GX rounding can differ. No blanket fast math is permitted.
enum class TransformArithmetic : std::uint8_t { float32_candidate_v1 = 1 };
struct TransformOutput {
    std::array<float, 3> eye_position{};
    std::array<float, 3> normal{}; // Normalized for active lighting; zero when unused.
    TransformVector gx_clip{};
    TransformVector gles_clip{};
    std::array<TransformVector, 2> colors{}; // Normalized 0..1; only computed_colors valid.
    std::array<std::array<float, 3>, 8> texcoords{}; // STQ; never divided per vertex.
};
struct TransformEvaluation {
    TransformStatus status;
    TransformOutput output;
    explicit operator bool() const { return static_cast<bool>(status); }
};
TransformEvaluation evaluate_transform(const TransformPipeline&, const TransformUniforms&,
                                       const ExpandedVertex&, TransformArithmetic);
// Scoped evaluator for one immutable draw snapshot. The snapshot must outlive
// this object. Pipeline checks and lighting dependencies are prepared once;
// every vertex retains the same value-dependent arithmetic checks.
class TransformEvaluator {
public:
    TransformEvaluator(const TransformSnapshot&, TransformArithmetic);
    const TransformStatus& status() const { return status_; }
    TransformEvaluation evaluate(const ExpandedVertex&) const;
private:
    const TransformSnapshot& snapshot_;
    TransformStatus status_;
    bool lighting_normal_ = false;
};
// Clip-disabled draws retain positive W and the GLES depth range; a draw with
// non-positive W at every vertex is discarded, as GLES clipping would. X/Y clipping
// preserves viewport pixels when scissor lies inside viewport; the GLES raster
// planner enforces that condition. Guard-band rasterization outside it is unsupported.
TransformStatus validate_transform_clipping(const TransformPipeline&, const TransformOutput&);
// Optional diagnostic pass over the same vertices used for capture. Executes
// the complete oracle and rejects degenerate/overflowing arithmetic. Not part
// of production capture. Does not retain inputs; allocation failures propagate.
TransformStatus validate_transform_vertices(const TransformSnapshot&, const VertexSnapshot&,
                                            TransformArithmetic);
struct TransformShader {
    TransformStatus status;
    std::vector<std::uint8_t> key; // Versioned explicit fields, no uniforms or matrix indices.
    std::string source;
    explicit operator bool() const { return static_cast<bool>(status); }
};
// Standalone GLES 3.1 vertex stage. std140 blocks, see TransformUniforms: GXTransform
// binding 0 (projection, colours, lights, latched vectors), GXMatrices binding 2
// (vec4 u_matrix[transform_matrix_rows]) and, when transform_post_rows is nonzero, GXPost
// binding 3 (vec4 u_post[64]). Inputs: pos=0, N/B/T=1..3, normalized RGBA=4..5, ST=6..13,
// uvec3 packed matrix bytes=14 (PN,TEX0,TEX1,NRM / TEX2..4 / TEX5..7): each a GXMatrices row,
// NRM the first of the normal matrix (transform_normal_row(PN) in the full form). A
// renderer may pack the rows a draw reads and give their packed rows instead.
// Output names: gx_eye, gx_normal, gx_clip, gx_gles_clip, gx_color0/1,
// gx_tex0..7. Inactive outputs are zero placeholders, not hardware defaults.
// Submit captured vertices with matching uniforms. Capture does not prove the
// value-dependent arithmetic domain: normalized vectors must have finite squared
// length >1e-30, attenuation denominators must have absolute value >1e-20, and
// all intermediates must stay finite. The explicit diagnostic validator checks
// these preconditions; the shader inserts no fabricated numerical fallback.
// GLES boundary: z'=2*z+w maps GX [-w,0] to GLES [-w,w]. X/Y are unchanged.
// Renderer owns viewport Y convention, depth range, scissor, EFB/presentation;
// viewport register values are preserved, never baked into clip coordinates.
// Keys are validated identically when source generation is deferred to a cache miss.
// batched: positions arrive in eye space and the four TEV registers as raw signed
// per-vertex values at locations 1-3 and 15, passed on as flat gx_register0..3;
// the pipeline must not consume normals or position texgen sources.
// affine: texgens whose q is 1 (2x4 or colour sources, no post transform but the
// identity one) output vec2 gx_texN without it. compact: 16-bit colour outputs.
// instanced: the draw's position matrix rows (pn..pn+2) and normal matrix rows come per
// instance from GXInstances (binding 1), transform_instance_rows vec4s per instance:
// three position rows, then three normal rows; texgen rows from GXMatrices. Emboss
// texgens (which read the normal matrix themselves) are rejected.
// fog (batched only, type | orthographic << 3 as for fast_fragment_source, no range
// adjustment): the vertex evaluates fast_fog_glsl at its own window depth into flat highp
// gx_fog, reading uGxFog and gl_DepthRange.
inline constexpr unsigned transform_instance_limit = 128;
inline constexpr unsigned transform_instance_rows = 6;
TransformShader generate_transform_glsl(const TransformPipeline&, TransformArithmetic, bool include_source = true,
                                        bool batched = false, std::uint8_t affine = 0, bool compact = false,
                                        bool instanced = false, std::uint8_t fog = 0);

} // namespace starlet::gx
