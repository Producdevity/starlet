#include "starlet/gx/transform.hpp"
#include <GLES3/gl32.h>
#include <algorithm>
#include <deque>
#include <map>
#include <array>
#include <string>
#include <cstring>
#include <vector>
namespace fake {
bool track_uniforms=false;
unsigned uniform_updates=0, negative_uniform_updates=0, transform_uploads=0;
starlet::gx::TransformUniforms uploaded_transform{};
std::vector<unsigned char> submitted_vertices;
std::vector<std::uint16_t> submitted_indices;
unsigned submitted_index_count=0;
GLuint current_program=0;
std::map<std::pair<GLuint,std::string>,GLint> uniform_locations;
std::map<GLint,std::array<double,4>> uniform_values;
GLenum error = GL_NO_ERROR;
GLenum wait_result = GL_ALREADY_SIGNALED;
// Mapped pack buffers hold byte i % 251 instead of zeros.
bool map_pattern = false;
// With map_pattern, a read into client memory gets the same pattern as a mapped pack
// buffer of read_pattern_row texels per row read from (0, 0).
int read_pattern_row = 0;
unsigned blits = 0;
GLuint next = 0, active = 0, bound[16]{}, samplers[16]{}, pack_buffer = 0, unpack_buffer = 0;
std::map<GLuint, unsigned char> first;
std::map<GLenum, GLint> pixel;
unsigned clears = 0;
GLuint framebuffer = 0;
std::map<GLuint, GLuint> color_attachments;
std::map<GLuint, GLuint> depth_attachments;
unsigned uniform_ranges = 0, read_pixels = 0;
// Compute dispatches, and the last int and ivec4 uniforms: for texel stores the buffer
// offset and source x, y, width and row stride, in texels.
unsigned dispatches = 0;
GLint last_int = 0;
std::array<GLint, 4> last_ivec4{};
// Instanced draws: count, last instance count, the binding-1 range and the uniform buffer's bytes.
unsigned instanced_draws = 0, instance_count = 0;
GLintptr instance_offset = 0;
GLsizeiptr instance_size = 0;
std::vector<unsigned char> uniform_bytes;
GLsizeiptr element_bytes = 0;
unsigned uploads = 0, draws = 0, draw_first = 0, sampler_seen = 0, pack_seen = 0, unpack_seen = 0, pack_pbo_seen = 0,
         unpack_pbo_seen = 0;
std::size_t pack_required = 0, unpack_required = 0;
GLint program_binary_formats = 0;
GLboolean binary_links = GL_TRUE, last_link = GL_TRUE, links = GL_TRUE;
GLenum binary_format = 0x1234;
std::vector<unsigned char> program_binary{1, 2, 3, 4, 5}, loaded_binary;
unsigned retrievable_hints = 0;
const char* renderer_name = "private dispatch fixture";
// Shader sources in compile order; depth state as last set.
std::vector<std::string> shader_sources;
std::vector<GLenum> invalidated;
GLenum depth_func = GL_LESS;
GLboolean depth_mask = GL_TRUE, depth_test = GL_FALSE;
} // namespace fake
namespace {
std::size_t transfer_bytes(GLsizei width, GLsizei height, bool pack) {
    const auto length = fake::pixel[pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH];
    const auto rows = fake::pixel[pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS];
    const auto pixels = fake::pixel[pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS];
    const auto alignment = fake::pixel[pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT];
    const std::size_t row_bytes = std::size_t(length ? length : width) * 4;
    const std::size_t a = alignment ? alignment : 4;
    const auto stride = (row_bytes + a - 1) / a * a;
    return std::size_t(rows + height - 1) * stride + std::size_t(pixels + width) * 4;
}
void uniform(GLint location,std::array<double,4> values) {
    if(!fake::track_uniforms)return;
    if(location<0){++fake::negative_uniform_updates;return;}
    ++fake::uniform_updates;fake::uniform_values[location]=values;
}
} // namespace
extern "C" {
void glActiveTexture(GLenum texture) { fake::active = texture - GL_TEXTURE0; }
void glAttachShader(GLuint program, GLuint shader) {}
void glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_PIXEL_PACK_BUFFER)
        fake::pack_buffer = buffer;
    if (target == GL_PIXEL_UNPACK_BUFFER)
        fake::unpack_buffer = buffer;
}
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {}
void* glMapBufferRange(GLenum, GLintptr offset, GLsizeiptr length, GLbitfield) {
    // Each mapping keeps its own bytes: renderers hold several snapshot buffers mapped at once.
    static std::deque<std::vector<unsigned char>> mappings;
    auto& mapped = mappings.emplace_back(std::size_t(length), 0);
    if (fake::map_pattern) for (GLsizeiptr i = 0; i < length; ++i) mapped[i] = static_cast<unsigned char>((offset + i) % 251);
    return mapped.data();
}
GLboolean glUnmapBuffer(GLenum) { return GL_TRUE; }
void glFlush() {}
void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    ++fake::uniform_ranges;
    if (index == 1) { fake::instance_offset = offset; fake::instance_size = size; }
}
void glDisableVertexAttribArray(GLuint index) {}
void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {}
void glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w) {}
void glBindFramebuffer(GLenum target, GLuint framebuffer) { if (target != GL_READ_FRAMEBUFFER) fake::framebuffer = framebuffer; }
void glBindRenderbuffer(GLenum target, GLuint renderbuffer) {}
void glBindSampler(GLuint unit, GLuint sampler) { fake::samplers[unit] = sampler; }
void glBindTexture(GLenum target, GLuint texture) { fake::bound[fake::active] = texture; }
void glBindVertexArray(GLuint array) {}
void glBlendEquation(GLenum mode) {}
void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {}
void glFrontFace(GLenum) {}
void glDepthFunc(GLenum func) { fake::depth_func = func; }
void glDepthRangef(GLfloat,GLfloat) {}
void glCullFace(GLenum) {}
void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {}
void glBlendFunc(GLenum sfactor, GLenum dfactor) {}
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                       GLint dstY1, GLbitfield mask, GLenum filter) { ++fake::blits; }
void glBufferSubData(GLenum, GLintptr, GLsizeiptr, const void*) {}
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    if(data && target==GL_ARRAY_BUFFER) fake::submitted_vertices.assign(static_cast<const unsigned char*>(data),static_cast<const unsigned char*>(data)+size);
    if(target==GL_ELEMENT_ARRAY_BUFFER) fake::element_bytes=size;
    if(data && target==GL_ELEMENT_ARRAY_BUFFER) fake::submitted_indices.assign(static_cast<const std::uint16_t*>(data),static_cast<const std::uint16_t*>(data)+size/2);
    if(data && target==GL_UNIFORM_BUFFER) fake::uniform_bytes.assign(static_cast<const unsigned char*>(data),static_cast<const unsigned char*>(data)+size);
    if(target==GL_UNIFORM_BUFFER && size==sizeof(fake::uploaded_transform)){
        ++fake::transform_uploads;std::memcpy(&fake::uploaded_transform,data,size);
    }
}
GLenum glCheckFramebufferStatus(GLenum target) { return GL_FRAMEBUFFER_COMPLETE; }
void glClear(GLbitfield mask) { ++fake::clears; }
void glInvalidateFramebuffer(GLenum, GLsizei count, const GLenum* attachments) {
    for (GLsizei i = 0; i < count; ++i) fake::invalidated.push_back(attachments[i]);
}
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {}
void glClearDepthf(GLfloat d) {}
GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) { return fake::wait_result; }
void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {}
void glCompileShader(GLuint shader) {}
GLuint glCreateProgram(void) { return ++fake::next; }
GLuint glCreateShader(GLenum type) { return ++fake::next; }
void glDeleteBuffers(GLsizei n, const GLuint *buffers) {}
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) {}
void glDeleteProgram(GLuint program) {}
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers) {}
void glDeleteShader(GLuint shader) {}
void glDeleteSync(GLsync sync) {}
void glDeleteTextures(GLsizei n, const GLuint *textures) {}
void glDeleteVertexArrays(GLsizei n, const GLuint *arrays) {}
void glDepthMask(GLboolean flag) { fake::depth_mask = flag; }
void glDisable(GLenum cap) { if (cap == GL_DEPTH_TEST) fake::depth_test = GL_FALSE; }
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    ++fake::draws;fake::submitted_index_count=count;
    fake::draw_first = fake::first[fake::bound[3]];
    fake::sampler_seen = fake::samplers[3];
}
void glDrawArrays(GLenum, GLint, GLsizei) { ++fake::draws; }
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices) { glDrawElements(mode, count, type, indices); }
void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint) { glDrawElements(mode, count, type, indices); }
void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances, GLint) {
    ++fake::instanced_draws; fake::instance_count = static_cast<unsigned>(instances); glDrawElements(mode, count, type, indices);
}
void glEnable(GLenum cap) { if (cap == GL_DEPTH_TEST) fake::depth_test = GL_TRUE; }
void glEnableVertexAttribArray(GLuint index) {}
GLsync glFenceSync(GLenum condition, GLbitfield flags) { return reinterpret_cast<GLsync>(++fake::next); }
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {}
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    if (attachment == GL_COLOR_ATTACHMENT0) fake::color_attachments[fake::framebuffer] = texture;
    if (attachment == GL_DEPTH_ATTACHMENT) fake::depth_attachments[fake::framebuffer] = texture;
}
void glGenBuffers(GLsizei n, GLuint *buffers) {
    for (GLsizei i = 0; i < n; ++i)
        buffers[i] = ++fake::next;
}
void glGenFramebuffers(GLsizei n, GLuint *framebuffers) {
    for (GLsizei i = 0; i < n; ++i)
        framebuffers[i] = ++fake::next;
}
void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers) {
    for (GLsizei i = 0; i < n; ++i)
        renderbuffers[i] = ++fake::next;
}
void glGenSamplers(GLsizei count, GLuint *samplers) {
    for (GLsizei i = 0; i < count; ++i)
        samplers[i] = ++fake::next;
}
void glDeleteSamplers(GLsizei, const GLuint *) {}
void glGenTextures(GLsizei n, GLuint *textures) {
    for (GLsizei i = 0; i < n; ++i)
        textures[i] = ++fake::next;
}
void glGenVertexArrays(GLsizei n, GLuint *arrays) {
    for (GLsizei i = 0; i < n; ++i)
        arrays[i] = ++fake::next;
}
void glGetActiveUniformBlockiv(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint *params) {
    *params = sizeof(starlet::gx::TransformUniforms);
}
GLenum glGetError(void) {
    auto e = fake::error;
    fake::error = GL_NO_ERROR;
    return e;
}
void glGetIntegerv(GLenum pname, GLint *data) {
    switch (pname) {
    case GL_MAJOR_VERSION:
        *data = 3;
        break;
    case GL_MINOR_VERSION:
        *data = 1;
        break;
    case GL_MAX_VERTEX_ATTRIBS:
        *data = 16;
        break;
    case GL_MAX_UNIFORM_BLOCK_SIZE:
        *data = 16384;
        break;
    case GL_NUM_PROGRAM_BINARY_FORMATS:
        *data = fake::program_binary_formats;
        break;
    case GL_MAX_VIEWPORT_DIMS:
        data[0] = 16384;
        data[1] = 16384;
        break;
    default:
        *data = 0;
    }
}
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {}
void glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    if (pname == GL_PROGRAM_BINARY_LENGTH) *params = GLint(fake::program_binary.size());
    else if (pname == GL_LINK_STATUS) *params = fake::last_link;
    else *params = GL_TRUE;
}
void glProgramParameteri(GLuint, GLenum pname, GLint value) {
    if (pname == GL_PROGRAM_BINARY_RETRIEVABLE_HINT && value == GL_TRUE) ++fake::retrievable_hints;
}
void glGetProgramBinary(GLuint, GLsizei size, GLsizei *length, GLenum *format, void *binary) {
    const auto n = std::min<std::size_t>(std::size_t(size), fake::program_binary.size());
    std::memcpy(binary, fake::program_binary.data(), n);
    *length = GLsizei(n);
    *format = fake::binary_format;
}
void glProgramBinary(GLuint, GLenum format, const void *binary, GLsizei length) {
    const auto* bytes = static_cast<const unsigned char*>(binary);
    fake::loaded_binary.assign(bytes, bytes + length);
    fake::last_link = format == fake::binary_format ? fake::binary_links : GL_FALSE;
}
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {}
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) { *params = GL_TRUE; }
const GLubyte *glGetStringi(GLenum,GLuint) { return nullptr; }
const GLubyte *glGetString(GLenum name) {
    if (name == GL_RENDERER) return reinterpret_cast<const GLubyte *>(fake::renderer_name);
    return reinterpret_cast<const GLubyte *>("OpenGL ES 3.1 private dispatch fixture");
}
GLuint glGetUniformBlockIndex(GLuint program, const GLchar *uniformBlockName) { return 0; }
GLint glGetUniformLocation(GLuint program, const GLchar *name) {
    if(!fake::track_uniforms || std::strcmp(name,"uGxCoordinateScale[15]")==0) return -1;
    auto key=std::make_pair(program,std::string(name));
    auto found=fake::uniform_locations.find(key);
    if(found!=fake::uniform_locations.end())return found->second;
    const auto location=GLint(++fake::next);fake::uniform_locations[key]=location;return location;
}
void glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { uniform(l,{a,b,c,d}); }
void glUniform1f(GLint l, GLfloat a) { uniform(l,{a,0,0,0}); }
void glLinkProgram(GLuint program) { fake::last_link = fake::links; }
void glPixelStorei(GLenum pname, GLint param) {
    if (pname == 0xffffffffu)
        fake::error = GL_INVALID_ENUM;
    else
        fake::pixel[pname] = param;
}
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels) {
    ++fake::read_pixels;
    fake::pack_seen = fake::pixel[GL_PACK_SKIP_ROWS];
    fake::pack_pbo_seen = fake::pack_buffer;
    fake::pack_required = transfer_bytes(width, height, true);
    if (fake::map_pattern && fake::read_pattern_row && !fake::pack_buffer && pixels)
        for (GLsizei row = 0; row < height; ++row)
            for (GLsizei column = 0; column < width; ++column)
                for (int k = 0; k < 4; ++k)
                    static_cast<unsigned char*>(pixels)[(std::size_t(row) * width + column) * 4 + k] = static_cast<unsigned char>(
                        ((std::size_t(y + row) * fake::read_pattern_row + std::size_t(x + column)) * 4 + k) % 251);
}
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {}
void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param) {}
void glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) {}
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {}
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    std::string source;
    for (GLsizei i = 0; i < count; ++i) source += length && length[i] >= 0 ? std::string(string[i], length[i]) : std::string(string[i]);
    fake::shader_sources.push_back(std::move(source));
}
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const void *pixels) {
    ++fake::uploads;
    fake::unpack_seen = fake::pixel[GL_UNPACK_SKIP_ROWS];
    fake::unpack_pbo_seen = fake::unpack_buffer;
    fake::unpack_required = transfer_bytes(width, height, false);
    if (!fake::unpack_seen && !fake::unpack_buffer)
        fake::first[fake::bound[fake::active]] = static_cast<const unsigned char *>(pixels)[0];
}
void glTexSubImage2D(GLenum target, GLint, GLint, GLint, GLsizei width, GLsizei height,
                     GLenum, GLenum, const void* pixels) {
    ++fake::uploads;fake::unpack_seen=fake::pixel[GL_UNPACK_SKIP_ROWS];fake::unpack_pbo_seen=fake::unpack_buffer;
    fake::unpack_required=transfer_bytes(width,height,false);
    if(!fake::unpack_seen&&!fake::unpack_buffer)fake::first[fake::bound[fake::active]]=static_cast<const unsigned char*>(pixels)[0];
}
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) {}
void glTexParameteri(GLenum target, GLenum pname, GLint param) {}
void glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {}
void glUniform1i(GLint location, GLint v0) { fake::last_int = v0; }
void glUniform2f(GLint location, GLfloat v0, GLfloat v1) { uniform(location,{v0,v1,0,0}); }
void glUniform2fv(GLint location, GLsizei count, const GLfloat* v) { for (GLsizei i = 0; i < count; ++i) uniform(location + i,{v[i*2],v[i*2+1],0,0}); }
void glUniform1fv(GLint location, GLsizei count, const GLfloat* v) { for (GLsizei i = 0; i < count; ++i) uniform(location + i,{v[i],0,0,0}); }
void glUniform3fv(GLint location, GLsizei count, const GLfloat* v) { for (GLsizei i = 0; i < count; ++i) uniform(location + i,{v[i*3],v[i*3+1],v[i*3+2],0}); }
void glUniform4fv(GLint location, GLsizei count, const GLfloat* v) { for (GLsizei i = 0; i < count; ++i) uniform(location + i,{v[i*4],v[i*4+1],v[i*4+2],v[i*4+3]}); }
void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2) { uniform(location,{double(v0),double(v1),double(v2),0}); }
void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) { uniform(location,{v0,v1,v2,0}); }
void glUniform2i(GLint location, GLint v0, GLint v1) { uniform(location,{double(v0),double(v1),0,0}); }
void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) { fake::last_ivec4={v0,v1,v2,v3}; uniform(location,{double(v0),double(v1),double(v2),double(v3)}); }
void glDispatchCompute(GLuint x, GLuint y, GLuint) {
    ++fake::dispatches;
    // Bytes up to the last texel a store writes, as glReadPixels reports them.
    const auto& r = fake::last_ivec4;
    if (x * 64 >= GLuint(r[2]))
        fake::pack_required = (std::size_t(fake::last_int) + (y - 1) * std::size_t(r[3]) + r[2]) * 4;
}
void glMemoryBarrier(GLbitfield) {}
void glUniform4ui(GLint l,GLuint a,GLuint b,GLuint c,GLuint d) { uniform(l,{double(a),double(b),double(c),double(d)}); }
void glUniform1ui(GLint l,GLuint a) { uniform(l,{double(a),0,0,0}); }
void glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding) {}
void glUseProgram(GLuint program) { fake::current_program=program; }
void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer) {}
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                           const void *pointer) {}
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {}
}
