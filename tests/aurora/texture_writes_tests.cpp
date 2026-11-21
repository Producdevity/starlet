#include "texture_writes.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

using aurora::starlet_backend::TextureWrites;

namespace {
void check(bool value, const char* what) { if (!value) throw std::runtime_error(what); }

std::vector<std::uint8_t> texture(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(seed + i * 7);
    return bytes;
}
bool same(const void* a, const std::vector<std::uint8_t>& b) { return !std::memcmp(a, b.data(), b.size()); }

void unwritten_textures_are_read_in_place() {
    TextureWrites writes;
    auto data = texture(4096, 1);
    check(writes.load(data.data(), data.size(), false) == data.data(), "an unwritten texture is read where it is");
    check(writes.load(data.data(), data.size(), true) == data.data(), "a texture that already is a copy is read where it is");
    check(writes.copies == 0, "nothing is copied");
}

void written_textures_are_copied_once() {
    TextureWrites writes;
    auto data = texture(4096, 2);
    writes.note(data.data(), data.size());
    const auto* first = writes.load(data.data(), data.size(), false);
    check(first != data.data() && same(first, data), "a texture written this frame is copied");
    writes.end_frame();
    for (unsigned frame = 0; frame < 8; ++frame) {
        check(writes.load(data.data(), data.size(), false) == first, "later loads reuse the copy");
        writes.end_frame();
    }
    check(writes.copies == 1 && writes.hits == 8, "the texture was copied once");
}

void a_reported_write_replaces_the_copy() {
    TextureWrites writes;
    auto data = texture(4096, 3);
    writes.note(data.data(), data.size());
    const auto* first = writes.load(data.data(), data.size(), false);
    writes.end_frame();
    data[100] ^= 0x55;
    writes.note(data.data() + 96, 8);
    const auto* second = writes.load(data.data(), data.size(), false);
    check(second != first && same(second, data), "a reported write is read at the next load");
    check(same(first, texture(4096, 3)), "the earlier copy keeps the bytes of its load");
    // A write after the load leaves the loaded copy with the bytes the draw was recorded with.
    auto before = data;
    data[7] ^= 0x11;
    writes.note(data.data() + 7, 1);
    check(same(second, before), "a copy keeps the bytes of its load");
    writes.end_frame();
    check(same(writes.load(data.data(), data.size(), false), data), "the next load sees the write");
}

void an_unreported_write_is_found_by_the_comparison() {
    TextureWrites writes;
    auto data = texture(4096, 4);
    writes.note(data.data(), data.size());
    writes.load(data.data(), data.size(), false);
    writes.end_frame();
    data[2000] ^= 0x7f;
    bool seen = false;
    for (unsigned frame = 0; frame < 2 * TextureWrites::verify_period && !seen; ++frame) {
        seen = same(writes.load(data.data(), data.size(), false), data);
        writes.end_frame();
    }
    check(seen, "bytes changed without a report are read within one verification period");
}

void neighbours_do_not_replace_a_copy() {
    TextureWrites writes;
    auto data = texture(8192, 5);
    writes.note(data.data(), data.size());
    const auto* first = writes.load(data.data(), 4096, false);
    writes.end_frame();
    writes.note(data.data() + 4096, 64);
    check(writes.load(data.data(), 4096, false) == first, "a write beside the texture keeps its copy");
}

void textures_rewritten_every_frame_stop_being_kept() {
    TextureWrites writes;
    auto data = texture(4096, 6);
    for (unsigned frame = 0; frame < 12; ++frame) {
        data[frame] ^= 1;
        writes.note(data.data(), data.size());
        check(same(writes.load(data.data(), data.size(), false), data), "a texture rewritten every frame is read as written");
        writes.end_frame();
    }
    check(writes.copies == 12, "each rewrite is copied");
}

void a_full_pool_falls_back_to_frame_copies() {
    TextureWrites writes;
    std::vector<std::vector<std::uint8_t>> textures;
    bool reset = false;
    for (unsigned i = 0; i < 40; ++i) {
        textures.push_back(texture(1 << 20, static_cast<std::uint8_t>(i)));
        writes.note(textures.back().data(), textures.back().size());
        const auto* copy = writes.load(textures.back().data(), textures.back().size(), false);
        check(copy == textures.back().data() || same(copy, textures.back()), "every load reads its own bytes");
        if (i % 4 == 3) { reset |= writes.end_frame(); }
    }
    check(reset, "an exhausted pool drops its kept copies at a frame boundary");
    writes.end_frame();
    for (const auto& data : textures) check(same(writes.load(data.data(), data.size(), false), data), "loads after the reset read their own bytes");
}
}

int main() {
    try {
        unwritten_textures_are_read_in_place();
        written_textures_are_copied_once();
        a_reported_write_replaces_the_copy();
        an_unreported_write_is_found_by_the_comparison();
        neighbours_do_not_replace_a_copy();
        textures_rewritten_every_frame_stop_being_kept();
        a_full_pool_falls_back_to_frame_copies();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "texture write tests passed\n";
    return 0;
}
