// AX audio diagnostics: the voice parameter checks, the rate limiter, the clip ring, the WAV writer and
// a clip saved from the always-on state.
#include "../../src/ax/diagnostics.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace starlet::sdk;
namespace diag = starlet::ax::diag;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr std::size_t memory = 1 << 20;

// A healthy looping ADPCM voice at ratio 1.0.
AXPB good_voice() {
    AXPB pb{};
    pb.state = AX_PB_STATE_RUN;
    pb.srcSelect = 1;
    pb.src.ratioHi = 1;
    pb.addr.loopFlag = 1;
    pb.addr.format = AX_PB_FORMAT_ADPCM;
    pb.addr.loopAddressLo = 0x1000;
    pb.addr.endAddressLo = 0x2000;
    pb.addr.currentAddressLo = 0x1000;
    pb.adpcm.pred_scale = 0x13;
    pb.adpcmLoop.loop_pred_scale = 0x13;
    return pb;
}

void voice_checks() {
    check(diag::check_voice(good_voice(), memory) == 0, "a healthy voice has no findings");
    AXPB pb = good_voice();
    pb.src.ratioHi = pb.src.ratioLo = 0;
    check(diag::check_voice(pb, memory) == diag::ratio_zero, "ratio 0");
    pb = good_voice();
    pb.src.ratioHi = 2;
    check(diag::check_voice(pb, memory) == 0, "ratio exactly 2.0 passes");
    pb.src.ratioLo = 1;
    check(diag::check_voice(pb, memory) == diag::ratio_high, "ratio above 2.0");
    pb = good_voice();
    pb.srcSelect = 0;
    check(diag::check_voice(pb, memory) == diag::src_polyphase, "polyphase SRC");
    pb.srcSelect = 7;
    check(diag::check_voice(pb, memory) == diag::src_invalid, "invalid SRC select");
    pb = good_voice();
    pb.addr.endAddressHi = 0x40;  // 0x400000 nibbles = 2 MB
    check(diag::check_voice(pb, memory) == diag::address_range, "end address outside memory");
    pb = good_voice();
    pb.addr.format = AX_PB_FORMAT_PCM16;  // words: the same address is twice the byte offset
    pb.addr.endAddressHi = 0x8;
    check((diag::check_voice(pb, memory) & diag::address_range) != 0, "PCM16 address counts words");
    pb = good_voice();
    pb.addr.currentAddressLo = 0;
    pb.addr.currentAddressHi = 0x2000;
    check(diag::check_voice(pb, memory) == diag::address_range, "current address outside memory");
    pb = good_voice();
    pb.adpcm.pred_scale = 0x93;
    check(diag::check_voice(pb, memory) == diag::adpcm_predictor, "predictor word above 0x7f");
    pb = good_voice();
    pb.adpcm.pred_scale = 0x1D;
    check(diag::check_voice(pb, memory) == diag::adpcm_scale, "scale above 12");
    pb = good_voice();
    pb.adpcmLoop.loop_pred_scale = 0;
    check(diag::check_voice(pb, memory) == diag::loop_context, "looped ADPCM voice without a loop context");
    pb.addr.loopFlag = 0;
    check(diag::check_voice(pb, memory) == 0, "no loop, no loop context needed");
    pb = good_voice();
    pb.addr.format = AX_PB_FORMAT_PCM16;
    pb.adpcmLoop.loop_pred_scale = 0;
    check(diag::check_voice(pb, memory) == 0, "PCM voices have no ADPCM context");
    pb = good_voice();
    pb.lpf.on = 1;
    pb.itd.flag = 1;
    check(diag::check_voice(pb, memory) == (diag::lpf_on | diag::itd_on), "LPF and ITD");
    check(!(diag::severe_mask & (diag::lpf_on | diag::itd_on | diag::src_polyphase)), "LPF, ITD and polyphase are notes");
    check(diag::flag_names(diag::ratio_zero | diag::lpf_on) == "ratio-zero lpf-on", "flag names");
    check(diag::flag_names(0) == "-", "no flags");
    check(diag::describe_voice(7, good_voice()).find("voice 7 fmt 0x00 ratio 1.0000") == 0, "voice description");
}

void rate_limit() {
    diag::RateLimiter limiter(3, 200);
    unsigned dropped = 99;
    int allowed = 0;
    for (int i = 0; i < 10; ++i) allowed += limiter.allow(10, &dropped);
    check(allowed == 3 && limiter.suppressed() == 7, "three lines per window, the rest counted");
    check(dropped == 0, "nothing dropped before the first window");
    check(!limiter.allow(199, &dropped), "still the same window");
    check(limiter.allow(200, &dropped), "a new window allows again");
    check(dropped == 8, "the new window reports the dropped lines");
    check(limiter.allow(201, &dropped) && dropped == 0, "reported once");
    check(limiter.allow(1000, &dropped) && dropped == 0, "a later window starts clean");
}

