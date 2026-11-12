// The AX microcode's frame: the DSP accelerator reading samples from ARAM, the sample-rate converter,
// the per-voice envelope, filter and mixer, the aux exchange with the CPU, the compressor and the
// output. Follows Dolphin's AX HLE (Source/Core/Core/HW/DSPHLE/UCodes/AX.cpp, AXVoice.h) and its
// accelerator (Source/Core/Core/DSP/DSPAccelerator.cpp); ax.md lists the differences.
#include "ax_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace starlet::ax::detail {
namespace {

s16 clamp16(s64 value) { return s16(std::clamp<s64>(value, -0x8000, 0x7FFF)); }

void warn_once(std::atomic<bool>& said, const char* message) {
    if (!said.exchange(true, std::memory_order_relaxed)) std::fprintf(stderr, "starlet: ax: %s\n", message);
}

std::atomic<bool> warned_format{false};
std::atomic<bool> warned_itd{false};

u32 hilo(u16 hi, u16 lo) { return (u32(hi) << 16) | lo; }

// The accelerator, set up from a parameter block for one millisecond as the microcode does.
class Accelerator {
public:
    Accelerator(AXPB& pb, const Memory& memory)
        : pb_(pb), memory_(memory),
          start_(hilo(pb.addr.loopAddressHi, pb.addr.loopAddressLo) & 0x3FFFFFFF),
          end_(hilo(pb.addr.endAddressHi, pb.addr.endAddressLo) & 0x3FFFFFFF),
          current_(hilo(pb.addr.currentAddressHi, pb.addr.currentAddressLo) & 0xBFFFFFFF),
          format_(pb.addr.format), yn1_(s16(pb.adpcm.yn1)), yn2_(s16(pb.adpcm.yn2)), gain_(s16(pb.adpcm.gain)),
          pred_scale_(pb.adpcm.pred_scale & 0x7F) {}

    s16 read_sample() {
        if (stopped_) return 0;
        const unsigned decode = (format_ >> 2) & 3;
        // Decodes 1 and 3 read the accelerator's input register, which AX never writes.
        s16 raw = decode == 1 || decode == 3 ? 0 : s16(current_sample());
        const int coef = (pred_scale_ >> 4) & 7;
        const s32 c1 = s16(pb_.adpcm.a[coef][0]);
        const s32 c2 = s16(pb_.adpcm.a[coef][1]);
        s16 value = 0;
        u32 step = 2;
        if (decode == 0) {
            raw &= 0xF;
            if (raw >= 8) raw -= 16;
            const s32 scale = 1 << (pred_scale_ & 0xF);
            value = clamp16(scale * raw + ((0x400 + c1 * yn1_ + c2 * yn2_) >> 11));
            yn2_ = yn1_;
            yn1_ = value;
            current_ += 1;
            // An end address at a frame's first or second nibble wraps without reading a header.
            if ((end_ & 0xF) == 0 && current_ == end_) {
                current_ = start_ + 1;
            } else if ((end_ & 0xF) == 1 && current_ == end_ - 1) {
                current_ = start_;
            } else if ((current_ & 15) == 0) {
                pred_scale_ = read_byte((current_ & ~15u) >> 1);
                current_ += 2;
                step += 2;
            }
        } else {
            int shift = 0;
            switch ((format_ >> 4) & 3) {
            case 0: shift = 11; break;
            case 1: shift = 0; break;
            case 2: shift = 16; break;
            default: warn_once(warned_format, "PCM sample format with an invalid gain scale"); break;
            }
            value = s16(((s32(gain_) * raw) >> shift) + ((c1 * yn1_) >> shift) + ((c2 * yn2_) >> shift));
            yn2_ = yn1_;
            yn1_ = value;
            if (decode != 1) current_ += 1;
        }
        if (current_ == end_ + step - 1) {
            current_ = start_;
            stopped_ = true;
            end_reached();
        }
        current_ &= 0xBFFFFFFF;
        return value;
    }

    void store() {
        pb_.addr.currentAddressHi = u16(current_ >> 16);
        pb_.addr.currentAddressLo = u16(current_);
        pb_.adpcm.yn1 = u16(yn1_);
        pb_.adpcm.yn2 = u16(yn2_);
        pb_.adpcm.pred_scale = pred_scale_;
    }

private:
    std::uint8_t read_byte(u32 address) const {
        return address < memory_.aram_bytes ? memory_.aram[address] : 0;
    }

    u16 current_sample() const {
        switch (format_ & 3) {
        case 0: {
            const u16 byte = read_byte(current_ >> 1);
            return current_ & 1 ? byte & 0xF : byte >> 4;
        }
        case 1: return read_byte(current_);
        case 2: return u16((read_byte(current_ * 2) << 8) | read_byte(current_ * 2 + 1));
        default: warn_once(warned_format, "sample format with an invalid sample size"); return 0;
        }
    }

