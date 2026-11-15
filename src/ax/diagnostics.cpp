// AX's audio diagnostics: what the voice parameter checks, the per-second counters and the rolling clip
// buffer report. Always on; the work per 5 ms frame is a few dozen compares per running voice, three
// passes over the aux buffers and a copy of 160 frames into the ring.
#include "diagnostics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace starlet::ax {
namespace {
const std::chrono::steady_clock::time_point process_start = std::chrono::steady_clock::now();
}

double diagnostics_seconds() noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - process_start).count();
}

}

namespace starlet::ax::diag {
namespace {

constexpr std::size_t clip_seconds = 10;
constexpr std::size_t clip_frames = clip_seconds * clip_rate;
constexpr std::size_t record_count = 512;
constexpr unsigned frames_per_second = 200;
constexpr unsigned max_clips = 10;
constexpr unsigned max_severe_lines = 300;
constexpr unsigned max_info_lines = 100;

u32 hilo(u16 hi, u16 lo) { return (u32(hi) << 16) | lo; }

u64 byte_address(u32 address, u16 format) {
    address &= 0x3FFFFFFF;
    switch (format & 3) {
    case 0: return address >> 1;
    case 2: return u64(address) * 2;
    default: return address;
    }
}

struct Record {
    std::uint64_t clock = 0;  // the frame (at 32 kHz) the voice started on
    double seconds = 0;
    int index = 0;
    std::uint32_t flags = 0;
    AXPB pb{};
};

struct State {
    std::mutex mutex;
    ClipRing ring{clip_frames};
    std::vector<Record> records = std::vector<Record>(record_count);
    std::size_t record_next = 0;
    std::uint64_t record_total = 0;
    std::uint64_t frame = 0;  // 5 ms frames
    std::uint32_t last_flags[AX_MAX_VOICES] = {};
    RateLimiter severe{8, frames_per_second};
    RateLimiter info{3, frames_per_second};
    unsigned severe_lines = 0, info_lines = 0;
    // The counters of the second in progress, and the totals.
    std::uint64_t sat_main = 0, sat_a = 0, sat_b = 0, past_end = 0;
    std::uint64_t total_sat_main = 0, total_sat_a = 0, total_sat_b = 0, total_past_end = 0, starts = 0;
    unsigned clips = 0;
    std::string directory;
    std::vector<s16> main_l, main_r, aux_a, aux_b;
};

std::atomic<State*> global{nullptr};

void log_line(const char* kind, double seconds, const std::string& text) {
    std::fprintf(stderr, "[audio] t=%.3f %s%s\n", seconds, kind, text.c_str());
}

std::size_t count_saturated(const s32* buffer, std::size_t count) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < count; ++i) n += (buffer[i] > 32767) | (buffer[i] < -32768);
    return n;
}

s16 mean_clamped(s32 l, s32 r) { return s16(std::clamp<s64>((s64(l) + r) / 2, -32768, 32767)); }

std::string clip_path(const char* reason) {
    State* s = global.load();
    std::string base = s->directory;
    if (!base.empty() && base.back() != '/') base += '/';
    char name[64];
    if (reason && !std::strcmp(reason, "exit"))
        std::snprintf(name, sizeof name, "audio-clip-exit");
    else
        std::snprintf(name, sizeof name, "audio-clip-%08.3f", diagnostics_seconds());
    return base + name;
}

}

std::uint32_t check_voice(const AXPB& pb, std::size_t memory_bytes) {
    std::uint32_t flags = 0;
    const u32 ratio = hilo(pb.src.ratioHi, pb.src.ratioLo);
    if (ratio == 0) flags |= ratio_zero;
    if (ratio > 0x20000) flags |= ratio_high;
    if (pb.srcSelect == 0) flags |= src_polyphase;
    if (pb.srcSelect > 2) flags |= src_invalid;
    const u16 format = pb.addr.format;
    const u32 loop = hilo(pb.addr.loopAddressHi, pb.addr.loopAddressLo);
    const u32 end = hilo(pb.addr.endAddressHi, pb.addr.endAddressLo);
    const u32 current = hilo(pb.addr.currentAddressHi, pb.addr.currentAddressLo);
    if (byte_address(loop, format) >= memory_bytes || byte_address(end, format) >= memory_bytes ||
        byte_address(current, format) >= memory_bytes)
        flags |= address_range;
    if (((format >> 2) & 3) == 0) {  // ADPCM decoding
        if (pb.adpcm.pred_scale > 0x7F || (pb.addr.loopFlag && pb.adpcmLoop.loop_pred_scale > 0x7F)) flags |= adpcm_predictor;
        if ((pb.adpcm.pred_scale & 0xF) > 12) flags |= adpcm_scale;
        if (pb.addr.loopFlag && pb.adpcmLoop.loop_pred_scale == 0 && pb.adpcmLoop.loop_yn1 == 0 && pb.adpcmLoop.loop_yn2 == 0)
            flags |= loop_context;
    }
    if (pb.lpf.on) flags |= lpf_on;
    if (pb.itd.flag) flags |= itd_on;
    return flags;
}