void ring_order() {
    diag::ClipRing ring(10);
    std::vector<s16> l(7), r(7), a(7), b(7);
    for (int i = 0; i < 7; ++i) l[i] = s16(i), r[i] = s16(-i), a[i] = s16(100 + i), b[i] = s16(200 + i);
    ring.push(l.data(), r.data(), a.data(), b.data(), 7);
    check(ring.size() == 7 && ring.total() == 7, "ring holds what was pushed");
    auto s = ring.snapshot();
    check(s.size() == 28 && s[0] == 0 && s[1] == 0 && s[2] == 100 && s[3] == 200 && s[24] == 6 && s[25] == -6, "snapshot interleaves");
    ring.push(l.data(), r.data(), nullptr, nullptr, 7);
    check(ring.size() == 10 && ring.total() == 14, "ring keeps its capacity");
    s = ring.snapshot();
    // Frames 4..13 survive: l values 4,5,6 then 0..6.
    check(s.size() == 40 && s[0] == 4 && s[8] == 6 && s[12] == 0 && s[36] == 6, "oldest first after wrapping");
    check(s[14] == 0 && s[15] == 0 && s[2] == 104, "aux channels follow their frames (zero without a callback)");
}

std::string temporary_directory() {
    char path[] = "/tmp/starlet-axdiag-XXXXXX";
    return mkdtemp(path) ? path : "";
}

unsigned le32(const std::string& s, std::size_t at) {
    return unsigned((unsigned char)s[at]) | unsigned((unsigned char)s[at + 1]) << 8 | unsigned((unsigned char)s[at + 2]) << 16 |
           unsigned((unsigned char)s[at + 3]) << 24;
}

std::string slurp(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void wav_writer(const std::string& directory) {
    const s16 samples[6] = {1, -2, 300, -32768, 32767, 5};
    const std::string path = directory + "/t.wav";
    check(diag::write_wav(path, samples, 3, 2, 32000), "the WAV is written");
    const std::string data = slurp(path);
    check(data.size() == 44 + 12, "header and data size");
    check(data.compare(0, 4, "RIFF") == 0 && data.compare(8, 8, "WAVEfmt ") == 0 && data.compare(36, 4, "data") == 0, "chunk ids");
    check(le32(data, 4) == 36 + 12 && le32(data, 40) == 12, "chunk sizes");
    check(le32(data, 24) == 32000 && le32(data, 28) == 32000 * 4 && (unsigned char)data[22] == 2 && (unsigned char)data[34] == 16, "format");
    check((unsigned char)data[44] == 1 && (unsigned char)data[45] == 0 && (unsigned char)data[46] == 0xFE && (unsigned char)data[47] == 0xFF &&
              (unsigned char)data[50] == 0x00 && (unsigned char)data[51] == 0x80, "little-endian samples");
    check(access((path + ".tmp").c_str(), F_OK) != 0, "no temporary file stays");
    check(!diag::write_wav(directory + "/missing/t.wav", samples, 3, 2, 32000), "an unwritable path fails");
}

void clip_from_state(const std::string& directory) {
    starlet::ax::set_diagnostics_directory(directory.c_str());
    diag::reset();
    const AXPB voice = good_voice();
    AXPB bad = good_voice();
    bad.src.ratioHi = 3;
    std::vector<s16> out(320);
    std::vector<s32> aux_a(480, 0), aux_b(480, 0);
    for (int f = 0; f < 250; ++f) {
        for (int i = 0; i < 160; ++i) out[2 * i] = s16(f), out[2 * i + 1] = s16(-f);
        if (f == 10) diag::running_voice(3, voice, true, memory);
        if (f == 20) diag::running_voice(4, bad, true, memory);
        if (f == 21) diag::running_voice(4, bad, false, memory);  // unchanged: no second line
        aux_a[0] = 40000;                                             // saturated aux A return
        aux_a[160] = 100;
        diag::end_frame(out.data(), aux_a.data(), f < 100 ? nullptr : aux_b.data(), f == 30 ? 5 : 0, f == 40 ? 2 : 0);
    }
    check(starlet::ax::save_audio_clip("test"), "a clip is saved");
    check(starlet::ax::save_audio_clip("exit"), "the exit clip is saved");
    bool found = false;
    std::string wav, text;
    for (const char* name : {"audio-clip-exit.wav", "audio-clip-exit.txt"}) {
        const std::string content = slurp(directory + "/" + name);
        check(!content.empty(), name);
        (name[strlen(name) - 1] == 'v' ? wav : text) = content;
        found = true;
    }
    check(found && le32(wav, 40) == 250 * 160 * 4 * 2, "the WAV holds the 250 frames of four channels");
    // Frame 249 is the last. The AI buffer carries the right channel first, the WAV the left.
    const std::size_t last = 44 + (250 * 160 - 1) * 8;
    check((unsigned char)wav[last] == (unsigned char)(-249 & 0xFF) && (unsigned char)wav[last + 2] == 249, "main L and R in order");
    check(text.find("voices started in the window: 2") != std::string::npos, "both voice starts are listed");
    check(text.find("voice 4 fmt 0x00 ratio 3.0000") != std::string::npos && text.find("[ratio-above-2]") != std::string::npos,
          "the anomalous start carries its flags and parameters");
    check(text.find("saturated main 5 aux-A 250") != std::string::npos, "totals count saturated samples");
    check(text.find("reads past end 2") != std::string::npos, "totals count reads past end");
}

}

int main() {
    voice_checks();
    rate_limit();
    ring_order();
    const std::string directory = temporary_directory();
    check(!directory.empty(), "a temporary directory");
    if (!directory.empty()) {
        wav_writer(directory);
        clip_from_state(directory);
        const std::string command = "rm -rf '" + directory + "'";
        if (std::system(command.c_str()) != 0) std::fprintf(stderr, "could not remove %s\n", directory.c_str());
    }
    std::printf("%s\n", failures ? "ax diagnostics: failures" : "ax diagnostics: ok");
    return failures ? 1 : 0;
}