    // The end-of-sample exception: loop with the loop context (a stream keeps its history), or stop.
    void end_reached() {
        if (pb_.addr.loopFlag) {
            pred_scale_ = pb_.adpcmLoop.loop_pred_scale & 0x7F;
            if (pb_.type != AX_PB_TYPE_STREAM) {
                yn1_ = s16(pb_.adpcmLoop.loop_yn1);
                yn2_ = s16(pb_.adpcmLoop.loop_yn2);
            }
            stopped_ = false;
        } else {
            pb_.state = AX_PB_STATE_STOP;
        }
    }

    AXPB& pb_;
    const Memory& memory_;
    u32 start_, end_, current_;
    u16 format_;
    s16 yn1_, yn2_, gain_;
    u16 pred_scale_;
    bool stopped_ = false;
};

// Converts to count output samples; position and ratio are 16.16 fixed point. Linear and 4-tap
// interpolate inside a four-sample history, the oldest samples first, as the microcode does.
template <typename Input>
u32 resample(Input&& input, s16* out, u32 count, u16* last, u32 position, u32 ratio, u16 type, const s16* coefs) {
    s16 history[4];
    u32 index = 0;
    for (int i = 0; i < 4; ++i) history[index++ & 3] = s16(last[i]);
    if (type == 0 || type == 1) {
        for (u32 i = 0; i < count; ++i) {
            position += ratio;
            while (position >= 0x10000) {
                history[index++ & 3] = input();
                position -= 0x10000;
            }
            if (type == 0) {
                const s16* c = &coefs[((position & 0xFFFF) >> 9) << 2];
                const s64 t0 = history[index++ & 3], t1 = history[index++ & 3];
                const s64 t2 = history[index++ & 3], t3 = history[index++ & 3];
                out[i] = clamp16((t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15);
            } else {
                const u16 frac = u16(position);
                if (frac) {
                    const s32 s0 = history[index++ & 3];
                    const s32 s1 = history[index++ & 3];
                    out[i] = s16((s0 * s32(u16(-frac)) + s1 * s32(frac)) >> 16);
                    index += 2;
                } else {
                    out[i] = history[index++ & 3];
                    index += 3;
                }
            }
        }
        for (int i = 3; i >= 0; --i) last[i] = u16(history[--index & 3]);
    } else {
        for (u32 i = 0; i < count; ++i) out[i] = input();
        for (int i = 0; i < 4; ++i) last[i] = u16(out[count - 4 + i]);
    }
    return position;
}

void apply_updates(AXPB& pb, const u16* updates, int ms) {
    u32 start = 0;
    for (int i = 0; i < ms; ++i) start += pb.update.updNum[i];
    const u32 pairs = update_words / 2;
    const u32 count = pb.update.updNum[ms];
    if (start >= pairs || count > pairs - start) return;
    auto* words = reinterpret_cast<u16*>(&pb);
    for (u32 i = start; i < start + count; ++i) {
        const u16 offset = updates[2 * i];
        if (offset < sizeof(AXPB) / sizeof(u16)) words[offset] = updates[2 * i + 1];
    }
}

struct Route {
    Bus bus;
    u16 AXPBMIX::*volume;
    u16 AXPBMIX::*delta;
    s16 AXPBDPOP::*dpop;
    u16 on, ramp;
};

// mixerCtrl bits as AXSetVoiceMix sets them.
constexpr Route routes[] = {
    {main_l, &AXPBMIX::vL, &AXPBMIX::vDeltaL, &AXPBDPOP::aL, 0x0001, 0x0008},
    {main_r, &AXPBMIX::vR, &AXPBMIX::vDeltaR, &AXPBDPOP::aR, 0x0002, 0x0008},
    {main_s, &AXPBMIX::vS, &AXPBMIX::vDeltaS, &AXPBDPOP::aS, 0x0004, 0x0008},
    {auxa_l, &AXPBMIX::vAuxAL, &AXPBMIX::vDeltaAuxAL, &AXPBDPOP::aAuxAL, 0x0010, 0x0040},
    {auxa_r, &AXPBMIX::vAuxAR, &AXPBMIX::vDeltaAuxAR, &AXPBDPOP::aAuxAR, 0x0020, 0x0040},
    {auxa_s, &AXPBMIX::vAuxAS, &AXPBMIX::vDeltaAuxAS, &AXPBDPOP::aAuxAS, 0x0080, 0x0100},
    {auxb_l, &AXPBMIX::vAuxBL, &AXPBMIX::vDeltaAuxBL, &AXPBDPOP::aAuxBL, 0x0200, 0x0800},
    {auxb_r, &AXPBMIX::vAuxBR, &AXPBMIX::vDeltaAuxBR, &AXPBDPOP::aAuxBR, 0x0400, 0x0800},
    {auxb_s, &AXPBMIX::vAuxBS, &AXPBMIX::vDeltaAuxBS, &AXPBDPOP::aAuxBS, 0x1000, 0x2000},
};

void process_voice(Dsp& dsp, AXPB& pb, const Memory& memory, int offset) {
    s16 samples[ms_samples];
    read_samples(pb, memory, samples, ms_samples);

    for (s16& sample : samples) {
        sample = clamp16((s32(sample) * s32(s16(pb.ve.currentVolume))) >> 15);
        pb.ve.currentVolume = u16(pb.ve.currentVolume + pb.ve.currentDelta);
    }

    if (pb.lpf.on) {
        const s32 a0 = pb.lpf.a0;
        const s32 b0 = s16(pb.lpf.b0);
        for (s16& sample : samples) {
            sample = clamp16((a0 * sample + b0 * s16(pb.lpf.yn1)) >> 15);
            pb.lpf.yn1 = u16(sample);
        }
    }

    for (const Route& route : routes) {
        if (!(pb.mixerCtrl & route.on)) continue;
        u16& volume = pb.mix.*route.volume;
        const u16 delta = pb.mixerCtrl & route.ramp ? pb.mix.*route.delta : 0;
        int* out = dsp.bus[route.bus] + offset;
        s16 last = 0;
        for (int i = 0; i < ms_samples; ++i) {
            last = clamp16((s64(samples[i]) * volume) >> 15);
            out[i] += last;
            volume = u16(volume + delta);
        }
        pb.dpop.*route.dpop = last;
    }

    if (pb.itd.flag && (pb.itd.shiftL || pb.itd.shiftR || pb.itd.targetShiftL || pb.itd.targetShiftR))
        warn_once(warned_itd, "a voice uses the initial time delay (ITD), which is not modelled; it plays without the delay");
}

void exchange_aux(Dsp& dsp, s32* send, const s32* receive, Bus first) {
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < frame_samples; ++i) send[c * frame_samples + i] = dsp.bus[first + c][i];
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < frame_samples; ++i) dsp.bus[main_l + c][i] += receive[c * frame_samples + i];
}