std::string flag_names(std::uint32_t flags) {
    static const struct {
        Anomaly flag;
        const char* name;
    } names[] = {{ratio_zero, "ratio-zero"},        {ratio_high, "ratio-above-2"}, {src_polyphase, "polyphase-src"},
                 {src_invalid, "src-invalid"},      {address_range, "address-outside-memory"},
                 {adpcm_predictor, "adpcm-predictor"}, {adpcm_scale, "adpcm-scale"}, {loop_context, "loop-context-zero"},
                 {lpf_on, "lpf-on"},                {itd_on, "itd-on"}};
    std::string text;
    for (const auto& n : names)
        if (flags & n.flag) text += (text.empty() ? "" : " ") + std::string(n.name);
    return text.empty() ? "-" : text;
}

double voice_ratio(const AXPB& pb) { return hilo(pb.src.ratioHi, pb.src.ratioLo) / 65536.0; }

std::string describe_voice(int index, const AXPB& pb) {
    char text[320];
    std::snprintf(text, sizeof text,
                  "voice %d fmt 0x%02x ratio %.4f src %u/%u loop %u addr loop 0x%x end 0x%x cur 0x%x "
                  "pred_scale 0x%02x loop_ps 0x%02x lpf %u itd %u type %u",
                  index, pb.addr.format, voice_ratio(pb), pb.srcSelect, pb.coefSelect, pb.addr.loopFlag,
                  hilo(pb.addr.loopAddressHi, pb.addr.loopAddressLo), hilo(pb.addr.endAddressHi, pb.addr.endAddressLo),
                  hilo(pb.addr.currentAddressHi, pb.addr.currentAddressLo), pb.adpcm.pred_scale,
                  pb.adpcmLoop.loop_pred_scale, pb.lpf.on, pb.itd.flag, pb.type);
    return text;
}

bool RateLimiter::allow(std::uint64_t frame, unsigned* suppressed_before) {
    if (suppressed_before) *suppressed_before = 0;
    if (frame >= window_start_ + window_) {
        if (suppressed_before) *suppressed_before = suppressed_;
        window_start_ = frame - (frame - window_start_) % window_;
        used_ = 0;
        suppressed_ = 0;
    }
    if (used_ >= budget_) {
        ++suppressed_;
        return false;
    }
    ++used_;
    return true;
}

ClipRing::ClipRing(std::size_t capacity_frames) : capacity_(capacity_frames), data_(capacity_frames * clip_channels) {}

void ClipRing::push(const s16* main_l, const s16* main_r, const s16* aux_a, const s16* aux_b, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        s16* frame = &data_[head_ * clip_channels];
        frame[0] = main_l[i];
        frame[1] = main_r[i];
        frame[2] = aux_a ? aux_a[i] : 0;
        frame[3] = aux_b ? aux_b[i] : 0;
        if (++head_ == capacity_) head_ = 0;
    }
    filled_ = std::min(capacity_, filled_ + count);
    total_ += count;
}

std::vector<s16> ClipRing::snapshot() const {
    std::vector<s16> out(filled_ * clip_channels);
    const std::size_t start = (head_ + capacity_ - filled_) % capacity_;
    const std::size_t first = std::min(filled_, capacity_ - start);
    std::memcpy(out.data(), &data_[start * clip_channels], first * clip_channels * sizeof(s16));
    std::memcpy(out.data() + first * clip_channels, data_.data(), (filled_ - first) * clip_channels * sizeof(s16));
    return out;
}

