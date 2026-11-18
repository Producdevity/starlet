#pragma once

// Hooks of the GX trace recorder, compiled only with STARLET_GX_TRACE (CMake option
// STARLET_GX_TRACE). Without it the macros below expand to nothing.

#if defined(STARLET_GX_TRACE)

#include "format.hpp"

#include <functional>

namespace starlet::gx {
class GlesRenderer;
}

namespace starlet::gx::trace {

struct DecoderTap;
DecoderTap* attach(Decoder*, MemoryReader&, CommandSink&, const Limits&, EfbLayout);
void detach(DecoderTap*);
MemoryReader& memory(DecoderTap&);
CommandSink& sink(DecoderTap&);

// One public decoder call: recorded when it is the outermost call on a recording decoder.
class DecoderCall {
public:
    DecoderCall(DecoderTap* tap, const Decoder& decoder, Op op, ByteView bytes = {}, std::uint32_t argument = 0);
    ~DecoderCall();
    void result(bool value) { result_ = value; }
    DecoderCall(const DecoderCall&) = delete;
    DecoderCall& operator=(const DecoderCall&) = delete;

private:
    DecoderTap* tap_;
    const Decoder& decoder_;
    bool recorded_ = false, result_ = false;
};

// Writes a renderer call's arguments; pointers become addresses in the canonical space.
class Encoder {
public:
    explicit Encoder(Writer& w) : w(w) {}
    Writer& w;
    void pointer(const void* data, std::size_t size);
    // The bytes at data also go into the trace as memory the call reads.
    void memory(const void* data, std::size_t size);
    // A copy destination whose canonical address the caller names.
    void target(const void* data, std::size_t size, std::uint32_t address);
};

void renderer_created(const GlesRenderer*, const GlesLimits&);
void renderer_destroyed(const GlesRenderer*);

// One public renderer call: recorded when it is the outermost one on the traced renderer.
class RendererCall {
public:
    RendererCall(const GlesRenderer* renderer, Op op, const std::function<void(Encoder&)>& arguments = {});
    ~RendererCall();
    void result(std::int32_t value);
    std::int32_t returned(std::int32_t value) { result(value); return value; }
    RendererCall(const RendererCall&) = delete;
    RendererCall& operator=(const RendererCall&) = delete;

private:
    bool entered_ = false, outermost_ = false, recorded_ = false, boundary_ = false;
};

} // namespace starlet::gx::trace

#define STARLET_GX_TRACE_RENDERER(op, ...) ::starlet::gx::trace::RendererCall starlet_trace_call(this, ::starlet::gx::trace::Op::op, ##__VA_ARGS__)
#define STARLET_GX_TRACE_RETURN(value) starlet_trace_call.returned(value)

#else

#define STARLET_GX_TRACE_RENDERER(op, ...) ((void)0)
#define STARLET_GX_TRACE_RETURN(value) (value)

#endif
