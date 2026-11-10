#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace starlet::disc {

// Every failure to open, read or install a disc image. what() is a sentence for the user.
class DiscError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Byte-addressed disc contents, whatever the container. Every read() may run concurrently
// with other reads of the same image.
class DiscImage {
public:
    virtual ~DiscImage() = default;
    // The size of the original disc.
    virtual std::uint64_t size() const noexcept = 0;
    // Fills output with size bytes from offset, or throws DiscError.
    virtual void read(std::uint64_t offset, void* output, std::size_t size) = 0;
};

// A plain disc copy (.iso, .gcm). A trimmed copy is shorter than the disc: size() is the
// file size, and reads past it throw.
class IsoImage final : public DiscImage {
public:
    explicit IsoImage(const std::string& path);
    ~IsoImage() override;
    IsoImage(const IsoImage&) = delete;
    IsoImage& operator=(const IsoImage&) = delete;

    std::uint64_t size() const noexcept override { return size_; }
    void read(std::uint64_t offset, void* output, std::size_t size) override;

private:
    int fd_ = -1;
    std::uint64_t size_ = 0;
};

// The padding generator of GameCube and Wii discs: lagged Fibonacci with f = xor, j = 32,
// k = 521, seeded with 68 bytes. Its output restarts at every 0x8000-byte boundary.
class JunkGenerator {
public:
    explicit JunkGenerator(const std::uint8_t* seed);
    void skip(std::size_t bytes);
    void generate(std::uint8_t* output, std::size_t bytes);

private:
    void forward();
    std::array<std::uint32_t, 521> buffer_{};
    std::size_t position_ = 0;
};

// Wii partition data as wia_part_data_t describes it, in 0x8000-byte disc sectors.
struct WiaPartitionData {
    std::uint32_t first_sector = 0, sectors = 0, group = 0, groups = 0;
};
struct WiaPartition {
    WiaPartitionData data[2];
};

// WIA 1.00 and RVZ images (Dolphin's docs/WiaAndRvz.md) of GameCube and Wii discs, stored
// without compression or with Zstandard. Wii partition data is stored decrypted without
// hashes, 0x7C00 bytes for each 0x8000-byte sector.
class WiaImage final : public DiscImage {
public:
    explicit WiaImage(const std::string& path);
    ~WiaImage() override;
    WiaImage(const WiaImage&) = delete;
    WiaImage& operator=(const WiaImage&) = delete;

    bool rvz() const noexcept;
    bool wii() const noexcept;
    std::uint64_t size() const noexcept override;
    // Disc bytes outside Wii partition data; a GameCube disc has no partition data.
    void read(std::uint64_t offset, void* output, std::size_t size) override;

    const std::vector<WiaPartition>& partitions() const noexcept;
    // Offset 0 is the first byte of the partition's first data sector; hashes are not counted.
    void read_partition(std::size_t partition, std::uint64_t offset, void* output, std::size_t size);
    std::uint64_t partition_data_size(std::size_t partition) const;
    // The index in partitions() of the DATA partition (type 0), found through the disc's
    // partition table at 0x40000.
    std::size_t data_partition();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// A WIA or RVZ image by its signature, an ISO by its GameCube or Wii magic word, otherwise
// by the extension (.wia, .rvz, .iso, .gcm).
std::unique_ptr<DiscImage> open_disc_image(const std::string& path);

// The game's own file system: the whole disc on GameCube, the DATA partition on Wii. Offsets
// count from boot.bin. A Wii volume needs a WIA or RVZ image, since an ISO keeps the
// partition encrypted.
class GameVolume {
public:
    explicit GameVolume(DiscImage& image);

    DiscImage& image() const noexcept { return image_; }
    bool wii() const noexcept { return wia_ != nullptr; }
    // boot.bin and the FST store offsets shifted right by this: 2 on Wii, 0 on GameCube.
    unsigned shift() const noexcept { return wii() ? 2 : 0; }
    std::uint64_t size() const;
    void read(std::uint64_t offset, void* output, std::size_t size);

    // sys/boot.bin.
    std::array<std::uint8_t, 0x440> boot();
    // sys/main.dol, up to the end of its furthest section.
    std::vector<std::uint8_t> main_dol();
    // sys/fst.bin.
    std::vector<std::uint8_t> fst();

private:
    DiscImage& image_;
    WiaImage* wia_ = nullptr;
    std::size_t partition_ = 0;
};

} // namespace starlet::disc
