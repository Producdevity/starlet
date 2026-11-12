#pragma once
#include "starlet/thp/thp.hpp"
#include <cstring>

namespace starlet::thp {
// Bounds-checked big-endian reads over movie bytes.
class View {
public:
    View() = default;
    View(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {
        if (!data && size) throw Error("null THP data");
    }
    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    void check(std::size_t offset, std::size_t count) const {
        if (offset > size_ || count > size_ - offset) throw Error("THP data ends early");
    }
    View subview(std::size_t offset, std::size_t count) const {
        check(offset, count);
        return View(offset ? data_ + offset : data_, count);
    }
    std::uint8_t u8(std::size_t at) const {
        check(at, 1);
        return data_[at];
    }
    std::uint16_t be16(std::size_t at) const {
        check(at, 2);
        return std::uint16_t(data_[at] << 8 | data_[at + 1]);
    }
    std::uint32_t be32(std::size_t at) const {
        check(at, 4);
        return std::uint32_t(data_[at]) << 24 | std::uint32_t(data_[at + 1]) << 16 | std::uint32_t(data_[at + 2]) << 8 | data_[at + 3];
    }
    float be_float(std::size_t at) const {
        const auto bits = be32(at);
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};
}
