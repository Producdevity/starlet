#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace starlet::disc {

// FIPS 180-4 SHA-1, for checking disc images and extracted files against pinned digests.
class Sha1 {
public:
    void update(const void* data, std::size_t size);
    // Both end the hash; the object is not reused afterwards.
    std::array<std::uint8_t, 20> finish();
    std::string hex();

private:
    void block(const std::uint8_t* data);
    std::array<std::uint32_t, 5> state_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    std::array<std::uint8_t, 64> pending_{};
    std::size_t used_ = 0;
    std::uint64_t length_ = 0;
};

// Lowercase hex SHA-1 of one buffer.
std::string sha1_hex(const void* data, std::size_t size);

} // namespace starlet::disc