bool write_wav(const std::string& path, const s16* interleaved, std::size_t frames, unsigned channels, unsigned rate) {
    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const std::uint64_t data_bytes = std::uint64_t(frames) * channels * 2;
    bool ok = data_bytes < 0xFFFFFFF0u - 36;
    const auto put32 = [&](std::uint32_t v) {
        const unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
        ok = ok && std::fwrite(b, 1, 4, file) == 4;
    };
    const auto put16 = [&](std::uint16_t v) {
        const unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
        ok = ok && std::fwrite(b, 1, 2, file) == 2;
    };
    ok = ok && std::fwrite("RIFF", 1, 4, file) == 4;
    put32(std::uint32_t(36 + data_bytes));
    ok = ok && std::fwrite("WAVEfmt ", 1, 8, file) == 8;
    put32(16);
    put16(1);
    put16(std::uint16_t(channels));
    put32(rate);
    put32(rate * channels * 2);
    put16(std::uint16_t(channels * 2));
    put16(16);
    ok = ok && std::fwrite("data", 1, 4, file) == 4;
    put32(std::uint32_t(data_bytes));
    // Little-endian samples whatever the host is.
    for (std::size_t i = 0; ok && i < frames * channels;) {
        unsigned char block[4096];
        std::size_t n = 0;
        for (; n < sizeof block && i < frames * channels; ++i, n += 2) {
            const std::uint16_t v = std::uint16_t(interleaved[i]);
            block[n] = (unsigned char)v;
            block[n + 1] = (unsigned char)(v >> 8);
        }
        ok = std::fwrite(block, 1, n, file) == n;
    }
    ok = std::fclose(file) == 0 && ok;
    if (ok) ok = std::rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) std::remove(temporary.c_str());
    return ok;
}

void reset() {
    State* s = global.load();
    if (!s) {
        s = new State;
        global.store(s);
        return;
    }
    // Re-initialisation keeps the clip window and the directory: the game's AXQuit/AXInit between
    // scenes does not clear what happened just before.
    std::lock_guard<std::mutex> lock(s->mutex);
    std::memset(s->last_flags, 0, sizeof s->last_flags);
}

void stopped_voice(int index) {
    if (State* s = global.load()) s->last_flags[index] = 0;
}

void running_voice(int index, const AXPB& pb, bool started, std::size_t memory_bytes) {
    State* s = global.load();
    if (!s) return;
    const std::uint32_t flags = check_voice(pb, memory_bytes);
    std::uint32_t& last = s->last_flags[index];
    if (started) {
        std::lock_guard<std::mutex> lock(s->mutex);
        Record& r = s->records[s->record_next];
        r.clock = s->frame * 160;
        r.seconds = diagnostics_seconds();
        r.index = index;
        r.flags = flags;
        r.pb = pb;
        s->record_next = (s->record_next + 1) % record_count;
        ++s->record_total;
        ++s->starts;
        last = 0;
    }
    if (flags == last) return;
    last = flags;
    if (!flags) return;
    const bool severe = flags & severe_mask;
    RateLimiter& limiter = severe ? s->severe : s->info;
    unsigned& lines = severe ? s->severe_lines : s->info_lines;
    unsigned dropped = 0;
    const bool allowed = lines < (severe ? max_severe_lines : max_info_lines) && limiter.allow(s->frame, &dropped);
    if (dropped) log_line("", diagnostics_seconds(), "(" + std::to_string(dropped) + (severe ? " voice anomaly" : " voice note") + " lines suppressed)");
    if (!allowed) return;
    ++lines;
    log_line(started ? "start " : "change ", diagnostics_seconds(), describe_voice(index, pb) + " [" + flag_names(flags) + "]");
    if (lines == (severe ? max_severe_lines : max_info_lines))
        log_line("", diagnostics_seconds(), "(voice lines stop here for this run)");
}

void end_frame(const s16* main_rl, const s32* aux_a, const s32* aux_b, unsigned main_saturated, unsigned reads_past_end) {
    State* s = global.load();
    if (!s) return;
    constexpr int n = 160;
    s16 ret_a[n] = {}, ret_b[n] = {};
    s16 left[n], right[n];
    for (int i = 0; i < n; ++i) {
        right[i] = main_rl[2 * i];
        left[i] = main_rl[2 * i + 1];
    }
    if (aux_a) {
        s->sat_a += count_saturated(aux_a, 3 * n);
        for (int i = 0; i < n; ++i) ret_a[i] = mean_clamped(aux_a[i], aux_a[n + i]);
    }
    if (aux_b) {
        s->sat_b += count_saturated(aux_b, 3 * n);
        for (int i = 0; i < n; ++i) ret_b[i] = mean_clamped(aux_b[i], aux_b[n + i]);
    }
    s->sat_main += main_saturated;
    s->past_end += reads_past_end;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->ring.push(left, right, aux_a ? ret_a : nullptr, aux_b ? ret_b : nullptr, n);
    }
    if (++s->frame % frames_per_second == 0) {
        if (s->sat_main || s->sat_a || s->sat_b || s->past_end) {
            char text[160];
            std::snprintf(text, sizeof text, "1 s: saturated samples main %" PRIu64 " aux-A %" PRIu64 " aux-B %" PRIu64 ", reads past end %" PRIu64,
                          s->sat_main, s->sat_a, s->sat_b, s->past_end);
            log_line("", diagnostics_seconds(), text);
        }
        s->total_sat_main += s->sat_main;
        s->total_sat_a += s->sat_a;
        s->total_sat_b += s->sat_b;
        s->total_past_end += s->past_end;
        s->sat_main = s->sat_a = s->sat_b = s->past_end = 0;
    }
}

}