// Dolby Pro Logic II: aux A carries L, R and the rear sends (aux A S for Ls, aux B S for Rs); aux B
// L/R carry the rear channels, which loop back through main memory one frame later.
void exchange_dpl2(Dsp& dsp, const FrameCommand& command, AuxBuffers& aux) {
    const int n = frame_samples;
    if (command.aux_a) {
        s32* send_a = aux.a[command.aux_write];
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < n; ++i) send_a[c * n + i] = dsp.bus[auxa_l + c][i];
        for (int i = 0; i < n; ++i) aux.b[command.aux_write][2 * n + i] = dsp.bus[auxb_s][i];
        const s32* back_a = aux.a[command.aux_read];
        const s32* back_b = aux.b[command.aux_read];
        for (int i = 0; i < n; ++i) {
            dsp.bus[main_l][i] += back_a[i];
            dsp.bus[main_r][i] += back_a[n + i];
            dsp.bus[auxb_l][i] += back_a[2 * n + i];
            dsp.bus[auxb_r][i] += back_b[2 * n + i];
        }
    }
    s32* up = aux.b[command.dpl2_write];
    const s32* down = aux.b[command.dpl2_read];
    for (int i = 0; i < n; ++i) {
        up[i] = dsp.bus[auxb_l][i];
        up[n + i] = dsp.bus[auxb_r][i];
    }
    for (int i = 0; i < n; ++i) {
        dsp.bus[auxb_l][i] = down[i];
        dsp.bus[main_l][i] += down[i];
        dsp.bus[auxb_r][i] = down[n + i];
        dsp.bus[main_r][i] += down[n + i];
    }
}

// The SDK's command: threshold 0x8000, 10 release frames, 11 attack frames in the table.
void compress(Dsp& dsp, const u16* table) {
    constexpr s32 threshold = 0x8000;
    constexpr u16 release_frames = 10;
    constexpr u32 attack_frames = 11;
    bool triggered = false;
    for (int i = 0; i < frame_samples && !triggered; ++i)
        triggered = std::abs(dsp.bus[main_l][i]) > threshold || std::abs(dsp.bus[main_r][i]) > threshold;
    u32 frame;
    if (triggered) {
        frame = dsp.compressor_pos;
        dsp.compressor_pos = release_frames;
    } else if (dsp.compressor_pos) {
        --dsp.compressor_pos;
        frame = attack_frames + dsp.compressor_pos;
    } else {
        return;
    }
    const u16* gain = table + frame * frame_samples;
    for (int i = 0; i < frame_samples; ++i) {
        dsp.bus[main_l][i] = int((s64(dsp.bus[main_l][i]) * gain[i]) >> 15);
        dsp.bus[main_r][i] = int((s64(dsp.bus[main_r][i]) * gain[i]) >> 15);
    }
}

