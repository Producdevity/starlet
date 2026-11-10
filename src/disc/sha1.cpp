#include "starlet/disc/sha1.hpp"

#include <algorithm>
#include <cstring>

namespace starlet::disc {
namespace {
std::uint32_t rotate(std::uint32_t value, unsigned bits) { return (value << bits) | (value >> (32 - bits)); }
}

void Sha1::block(const std::uint8_t* data) {
    std::uint32_t w[80];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = std::uint32_t{data[i * 4]} << 24 | std::uint32_t{data[i * 4 + 1]} << 16 |
               std::uint32_t{data[i * 4 + 2]} << 8 | data[i * 4 + 3];
    for (unsigned i = 16; i < 80; ++i) w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    auto [a, b, c, d, e] = state_;
    for (unsigned i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        const auto next = rotate(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotate(b, 30); b = a; a = next;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e;
}

void Sha1::update(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    length_ += size;
    if (used_) {
        const auto take = std::min(size, pending_.size() - used_);
        std::memcpy(pending_.data() + used_, bytes, take);
        used_ += take; bytes += take; size -= take;
        if (used_ < pending_.size()) return;
        block(pending_.data());
        used_ = 0;
    }
    for (; size >= 64; bytes += 64, size -= 64) block(bytes);
    if (size) std::memcpy(pending_.data(), bytes, size);
    used_ = size;
}

std::array<std::uint8_t, 20> Sha1::finish() {
    const auto bits = length_ * 8;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (used_ != 56) update(&zero, 1);
    std::uint8_t tail[8];
    for (unsigned i = 0; i < 8; ++i) tail[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(tail, 8);
    std::array<std::uint8_t, 20> digest{};
    for (unsigned i = 0; i < 20; ++i) digest[i] = static_cast<std::uint8_t>(state_[i / 4] >> (24 - 8 * (i % 4)));
    return digest;
}

std::string Sha1::hex() {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : finish()) { text += digits[byte >> 4]; text += digits[byte & 15]; }
    return text;
}

std::string sha1_hex(const void* data, std::size_t size) {
    Sha1 hash;
    hash.update(data, size);
    return hash.hex();
}

} // namespace starlet::disc