namespace starlet::ax {

void set_diagnostics_directory(const char* directory) {
    diag::State* s = diag::global.load();
    if (!s) {
        s = new diag::State;
        diag::global.store(s);
    }
    std::lock_guard<std::mutex> lock(s->mutex);
    s->directory = directory ? directory : "";
}

bool save_audio_clip(const char* reason) {
    using namespace diag;
    State* s = global.load();
    if (!s) return false;
    const bool exiting = reason && !std::strcmp(reason, "exit");
    std::vector<s16> audio;
    std::vector<Record> window;
    std::uint64_t first_frame, total;
    std::string path;
    double now;
    std::uint64_t totals[5];
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->directory.empty() || s->ring.size() == 0) return false;
        if (!exiting && s->clips >= max_clips) return false;
        if (!exiting) ++s->clips;
        audio = s->ring.snapshot();
        total = s->ring.total();
        first_frame = total - s->ring.size();
        const std::size_t kept = std::size_t(std::min<std::uint64_t>(s->record_total, record_count));
        for (std::size_t i = 0; i < kept; ++i) {
            const Record& r = s->records[(s->record_next + record_count - kept + i) % record_count];
            if (r.clock >= first_frame) window.push_back(r);
        }
        now = diagnostics_seconds();
        path = clip_path(reason);
        totals[0] = s->total_sat_main + s->sat_main;
        totals[1] = s->total_sat_a + s->sat_a;
        totals[2] = s->total_sat_b + s->sat_b;
        totals[3] = s->total_past_end + s->past_end;
        totals[4] = s->starts;
    }
    const std::size_t frames = audio.size() / clip_channels;
    const bool wav = write_wav(path + ".wav", audio.data(), frames, clip_channels, clip_rate);
    std::FILE* text = std::fopen((path + ".txt").c_str(), "w");
    if (text) {
        std::fprintf(text, "audio clip, reason %s, saved at t=%.3f s\n", reason ? reason : "-", now);
        std::fprintf(text, "wav: %zu frames at %u Hz, channels main L, main R, aux A return, aux B return (mean of L and R)\n",
                     frames, clip_rate);
        const double start = double(first_frame) / clip_rate;
        std::fprintf(text, "window: t=%.3f to %.3f s (the wav's first sample is at t=%.3f)\n", now - double(frames) / clip_rate, now, start);
        std::fprintf(text, "totals this run: saturated main %" PRIu64 " aux-A %" PRIu64 " aux-B %" PRIu64 ", reads past end %" PRIu64 ", voice starts %" PRIu64 "\n",
                     totals[0], totals[1], totals[2], totals[3], totals[4]);
        std::fprintf(text, "voices started in the window: %zu (wav time = (clock - %" PRIu64 ") / %u s)\n", window.size(), first_frame, clip_rate);
        for (const Record& r : window) {
            std::fprintf(text, "\nt=%.3f wav+%.3f s: %s [%s]\nAXPB words:", r.seconds, double(r.clock - std::min(r.clock, first_frame)) / clip_rate,
                         describe_voice(r.index, r.pb).c_str(), flag_names(r.flags).c_str());
            const auto* words = reinterpret_cast<const u16*>(&r.pb);
            for (std::size_t i = 0; i < sizeof(AXPB) / 2; ++i) std::fprintf(text, "%s%04x", i % 16 ? " " : "\n  ", words[i]);
            std::fputc('\n', text);
        }
        std::fclose(text);
    }
    std::fprintf(stderr, "[audio] t=%.3f clip %s%s (%.1f s, %zu voice starts)\n", now, path.c_str(),
                 wav ? ".wav" : ".wav FAILED", double(frames) / clip_rate, window.size());
    return wav;
}

void request_audio_clip(const char* reason) {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return;
    std::string why = reason ? reason : "request";
    std::thread([why] {
        save_audio_clip(why.c_str());
        busy = false;
    }).detach();
}

}