double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// Windowed-sinc polyphase tables after Dolphin's free DSP ROM (docs/DSP/free_dsp_rom/generate_coefs.py):
// cutoffs 0.5, 0.75 and 1.0 of 16 kHz, 512 taps split into 128 phases of 4.
struct CoefficientTables {
    s16 data[0x800] = {};

    CoefficientTables() {
        constexpr int taps = 512;
        constexpr double pi = 3.14159265358979323846;
        const double beta = pi * 9 / 4;
        double hamming[taps], kaiser[taps], x[taps];
        for (int n = 0; n < taps; ++n) {
            x[n] = -2 + 4.0 * n / (taps - 1);
            hamming[n] = 0.54 - 0.46 * std::cos(2 * pi * n / (taps - 1));
            const double r = (n - (taps - 1) / 2.0) / ((taps - 1) / 2.0);
            kaiser[n] = bessel_i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel_i0(beta);
        }
        const auto sinc = [&](double t) { return t == 0 ? 1.0 : std::sin(pi * t) / (pi * t); };
        double table[taps];
        const double cutoffs[3] = {0.5, 0.75, 1.0};
        for (int t = 0; t < 3; ++t) {
            for (int n = 0; n < taps; ++n) table[n] = sinc(x[n] * cutoffs[t]) * (t == 1 ? kaiser[n] : hamming[n]);
            double peak = -1e300;
            for (int k = 0; k < 128; ++k)
                peak = std::max(peak, table[127 - k] + table[255 - k] + table[383 - k] + table[511 - k]);
            for (int k = 0; k < 128; ++k)
                for (int j = 0; j < 4; ++j)
                    data[t * 0x200 + k * 4 + j] = s16(u16(long(std::nearbyint(table[j * 128 + 127 - k] / peak * 32767)) & 0xFFFF));
        }
    }
};

}

const s16* src_coefficients() {
    static const CoefficientTables tables;
    return tables.data;
}

void read_samples(AXPB& pb, const Memory& memory, s16* out, u32 count) {
    Accelerator accelerator(pb, memory);
    const s16* coefs = src_coefficients() + (pb.coefSelect & 3) * 0x200;
    const u32 position = resample([&] { return accelerator.read_sample(); }, out, count, pb.src.last_samples,
                                  pb.src.currentAddressFrac, hilo(pb.src.ratioHi, pb.src.ratioLo), pb.srcSelect, coefs);
    pb.src.currentAddressFrac = u16(position);
    accelerator.store();
}

void reset(Dsp& dsp) {
    std::memset(&dsp, 0, sizeof dsp);
}

void run_frame(Dsp& dsp, const FrameCommand& command, AuxBuffers& aux, const Memory& memory,
               const u16* compressor_table, s16* out) {
    for (int b = 0; b < bus_count; ++b) {
        const s32 value = command.studio_value[b];
        const s16 delta = command.studio_delta[b];
        for (int i = 0; i < frame_samples; ++i) dsp.bus[b][i] = value ? value + i * delta : 0;
    }
    // Stereo folds last frame's S into L and R; surround encodes it with opposite phase.
    if (command.mode == AX_MODE_STEREO || command.mode == AX_MODE_SURROUND) {
        const int left = command.mode == AX_MODE_STEREO ? 1 : -1;
        for (int i = 0; i < frame_samples; ++i) {
            dsp.bus[main_l][i] += left * dsp.surround[i];
            dsp.bus[main_r][i] += dsp.surround[i];
        }
    }

    for (u32 v = 0; v < AX_MAX_VOICES; ++v) {
        AXPB& pb = dsp.pb[v];
        for (int ms = 0; ms < frame_ms; ++ms) {
            apply_updates(pb, dsp.updates[v], ms);
            if (pb.state == AX_PB_STATE_RUN) process_voice(dsp, pb, memory, ms * ms_samples);
        }
    }

    if (command.mode == AX_MODE_DPL2) {
        exchange_dpl2(dsp, command, aux);
    } else {
        if (command.aux_a) exchange_aux(dsp, aux.a[command.aux_write], aux.a[command.aux_read], auxa_l);
        if (command.aux_b) exchange_aux(dsp, aux.b[command.aux_write], aux.b[command.aux_read], auxb_l);
    }

    if (command.compressor && compressor_table) compress(dsp, compressor_table);

    for (int i = 0; i < frame_samples; ++i) {
        dsp.surround[i] = dsp.bus[main_s][i];
        out[2 * i] = clamp16(dsp.bus[main_r][i]);
        out[2 * i + 1] = clamp16(dsp.bus[main_l][i]);
    }
}

}
