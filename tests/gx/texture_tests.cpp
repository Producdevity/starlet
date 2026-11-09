#include "starlet/gx/texture.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace starlet::gx;
namespace {
void check(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
void ok(const TextureResult& result) { check(static_cast<bool>(result), result.message.c_str()); }
struct Memory : MemoryReader {
    std::vector<std::uint8_t> image;
    std::vector<std::uint8_t> tlut;
    std::uint32_t image_token = 0x12000000;
    unsigned calls = 0;
    std::size_t last_size = 0;
    bool fail = false, throws = false, borrow = false, short_view = false;
    ByteView view(std::uint32_t address, std::size_t size) override {
        if (!borrow) return {};
        if (throws) throw std::runtime_error("provider view exception");
        if (fail || address != image_token || size > image.size()) return {};
        return {image.data(), short_view && size ? size - 1 : size};
    }
    bool copy(std::uint32_t address, std::uint8_t* dest, std::size_t size) override {
        ++calls; last_size = size;
        if (throws) throw std::runtime_error("provider exception");
        const auto* src = address == image_token ? &image : address == 0x2000 ? &tlut : nullptr;
        if (fail) { if (size) dest[0] = 0xff; return false; }
        if (!src || size > src->size()) return false;
        std::copy_n(src->data(), size, dest);
        return true;
    }
};
struct Sink : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return true; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};
struct Test {
    Memory memory;
    Sink sink;
    Decoder decoder{memory,sink};
    void bp(unsigned reg, std::uint32_t value) {
        check(static_cast<bool>(decoder.write_u8(0x61)), "BP opcode");
        check(static_cast<bool>(decoder.write_u32((reg << 24) | value)), "BP payload");
    }
    void setup(unsigned format, unsigned width, unsigned height, unsigned map = 0,
               unsigned mode0 = 0, unsigned mode1 = 0) {
        const unsigned bank = (map < 4 ? 0x80 : 0xa0) + (map & 3);
        bp(bank, mode0); bp(bank+4, mode1);
        bp(bank+8, (format << 20) | ((height-1) << 10) | (width-1));
        bp(bank+12, 0); bp(bank+16, 0); bp(bank+20, memory.image_token >> 5);
    }
    void palette(unsigned format, std::size_t offset, unsigned chunks) {
        bp(0x98, static_cast<unsigned>(offset / 512) | (format << 10));
        bp(0x64, 0x2000 >> 5);
        bp(0x65, static_cast<unsigned>(offset / 512) | (chunks << 10));
    }
    TextureResult capture(unsigned map = 0, TextureLimits limits = {},
                          TextureCmprModel model = TextureCmprModel::reject_unverified, TextureCache* cache = nullptr) {
        return capture_texture(decoder.state(), static_cast<std::uint8_t>(map), memory, model, limits, cache);
    }
};
void pixel(const TextureSnapshot& image, unsigned x, unsigned y, std::array<std::uint8_t,4> expected,
           unsigned lod = 0) {
    const auto& level = image.levels().at(lod);
    check(x < level.width && y < level.height, "test pixel bounds");
    const auto offset = (std::size_t{y} * level.width + x) * 4;
    check(std::equal(expected.begin(), expected.end(), level.rgba.begin() + static_cast<std::ptrdiff_t>(offset)),
          "decoded texel mismatch");
}
void error(const TextureResult& result, TextureError expected) {
    check(result.error == expected && !result.snapshot, "error must publish no snapshot");
}
void content_cache(bool borrow) {
    TextureCache cache;
    check(cache.stats().bytes>0,"cache includes reserved entry metadata");
    Test t;t.memory.borrow=borrow;t.memory.image.assign(32,0x45);t.setup(1,8,4);
    auto capture=[&] {return t.capture(0,{},TextureCmprModel::reject_unverified,&cache);};
    auto first=capture();ok(first);auto second=capture();ok(second);
    check(&first.snapshot->levels()==&second.snapshot->levels(),"identical content shares immutable decoded levels");
    t.bp(0x80,5);auto sampler=capture();ok(sampler);
    check(&first.snapshot->levels()==&sampler.snapshot->levels() && first.snapshot->state_revision()!=sampler.snapshot->state_revision(),
          "sampler state and revision remain fresh on cache hits");
    t.memory.image[0]=0x91;auto changed=capture();ok(changed);
    pixel(*first.snapshot,0,0,{0x45,0x45,0x45,0x45});pixel(*changed.snapshot,0,0,{0x91,0x91,0x91,0x91});
    check(&first.snapshot->levels()!=&changed.snapshot->levels(),"same-address mutation decodes new content");
    t.memory.image_token+=32;t.setup(1,8,4);auto moved=capture();ok(moved);
    check(&moved.snapshot->levels()==&changed.snapshot->levels(),"cache identity is content, not address");
    if (borrow) {
        check(t.memory.calls==0,"borrowed capture compares current bytes without temporary copies");
        t.memory.short_view=true;auto fallback=capture();ok(fallback);t.memory.short_view=false;
        check(t.memory.calls==1 && &fallback.snapshot->levels()==&changed.snapshot->levels(),
              "partial view falls back to an exact owned copy");
    }
    t.memory.fail=true;error(capture(),TextureError::memory_read);t.memory.fail=false;
    t.memory.throws=true;bool threw=false;try{capture();}catch(const std::runtime_error&){threw=true;}
    check(threw,"cache hit still reads current source and propagates provider failure");t.memory.throws=false;
    t.setup(8,8,8);t.memory.image.assign(32,0);t.memory.tlut.assign(32,0);t.memory.tlut[0]=0xff;t.memory.tlut[1]=0x12;t.palette(0,0,1);
    auto paletted=capture();ok(paletted);t.memory.tlut[1]=0x34;t.palette(0,0,1);auto palette_changed=capture();ok(palette_changed);
    pixel(*paletted.snapshot,0,0,{0x12,0x12,0x12,255});pixel(*palette_changed.snapshot,0,0,{0x34,0x34,0x34,255});
    check(&paletted.snapshot->levels()!=&palette_changed.snapshot->levels(),"same-address palette mutation invalidates decoded content");
    TextureCache one({4096,1});t.setup(1,8,4);t.memory.image.assign(32,1);
    auto a=t.capture(0,{},TextureCmprModel::reject_unverified,&one);ok(a);
    t.memory.image[0]=2;ok(t.capture(0,{},TextureCmprModel::reject_unverified,&one));t.memory.image[0]=1;
    auto b=t.capture(0,{},TextureCmprModel::reject_unverified,&one);ok(b);
    check(one.stats().entries==1 && one.stats().bytes<=4096 && one.stats().misses==3 && &a.snapshot->levels()!=&b.snapshot->levels(),
          "LRU eviction bounds retention without invalidating old packets");
    t.memory.image.clear();t.memory.image.shrink_to_fit();
    pixel(*changed.snapshot,0,0,{0x91,0x91,0x91,0x91});
    t.memory.image.assign(32,0x45);t.memory.image[0]=0x91;auto restored=capture();ok(restored);
    check(&restored.snapshot->levels()==&changed.snapshot->levels(),"cache owns source bytes after borrowed allocation destruction");
    TextureCache tiny({1,1});ok(t.capture(0,{},TextureCmprModel::reject_unverified,&tiny));
    check(tiny.stats().bytes==0 && tiny.stats().entries==0,"oversized entry is decoded without caching");
}
void cache_across_maps_and_eviction() {
    Test t; t.memory.borrow=true; TextureCache cache({16384,3});
    for (unsigned step=0;step<80;++step) {
        const unsigned map=std::array<unsigned,3>{{0,3,7}}[step%3];
        const auto value=static_cast<std::uint8_t>((step/2)%5);
        t.memory.image.assign(32,value);t.setup(1,8,4,map);
        auto cached=t.capture(map,{},TextureCmprModel::reject_unverified,&cache);
        auto fresh=t.capture(map);ok(cached);ok(fresh);
        check(cached.snapshot->levels()[0].rgba==fresh.snapshot->levels()[0].rgba,
              "map switches, mutations and repeated eviction preserve current pixels");
        check(cache.stats().entries<=3 && cache.stats().bytes<=16384,"multi-map cache remains bounded");
    }
}
void direct_formats() {
    struct Example { unsigned format; std::vector<std::uint8_t> data; std::array<std::uint8_t,4> c0, c1; };
    const Example examples[] = {
        {0,{0xa5},{170,170,170,170},{85,85,85,85}},
        {1,{0x12,0x89},{18,18,18,18},{137,137,137,137}},
        {2,{0xa3,0x5c},{51,51,51,170},{204,204,204,85}},
        {3,{0x12,0x34,0xab,0xcd},{52,52,52,18},{205,205,205,171}},
        {4,{0xa5,0xa5,0x07,0xe0},{165,182,41,255},{0,255,0,255}},
        {5,{0xfc,0x1f,0x31,0x2f},{255,0,255,255},{17,34,255,109}}
    };
    for (const auto& example : examples) {
        Test t; t.setup(example.format,2,1); t.memory.image = example.data; t.memory.image.resize(32,0xee);
        const auto image = t.capture(); ok(image);
        pixel(*image.snapshot,0,0,example.c0); pixel(*image.snapshot,1,0,example.c1);
        check(image.snapshot->levels()[0].rgba.size() == 8 && t.memory.last_size == 32, "exact output and tile source size");
    }
    Test rgba; rgba.setup(6,5,5); rgba.memory.image.resize(256,0xee);
    const std::array<std::array<std::uint8_t,4>,4> colors{{{1,2,3,4},{5,6,7,8},{9,10,11,12},{13,14,15,16}}};
    for (unsigned tile = 0; tile < 4; ++tile) for (unsigned n = 0; n < 16; ++n) {
        rgba.memory.image[tile*64+n*2] = colors[tile][3];
        rgba.memory.image[tile*64+n*2+1] = colors[tile][0];
        rgba.memory.image[tile*64+32+n*2] = colors[tile][1];
        rgba.memory.image[tile*64+33+n*2] = colors[tile][2];
    }
    const auto image = rgba.capture(); ok(image);
    pixel(*image.snapshot,0,0,colors[0]); pixel(*image.snapshot,3,3,colors[0]);
    pixel(*image.snapshot,4,0,colors[1]); pixel(*image.snapshot,0,4,colors[2]); pixel(*image.snapshot,4,4,colors[3]);
    check(image.snapshot->levels()[0].rgba.size() == 100, "RGBA8 excludes padded texels");
    Test alpha; alpha.setup(5,8,1); alpha.memory.image.resize(64);
    const std::uint8_t expanded[] = {0,36,73,109,146,182,219,255};
    for (unsigned i=0;i<8;++i) alpha.memory.image[(i/4)*32+(i%4)*2] = static_cast<std::uint8_t>(i << 4);
    const auto result = alpha.capture(); ok(result);
    for (unsigned i=0;i<8;++i) pixel(*result.snapshot,i,0,{0,0,0,expanded[i]});
}
void tiling_and_padding() {
    struct Shape { unsigned format, w, h; };
    for (auto shape : {Shape{0,8,8},Shape{1,8,4},Shape{2,8,4},Shape{3,4,4},Shape{4,4,4},Shape{5,4,4}}) {
        Test t; t.setup(shape.format,shape.w+1,shape.h+1); t.memory.image.resize(128);
        const std::uint8_t values[] = {0x00,0x55,0xaa,0xff};
        for (unsigned tile=0;tile<4;++tile) std::fill_n(t.memory.image.begin()+tile*32,32,values[tile]);
        auto result = t.capture(); ok(result);
        for (unsigned corner=0;corner<4;++corner) {
            Test single; single.setup(shape.format,1,1); single.memory.image.assign(32,values[corner]);
            auto one = single.capture(); ok(one);
            std::array<std::uint8_t,4> expected{};
            std::copy_n(one.snapshot->levels()[0].rgba.begin(),4,expected.begin());
            pixel(*result.snapshot,corner%2*shape.w,corner/2*shape.h,expected);
        }
        check(result.snapshot->source_bytes_read()==128, "partial edge tiles retained in input");
        t.memory.image.resize(127); error(t.capture(),TextureError::memory_read);
    }
}
void palettes() {
    for (unsigned format : {8u,9u,10u}) for (unsigned palette : {0u,1u,2u}) {
        Test t; t.setup(format,2,1); t.memory.image.assign(32,0xff);
        if (format==8) t.memory.image[0]=0x12;
        else if (format==9) { t.memory.image[0]=1;t.memory.image[1]=2; }
        else { t.memory.image[0]=0xc0;t.memory.image[1]=1;t.memory.image[2]=0x80;t.memory.image[3]=2; }
        t.memory.tlut.assign(32,0); t.memory.tlut[2]=0xa5;t.memory.tlut[3]=0xa5;
        t.memory.tlut[4]=0x31;t.memory.tlut[5]=0x2f; t.palette(palette,1024,1);
        const auto r=t.capture(); ok(r);
        const std::array<std::array<std::uint8_t,4>,3> expected{{{165,165,165,165},{165,182,41,255},{74,107,41,255}}};
        pixel(*r.snapshot,0,0,expected[palette]);
        check(r.snapshot->palette().size()==1 && r.snapshot->palette()[0].first_index==0, "only visible palette chunk owned");
        check(r.snapshot->descriptor().palette_offset==1024, "TLUT offset uses 512 byte units");
        check(r.snapshot->palette()[0].generation!=0, "TLUT generation captured");
    }
    Test t; t.setup(9,2,1); t.memory.image.assign(32,0); t.memory.image[1]=16;
    t.memory.tlut.assign(64,0); t.memory.tlut[0]=0x11;t.memory.tlut[1]=0x22;t.memory.tlut[32]=0x33;t.memory.tlut[33]=0x44;
    t.palette(0,0,2); const State prior=t.decoder.state(); const auto before=t.capture(); ok(before);
    t.memory.tlut.assign(32,0);t.memory.tlut[0]=0x55;t.memory.tlut[1]=0x66;t.palette(0,0,1);
    const auto after=t.capture();ok(after);
    pixel(*before.snapshot,0,0,{34,34,34,17});pixel(*after.snapshot,0,0,{102,102,102,85});
    pixel(*after.snapshot,1,0,{68,68,68,51});
    check(after.snapshot->palette()[0].generation>before.snapshot->palette()[0].generation,"same-frame palette replacement generation");
    auto old=capture_texture(prior,0,t.memory);ok(old);pixel(*old.snapshot,0,0,{34,34,34,17});
    check(before.snapshot->palette()[0].bytes[0]==0x11,"captured palette bytes remain owned");
    Test sparse; sparse.setup(10,1,1);sparse.memory.image.assign(32,0xff);sparse.memory.image[0]=1;sparse.memory.image[1]=0;
    sparse.memory.tlut.assign(32,0x99);sparse.palette(0,512,1);sparse.bp(0x98,0);
    auto r=sparse.capture();ok(r);pixel(*r.snapshot,0,0,{153,153,153,153});
    check(r.snapshot->palette()[0].first_index==256,"unreferenced unloaded TLUT prefix does not matter");
    sparse.memory.image[0]=0;error(sparse.capture(),TextureError::palette_read);
    sparse.bp(0x98,1023);sparse.memory.image[0]=0x3f;sparse.memory.image[1]=0xff;
    error(sparse.capture(),TextureError::palette_read);
}
void mipmaps_and_samplers() {
    for (unsigned map=0;map<8;++map) {
        Test t; t.setup(1,16,8,map,1u|(2u<<2)|16u|(6u<<5)|(0xf0u<<9)|(2u<<19)|(1u<<21),16u|(56u<<8));
        t.memory.image.assign(256,0);
        const std::size_t offsets[]={0,128,160,192,224},sizes[]={128,32,32,32,32};
        const unsigned widths[]={16,8,4,2,1},heights[]={8,4,2,1,1};
        for(unsigned i=0;i<5;++i)std::fill_n(t.memory.image.begin()+static_cast<std::ptrdiff_t>(offsets[i]),sizes[i],static_cast<std::uint8_t>(i+1));
        auto r=t.capture(map);ok(r);check(r.snapshot->levels().size()==5,"fractional max LOD captures upper mip");
        for(unsigned i=0;i<5;++i){const auto& l=r.snapshot->levels()[i];check(l.width==widths[i]&&l.height==heights[i]&&l.source_offset==offsets[i]&&l.source_size==sizes[i],"mip shape/offset");const auto v=static_cast<std::uint8_t>(i+1);pixel(*r.snapshot,0,0,{v,v,v,v},i);}
        const auto& d=r.snapshot->descriptor();const auto& s=d.sampler;
        check(d.map==map&&d.address==t.memory.image_token,"map bank/address token");
        check(s.wrap_s==TextureWrap::repeat&&s.wrap_t==TextureWrap::mirror&&s.linear_magnification&&s.min_filter==TextureMinFilter::linear_mip_linear,"wrap/filter preservation");
        check(s.min_lod_sixteenths==16&&s.max_lod_sixteenths==56&&s.lod_bias_thirtyseconds==-16&&s.anisotropy==4&&s.edge_lod&&s.bias_clamp,"LOD/aniso preservation");
        TextureLimits limits;limits.max_storage_bytes=r.snapshot->peak_storage_bytes();ok(t.capture(map,limits));
        --limits.max_storage_bytes;error(t.capture(map,limits),TextureError::storage_limit);
        t.memory.image.resize(255);error(t.capture(map),TextureError::memory_read);
    }
    for(unsigned filter:{0u,1u,2u,4u,5u,6u}) {
        Test t;t.setup(1,8,1,0,filter<<5,160u<<8);t.memory.image.assign(128,9);auto r=t.capture();ok(r);
        check(r.snapshot->levels().size()==((filter==0||filter==4)?1:4),"filter mip selection and 1x1 stop");
    }
}
void sampler_boundaries() {
    for (unsigned bias : {0u,127u,128u,255u}) {
        Test t; t.setup(1,1,1,0,256u|(bias<<9),160u|(160u<<8)); t.memory.image.assign(32,1);
        const auto r=t.capture(); ok(r); const auto& s=r.snapshot->descriptor().sampler;
        check(!s.edge_lod&&!s.bias_clamp&&s.anisotropy==1,"diagonal LOD preserves independent flags");
        check(s.lod_bias_thirtyseconds==(bias<128?int(bias):int(bias)-256),"signed bias boundaries");
        check(s.min_lod_sixteenths==160&&s.max_lod_sixteenths==160,"maximum LOD fields preserved without mip reads");
        check(r.snapshot->levels().size()==1,"non-mip filter ignores mip range for storage");
    }
}
void compression() {
    Test t;t.setup(14,8,8);t.memory.image.assign(32,0);
    const std::uint8_t blocks[]={0xf8,0,0,0x1f,0x1b,0x1b,0x1b,0x1b,
                                 0,0,0xff,0xff,0x1b,0x1b,0x1b,0x1b,
                                 0x07,0xe0,0,0,0,0,0,0,
                                 0,0x1f,0,0,0,0,0,0};
    std::copy(std::begin(blocks),std::end(blocks),t.memory.image.begin());
    error(t.capture(),TextureError::unsupported_cmpr_model);
    auto r=t.capture(0,{},TextureCmprModel::s3tc_candidate_v1);ok(r);
    pixel(*r.snapshot,0,0,{255,0,0,255});pixel(*r.snapshot,1,0,{0,0,255,255});
    pixel(*r.snapshot,2,0,{170,0,85,255});pixel(*r.snapshot,3,0,{85,0,170,255});
    pixel(*r.snapshot,4,0,{0,0,0,255});pixel(*r.snapshot,5,0,{255,255,255,255});
    pixel(*r.snapshot,6,0,{127,127,127,255});pixel(*r.snapshot,7,0,{0,0,0,0});
    pixel(*r.snapshot,0,4,{0,255,0,255});pixel(*r.snapshot,4,4,{0,0,255,255});
    check(r.snapshot->cmpr_model()==TextureCmprModel::s3tc_candidate_v1,"candidate model provenance");
    auto native=t.capture(0,{},TextureCmprModel::gx_candidate_v1);ok(native);
    pixel(*native.snapshot,2,0,{159,0,95,255});pixel(*native.snapshot,3,0,{95,0,159,255});
    pixel(*native.snapshot,6,0,{127,127,127,255});pixel(*native.snapshot,7,0,{127,127,127,0});
    check(native.snapshot->cmpr_model()==TextureCmprModel::gx_candidate_v1,"GX arithmetic model is explicit");
    pixel(*r.snapshot,7,0,{0,0,0,0});

    t.setup(14,1,1);r=t.capture(0,{},TextureCmprModel::s3tc_candidate_v1);ok(r);
    check(r.snapshot->levels()[0].rgba.size()==4&&r.snapshot->source_bytes_read()==32,"CMPR minimum tile");
}
void failures_and_ownership() {
    Test t;t.setup(1,1,1);t.memory.image.assign(32,0x12);auto before=t.capture();ok(before);
    t.memory.image.assign(32,0x34);auto after=t.capture();ok(after);
    pixel(*before.snapshot,0,0,{18,18,18,18});pixel(*after.snapshot,0,0,{52,52,52,52});
    t.memory.fail=true;error(t.capture(),TextureError::memory_read);pixel(*before.snapshot,0,0,{18,18,18,18});t.memory.fail=false;
    t.memory.throws=true;bool threw=false;try{t.capture();}catch(const std::runtime_error&){threw=true;}check(threw,"provider exception propagates");t.memory.throws=false;
    error(t.capture(8),TextureError::invalid_map);
    error(t.capture(0,{},static_cast<TextureCmprModel>(99)),TextureError::unsupported_cmpr_model);
    TextureLimits l;l.max_dimension=0;error(t.capture(0,l),TextureError::dimension_limit);
    l={};l.max_levels=0;error(t.capture(0,l),TextureError::level_limit);
    l={};l.max_source_bytes=31;const auto calls=t.memory.calls;error(t.capture(0,l),TextureError::source_limit);check(t.memory.calls==calls,"source budget before read");
    l={};l.max_storage_bytes=0;error(t.capture(0,l),TextureError::storage_limit);check(t.memory.calls==calls,"storage budget before read");
    for(unsigned f:{7u,11u,12u,13u,15u}){t.setup(f,1,1);error(t.capture(),TextureError::unsupported_format);}
    for(unsigned m:{3u,12u,3u<<5,7u<<5,3u<<19,1u<<17,1u<<18,1u<<22,1u<<23,(1u<<19)|256u}){t.setup(1,1,1,0,m);error(t.capture(),TextureError::unsupported_state);}
    for(unsigned m:{161u<<8,32u|(16u<<8),1u<<16}){t.setup(1,1,1,0,0,m);error(t.capture(),TextureError::unsupported_state);}
    t.setup(1,7,1,0,1u<<5);error(t.capture(),TextureError::unsupported_state);
    t.setup(1,1,1);t.bp(0x8c,1u<<21);error(t.capture(),TextureError::unsupported_state);
    t.setup(8,1,1);error(t.capture(),TextureError::palette_read);t.bp(0x98,3u<<10);error(t.capture(),TextureError::unsupported_state);
    t.bp(0x98,0);t.memory.tlut.assign(32,0);t.palette(0,0,1);auto r=t.capture();ok(r);
    l={};l.max_storage_bytes=r.snapshot->peak_storage_bytes()-1;error(t.capture(0,l),TextureError::storage_limit);
    t.setup(1,1024,1024);t.memory.image.assign(1024*1024,0x91);r=t.capture();ok(r);
    pixel(*r.snapshot,1023,1023,{145,145,145,145});check(r.snapshot->levels()[0].rgba.size()==4*1024*1024,"maximum dimensions");
}
}
// A host extent widens a map past the register's 1024 texels while the register holds its low bits.
void host_extents() {
    Test t;
    t.setup(1,128,128);
    t.decoder.set_texture_extent(0,{128,1152});
    TextureDescriptor d;
    ok(describe_texture(t.decoder.state(),0,TextureCmprModel::reject_unverified,{},d));
    check(d.width==128&&d.height==1152,"extent replaces the register's truncated height");
    t.memory.image.assign(128*1152,0x37);
    auto r=t.capture();ok(r);
    check(r.snapshot->levels()[0].height==1152,"extended texture captured whole");
    t.setup(1,64,128);
    ok(describe_texture(t.decoder.state(),0,TextureCmprModel::reject_unverified,{},d));
    check(d.width==64&&d.height==128,"a register naming another size ignores the extent");
    t.decoder.set_texture_extent(0,{});
    t.setup(1,128,128);
    ok(describe_texture(t.decoder.state(),0,TextureCmprModel::reject_unverified,{},d));
    check(d.height==128,"a cleared extent leaves the register size");
    const auto revision=t.decoder.state().bp_revision();
    t.decoder.set_texture_extent(3,{2048,16});
    check(t.decoder.state().bp_revision()==revision+1,"an extent change counts as a BP change");
    bool rejected=false;
    try { t.decoder.set_texture_extent(0,{9000,16}); } catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"extents stop at 8192");
}
int main() {
    static_assert(std::is_same_v<decltype(TextureResult{}.snapshot),std::shared_ptr<const TextureSnapshot>>);
    try { content_cache(false);content_cache(true);cache_across_maps_and_eviction();direct_formats();tiling_and_padding();palettes();mipmaps_and_samplers();sampler_boundaries();compression();failures_and_ownership();host_extents(); }
    catch(const std::exception& e){std::cerr<<"GX texture test failed: "<<e.what()<<'\n';return 1;}
    std::cout<<"GX texture formats, tiling, mipmaps, palettes, sampler, ownership and limits passed\n";
    return 0;
}
