#include "starlet/gx/gles/renderer.hpp"
#include "draw_fixture.hpp"
#include "jfw_clear_fixture.hpp"
#include <GLES3/gl31.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <memory>
#include <future>
#include <functional>
#include <deque>
#include <condition_variable>
#include <thread>
#include <map>

namespace fake {
extern bool track_uniforms;
extern unsigned uniform_updates, negative_uniform_updates, transform_uploads;
extern starlet::gx::TransformUniforms uploaded_transform;
extern std::vector<unsigned char> submitted_vertices, split_vertices;
extern std::vector<std::uint32_t> split_indices;
extern unsigned split_draws, static_buffer_uploads;
extern std::vector<std::uint16_t> submitted_indices;
extern unsigned submitted_index_count;
extern GLuint current_program;
extern std::map<std::pair<GLuint,std::string>,GLint> uniform_locations;
extern std::map<GLint,std::array<double,4>> uniform_values;
extern GLenum error, wait_result;
extern GLboolean links;
extern bool map_pattern;
extern int read_pattern_row;
extern unsigned blits;
extern unsigned clears, uploads, draws, draw_first, sampler_seen, pack_seen, unpack_seen, pack_pbo_seen, unpack_pbo_seen;
extern std::size_t pack_required, unpack_required;
extern std::map<GLuint, GLuint> color_attachments;
extern std::map<GLuint, GLuint> depth_attachments;
extern unsigned uniform_ranges, read_pixels, draws_at_read;
extern std::thread::id read_thread;
extern unsigned dispatches;
extern const char* renderer_name;
extern unsigned instanced_draws, instance_count;
extern GLintptr instance_offset;
extern GLsizeiptr instance_size;
extern std::vector<unsigned char> uniform_bytes;
extern std::map<GLuint, std::pair<GLintptr, GLsizeiptr>> uniform_bindings;
extern std::array<GLuint, 4> matrix_latches;
extern GLint uniform_alignment;
extern GLsizeiptr element_bytes;
extern std::vector<std::string> shader_sources;
extern std::vector<GLenum> invalidated;
extern GLenum depth_func;
extern GLboolean depth_mask, depth_test;
extern unsigned texture_copies;
extern GLuint copy_source, copy_texture, array_framebuffer, draw_texture;
extern std::array<GLint, 4> copy_rect;
} // namespace fake
using namespace starlet::gx;
using namespace starlet::gx::test;
namespace {
template <class Function> void rejects(Function action, const char *message) {
    bool threw = false;
    try {
        action();
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, message);
}
void clip_disabled_draw_behind_eye() {
    Fixture f;auto captured=f.capture();const auto base=packet(captured);
    auto transform=base.transform->pipeline;transform.clipping_disabled=true;transform.orthographic=false;
    auto uniforms=base.transform->uniforms;uniforms.position[2][3]=10;
    auto behind=base;
    behind.transform=std::make_shared<TransformSnapshot>(TransformSnapshot{transform,uniforms,base.transform->state_revision});
    GlesRenderer renderer;const auto before=fake::draws;
    renderer.draw(behind);
    check(fake::draws==before && !renderer.failed(),"a clip-disabled draw behind the eye submits nothing and latches no error");
    renderer.draw(base);
    check(fake::draws==before+1 && !renderer.failed(),"drawing continues after a draw behind the eye");
    uniforms.position[2]={0,.1f,1,-15};
    auto crossing=base;
    crossing.transform=std::make_shared<TransformSnapshot>(TransformSnapshot{transform,uniforms,base.transform->state_revision});
    renderer.draw(crossing);
    check(fake::draws==before+2 && !renderer.failed(),"a clip-disabled draw crossing the eye plane is submitted for GLES to clip");
}
void adjacent_draw_batches() {
    Fixture f;auto captured=f.capture();const auto first=packet(captured);
    std::array<DrawPacket,3> draws{{first,first,first}};
    GlesRenderer renderer;const auto before=fake::draws;
    renderer.draw_batch(draws.data(),2);
    check(renderer.stats().draws==2 && renderer.stats().submissions==1 && fake::draws==before+1,"compatible adjacent triangles share one submission");
    const std::vector<std::uint16_t> expected{0,1,2,0,2,3,4,5,6,4,6,7};
    check(fake::submitted_indices==expected && fake::submitted_index_count==12,"batched indices preserve primitive order and vertex offsets");
    const auto& vertices=fake::submitted_vertices;
    check(vertices.size()==8*132 && std::equal(vertices.begin(),vertices.begin()+4*132,vertices.begin()+4*132),"original vertex bytes are duplicated exactly, without transform approximation");
    auto uniforms=first.tev->uniforms;uniforms.alpha_reference[0]=47;
    draws[1].tev=std::make_shared<const TevMaterial>(TevMaterial{first.tev->pipeline,uniforms,first.tev->state_revision});
    renderer.draw_batch(draws.data(),3);
    check(renderer.stats().draws==5 && renderer.stats().submissions==4,"uniform change and restoration split groups without sorting");
    std::fill(f.memory.bytes.begin(),f.memory.bytes.end(),0x42);auto changed=f.capture();
    draws={first,packet(changed),first};renderer.draw_batch(draws.data(),3);
    check(renderer.stats().submissions==7,"changed image and restored owned snapshot cannot merge");
    f.bp(0x83,1);auto sampler=f.capture();draws={packet(changed),packet(sampler),packet(changed)};
    renderer.draw_batch(draws.data(),3);check(renderer.stats().submissions==10,"sampler changes preserve sampling boundaries");
    auto transform=first.transform->uniforms;transform.position[0][3]+=1;
    auto moved=first;moved.transform=std::make_shared<const TransformSnapshot>(TransformSnapshot{first.transform->pipeline,transform,first.transform->state_revision});
    draws={first,moved,first};renderer.draw_batch(draws.data(),3);
    check(renderer.stats().submissions==13,"matrix changes cannot merge");
    Fixture scissor;scissor.bp(0x20,(492u<<12)|467u);scissor.bp(0x21,(591u<<12)|516u);auto clipped=scissor.capture();
    draws={first,packet(clipped),first};renderer.draw_batch(draws.data(),3);
    check(renderer.stats().submissions==16,"raster rectangles preserve boundaries");
    Fixture equal;equal.bp(0xfe,0xffffff);auto same=equal.capture();
    draws={first,packet(same),first};renderer.draw_batch(draws.data(),3);
    check(renderer.stats().submissions==17,"equal owned state can merge across distinct decoder revisions");
    renderer.draw_batch(nullptr,0);
    const auto submitted=fake::draws;
    rejects([&]{renderer.draw_batch(nullptr,1);},"null nonempty batch rejects");
    rejects([&]{renderer.draw_batch(draws.data(),4097);},"batch bound checked before array access");
    draws[1]={};rejects([&]{renderer.draw_batch(draws.data(),2);},"invalid later packet rejects entire batch before driver mutation");
    check(fake::draws==submitted,"failed batch validation submits no partial geometry");
    renderer.draw(first);
    check(renderer.stats().submissions==18,"validation rejection preserves renderer usability");
    std::thread foreign([&]{rejects([&]{renderer.draw_batch(draws.data(),1);},"batch obeys owner thread");});foreign.join();
}
void batch_geometry_bound() {
    Fixture f;const auto original=f.vertices;f.vertices.clear();f.vertices.reserve(40000*32);
    for(unsigned i=0;i<10000;++i)f.vertices.insert(f.vertices.end(),original.begin(),original.end());
    auto command=f.command();command.vertex_count=40000;command.vertices={f.vertices.data(),f.vertices.size()};
    const auto captured=capture_draw(f.decoder.state(),command,f.memory);const auto first=packet(captured);
    const std::array<DrawPacket,2> draws{{first,first}};
    GlesRenderer renderer;renderer.draw_batch(draws.data(),draws.size());
    check(renderer.stats().draws==2&&renderer.stats().submissions==2,"16-bit vertex bound splits compatible groups without wrapping indices");
    check(fake::submitted_vertices.size()==40000*132 && *std::max_element(fake::submitted_indices.begin(),fake::submitted_indices.end())==39999,"large group retains its full vertex/index range");
    fake::error=GL_INVALID_OPERATION;
    rejects([&]{renderer.draw_batch(draws.data(),1);},"driver error during batch propagates");
    rejects([&]{renderer.draw_batch(nullptr,0);},"failed batch latches context");
}
void parallel_batch_preparation() {
    Fixture fixture; auto captured = fixture.capture(); const auto first = packet(captured);
    std::vector<DrawPacket> draws(256, first);
    GlesRenderer renderer;
    for (unsigned repeat = 0; repeat < 4; ++repeat) renderer.draw_batch(draws.data(), draws.size());
    check(renderer.stats().draws == 1024 && renderer.stats().submissions == 4,
          "parallel preparation keeps all packets ordered and merges the same compatible geometry");
    const auto submitted = fake::draws;
    for (unsigned index : {0u, 127u, 255u}) {
        draws[index] = {};
        rejects([&] { renderer.draw_batch(draws.data(), draws.size()); },
                "invalid packet at any position rejects parallel preparation");
        check(fake::draws == submitted, "parallel failure submits no partial geometry");
        draws[index] = first;
    }
    renderer.draw_batch(draws.data(), draws.size());
    check(renderer.stats().submissions == 5, "preparation workers remain reusable after validation failures");
}
void program_uniform_values() {
    fake::track_uniforms=true;
    fake::uniform_updates=0;fake::negative_uniform_updates=0;
    Fixture fixture;auto captured=fixture.capture();
    auto first=packet(captured);
    GlesRenderer renderer;
    renderer.draw(first);
    const auto initial=fake::uniform_updates;
    check(initial>0,"a new program uploads its initial uniform values");
    const auto original_program=fake::current_program;
    const auto transform_uploads=fake::transform_uploads;
    renderer.draw(first);
    check(fake::uniform_updates==initial,"unchanged draws retain the same driver uniform state");
    check(fake::transform_uploads==transform_uploads,"unchanged transforms retain the same buffer storage");
    auto transform=first.transform->uniforms;transform.position[0][3]+=1;
    auto moved=first;moved.transform=std::make_shared<const TransformSnapshot>(TransformSnapshot{first.transform->pipeline,transform,first.transform->state_revision});
    renderer.draw(moved);
    check(fake::transform_uploads==transform_uploads+1 && fake::uploaded_transform.position[0][3]==transform.position[0][3],"changed transform uploads its new buffer bytes");
    renderer.draw(first);
    check(fake::transform_uploads==transform_uploads+2 && fake::uploaded_transform.position==first.transform->uniforms.position,"restoring a transform restores the buffer contents");
    auto uniforms=first.tev->uniforms;uniforms.registers[1]={17,31,63,127};
    auto changed=first;changed.tev=std::make_shared<const TevMaterial>(TevMaterial{first.tev->pipeline,uniforms,first.tev->state_revision});
    renderer.draw(changed);
    auto location=fake::uniform_locations.at({original_program,"uGxTevRegisters[1]"});
    check(fake::uniform_updates==initial+1 && fake::uniform_values.at(location)==std::array<double,4>{17,31,63,127},"changing one register updates exactly that register in the driver");
    renderer.draw(first);
    check(fake::uniform_values.at(location)==std::array<double,4>{0,0,0,0},"restoring a prior packet restores its register values");
    auto other_pipeline=first.tev->pipeline;other_pipeline.alpha_compare[0]=TevCompare::less;
    auto other=first;other.tev=std::make_shared<const TevMaterial>(TevMaterial{other_pipeline,uniforms,first.tev->state_revision});
    renderer.draw(other);
    const auto other_program=fake::current_program, before=fake::uniform_updates;
    check(other_program!=original_program,"different shader keys use distinct program state");
    const auto retained_transform_uploads=fake::transform_uploads;
    renderer.draw(first);
    check(fake::transform_uploads==retained_transform_uploads,"program switches share the existing matching transform buffer");
    check(fake::current_program==original_program && fake::uniform_updates==before,"switching programs retains each program's own values");
    GlesLimits limits;limits.programs=1;GlesRenderer evicting(limits);
    evicting.draw(first);evicting.draw(other);const auto evicted=fake::current_program;
    const auto previous_updates=fake::uniform_updates;
    evicting.draw(first);
    check(fake::current_program!=evicted && fake::uniform_updates>previous_updates,"a recreated program uploads fresh values after eviction");
    check(fake::negative_uniform_updates==0,"inactive uniform locations issue no driver calls");
    fake::track_uniforms=false;
}
void program_working_set() {
    Fixture fixture;auto captured=fixture.capture();const auto first=packet(captured);
    std::vector<DrawPacket> packets;
    for(unsigned i=0;i<40;++i){
        auto pipeline=first.tev->pipeline;
        pipeline.alpha_compare={TevCompare(i%8),TevCompare(i/8)};
        auto next=first;
        next.tev=std::make_shared<const TevMaterial>(TevMaterial{pipeline,first.tev->uniforms,first.tev->state_revision});
        packets.push_back(std::move(next));
    }
    GlesLimits small;small.programs=32;GlesRenderer evicting(small),retained;
    for(unsigned pass=0;pass<3;++pass)for(const auto& p:packets){evicting.draw(p);retained.draw(p);}
    check(evicting.stats().shader_compiles==120,"a working set larger than the small cache recompiles each pass");
    check(retained.stats().shader_compiles==40,"default cache retains a multi-material working set across passes");
}
void failed_upload_poison() {
    GlesRenderer renderer;
    Fixture fixture;
    auto original = fixture.capture();
    renderer.draw(packet(original));
    std::fill(fixture.memory.bytes.begin(), fixture.memory.bytes.end(), 0x42);
    auto changed = fixture.capture();
    fake::error = GL_INVALID_ENUM;
    rejects([&] { renderer.draw(packet(changed)); }, "draw must report GL error");
    const auto uploads = fake::uploads, draws = fake::draws;
    rejects([&] { renderer.draw(packet(original)); }, "a failed renderer cannot trust its old image cache");
    rejects([&] { renderer.clear({{0, 0, 0}}); }, "clear after GL failure");
    rejects([&] { renderer.present(640, 480); }, "present after GL failure");
    rejects([&] { renderer.readback(); }, "readback after GL failure");
    check(fake::uploads == uploads && fake::draws == draws, "poisoned renderer issues no further draw/upload");
    const auto reads=fixture.memory.reads; const auto command=fixture.command();
    rejects([&]{renderer.draw(fixture.decoder.state(),command,fixture.memory);},"raw capture after GL failure rejects before source access");
    check(fixture.memory.reads==reads,"poisoned renderer does not record new packets");
    check(renderer.stats().draws == 1, "statistics remain readable after failure");
}
void inherited_transfer_state() {
    GlesRenderer renderer;
    Fixture fixture;
    auto captured = fixture.capture();
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 19);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    GLuint buffers[2]{}, sampler = 0;
    glGenBuffers(2, buffers);
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffers[0]);
    glBindSampler(3, sampler);
    renderer.draw(packet(captured));
    check(fake::unpack_seen == 0 && fake::unpack_pbo_seen == 0 && fake::sampler_seen == 0,
          "external row/PBO/sampler state cannot redirect texture upload or sampling");
    check(fake::unpack_required == captured.packet->textures[3]->levels()[0].rgba.size(), "upload fits owned bytes");
    glPixelStorei(GL_PACK_ROW_LENGTH, 700);
    glPixelStorei(GL_PACK_SKIP_ROWS, 1);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffers[1]);
    const auto pixels = renderer.readback();
    check(fake::pack_seen == 0 && fake::pack_pbo_seen == 0 && fake::pack_required == pixels.size(),
          "readback fits owned allocation regardless of inherited pixel-store state");
    glDeleteBuffers(2, buffers);
    glDeleteSamplers(1, &sampler);
}
void fence_failure_and_thread_owner() {
    GlesRenderer renderer;
    renderer.present(640, 480);
    fake::wait_result = GL_WAIT_FAILED;
    rejects([&] { renderer.present(640, 480); }, "failed retirement is not successful presentation");
    rejects([&] { renderer.present(640, 480); }, "failed retirement cannot accumulate more fences");
    fake::wait_result = GL_ALREADY_SIGNALED;
    GlesRenderer other;
    Fixture fixture;
    auto captured = fixture.capture();
    std::thread foreign(
        [&] { rejects([&] { other.draw(packet(captured)); }, "foreign thread cannot use GL context"); });
    foreign.join();
    other.draw(packet(captured));
    check(other.stats().draws == 1, "owner remains usable after foreign-thread rejection");
}
void captured_texture_cache() {
    GlesRenderer renderer; Fixture f; const auto command=f.command();
    renderer.draw(f.decoder.state(),command,f.memory);
    auto stats=renderer.stats();
    check(stats.draws==1&&stats.capture_cache.misses==1&&stats.texture_uploads==1&&fake::draw_first==0xa5,"first raw draw captures and uploads original pixels");
    renderer.draw(f.decoder.state(),command,f.memory);
    stats=renderer.stats();
    check(stats.draws==2&&stats.capture_cache.hits==1&&stats.texture_uploads==1&&fake::draw_first==0xa5,"raw draws reuse exact decoded content and GPU texture");
    std::fill(f.memory.bytes.begin(),f.memory.bytes.end(),0x42);
    renderer.draw(f.decoder.state(),command,f.memory); stats=renderer.stats();
    check(stats.capture_cache.misses==2&&stats.texture_uploads==2&&fake::draw_first==0x42,"same-address source mutation decodes and uploads new pixels");
    f.bp(0x83,1);renderer.draw(f.decoder.state(),command,f.memory);stats=renderer.stats();
    check(stats.capture_cache.hits==2&&stats.texture_uploads==3,"decoded pixels share across distinct sampler bindings");
    f.bp(0x83,0);std::fill(f.memory.bytes.begin(),f.memory.bytes.end(),0xa5);
    renderer.draw(f.decoder.state(),command,f.memory);stats=renderer.stats();
    check(stats.capture_cache.hits==3&&stats.texture_uploads==3&&fake::draw_first==0xa5,"restored original content reuses its own immutable image");
    const auto reads=f.memory.reads, draws=fake::draws;
    std::thread foreign([&]{rejects([&]{renderer.draw(f.decoder.state(),command,f.memory);},"raw draw owner check");});foreign.join();
    check(f.memory.reads==reads&&fake::draws==draws,"foreign raw draws neither read source nor touch driver");
    f.memory.fail=true;rejects([&]{renderer.draw(f.decoder.state(),command,f.memory);},"cache hits cannot bypass current source bounds");
    check(fake::draws==draws,"failed capture issues no GL draw");
    f.memory.fail=false;renderer.draw(f.decoder.state(),command,f.memory);
    GlesLimits limits;limits.texture_bytes=128;limits.textures=1;GlesRenderer bounded(limits);
    bounded.draw(f.decoder.state(),command,f.memory);bounded.draw(f.decoder.state(),command,f.memory);
    check(bounded.stats().capture_cache.bytes<=128&&bounded.stats().capture_cache.entries<=1,"decoded cache obeys renderer budget even when a cache entry cannot fit");
}
void complete_mip_cache_budget() {
    Fixture f; f.memory.bytes.resize(96,0xa5); f.bp(0x83,0x1e1d5); f.bp(0x87,0x2000);
    const auto first=f.capture();
    GlesRenderer renderer;renderer.draw(packet(first));
    check(renderer.stats().texture_bytes==168,"all decoded mip levels count toward GPU budget");
    f.memory.bytes[32]=42;const auto second=f.capture();renderer.draw(packet(second));
    check(renderer.stats().texture_uploads==2 && renderer.stats().texture_bytes==336,
          "identical base level cannot alias different lower mip content");
    renderer.draw(packet(first));check(renderer.stats().texture_uploads==2,"original complete chain stays resident");
    GlesLimits limits;limits.texture_bytes=167;GlesRenderer bounded(limits);
    const auto uploads=fake::uploads;
    rejects([&]{bounded.draw(packet(first));},"entire active chain must fit before uploading");
    check(fake::uploads==uploads,"budget failure uploads no partial mip chain");
}
void resident_texture_cache() {
    GlesRenderer renderer;Fixture f;auto first=f.capture();renderer.draw(packet(first));
    std::fill(f.memory.bytes.begin(),f.memory.bytes.end(),0x42);auto second=f.capture();renderer.draw(packet(second));
    renderer.draw(packet(first));renderer.draw(packet(second));
    check(renderer.stats().texture_uploads==2,"alternating content stays resident");
    f.bp(0x83,1);auto repeat=f.capture();renderer.draw(packet(repeat));
    check(renderer.stats().texture_uploads==3,"sampler state is part of resident texture identity");
    renderer.draw(packet(second));check(renderer.stats().texture_uploads==3,"sampler variant does not overwrite earlier object");
    GlesLimits limits;limits.textures=1;limits.texture_bytes=128;
    GlesRenderer bounded(limits);bounded.draw(packet(first));bounded.draw(packet(second));bounded.draw(packet(first));
    check(bounded.stats().texture_uploads==3 && bounded.stats().texture_bytes==128,"resident eviction obeys entry and byte budgets");
}
void warm_shader_validation() {
    GlesRenderer renderer;
    Fixture f; f.xf(0x1041, (6u << 7) | 2);
    const auto first = f.capture(); renderer.draw(packet(first)); renderer.draw(packet(first));
    check(renderer.stats().shader_compiles == 1 && renderer.stats().draws == 2, "warm program cache reuses compiled shader");
    f.f(62u * 4 + 2, 0);
    const auto invalid = f.capture();
    rejects([&] { renderer.draw(packet(invalid)); }, "warm shader still validates zero projected Q in current uniforms");
    check(renderer.stats().draws == 2 && renderer.stats().shader_compiles == 1, "invalid warm draw never reaches driver");
    f.f(62u * 4 + 2, 1);
    const auto restored = f.capture(); renderer.draw(packet(restored));
    check(renderer.stats().draws == 3 && renderer.stats().shader_compiles == 1, "valid uniform change retains program after rejected draw");
}
void rgb8_replacement_boundary() {
    GlesRenderer renderer; EfbClear clear;clear.rectangle={0,0,640,456};clear.rgba6=true;
    clear.color_write=true;clear.alpha_write=true;renderer.clear_efb(clear);
    Fixture f;auto partial=f.capture();
    renderer.draw(packet(partial));
    f.full_opaque_quad();auto full=f.capture();renderer.draw(packet(full));
    renderer.draw(packet(partial));
    check(renderer.stats().draws==3, "partial and full draws can switch EFB format");
}
void clear_boundary() {
    GlesRenderer renderer; EfbClear clear; clear.rectangle={0,0,640,456};
    const auto count=fake::clears;renderer.clear_efb(clear);
    check(fake::clears==count+1,"native clear reaches GPU even when only depth is written");
    clear.rectangle={639,455,2,1};rejects([&]{renderer.clear_efb(clear);},"invalid clear extent rejected");
    clear.rectangle={0,0,640,456};renderer.clear_efb(clear);
    clear.rgba6=true;clear.alpha_write=true;clear.color_write=true;renderer.clear_efb(clear);
    clear.rectangle={1,1,2,2};clear.rgba6=false;
    renderer.clear_efb(clear);
    clear.rectangle={0,0,640,456};renderer.clear_efb(clear);
    fake::error=GL_INVALID_OPERATION;
    rejects([&]{renderer.clear_efb(clear);},"clear propagates driver failure");
    rejects([&]{renderer.present(640,480);},"failed clear poisons renderer");
}
// A renderer serves one EFB layout; work planned for another frame height is refused.
void efb_layout_boundary() {
    GlesLimits limits; limits.efb = EfbLayout{480};
    GlesRenderer renderer(limits); EfbClear clear; clear.efb = EfbLayout{480}; clear.rectangle={0,0,640,480};
    const auto count=fake::clears;renderer.clear_efb(clear);
    check(fake::clears==count+1,"a 640x480 frame clears whole");
    clear.rectangle={0,0,640,481};rejects([&]{renderer.clear_efb(clear);},"clears stay inside the 480-row frame");
    clear.efb = EfbLayout{}; clear.rectangle={0,0,640,456};
    rejects([&]{renderer.clear_efb(clear);},"a clear planned for a 456-row frame is refused");
}
void display_copy_boundary() {
    GlesRenderer renderer;DisplayCopy copy;copy.width=16;copy.height=16;copy.stride=64;
    copy.filter={{0,64,0}};copy.clamp_top=true;copy.clamp_bottom=true;
    alignas(32)std::array<std::uint8_t,1024> bytes{};
    glPixelStorei(GL_PACK_SKIP_ROWS,5);glPixelStorei(GL_PACK_SKIP_PIXELS,9);glBindBuffer(GL_PIXEL_PACK_BUFFER,123);
    renderer.copy_display(copy,bytes.data(),bytes.size());
    check(fake::pack_seen==0&&fake::pack_pbo_seen==0&&fake::pack_required==15*64+32,"XFB GPU readback obeys explicit row stride and bounds");
    auto calls=fake::draws;rejects([&]{renderer.copy_display(copy,bytes.data(),16);},"short XFB target rejected");
    check(fake::draws==calls,"short copy target fails before GL draw");
    FramebufferView frame{{bytes.data(),bytes.size()},16,16,64};
    glPixelStorei(GL_UNPACK_SKIP_ROWS,7);glPixelStorei(GL_UNPACK_SKIP_PIXELS,3);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,456);glBindSampler(0,789);
    renderer.present_framebuffer(frame,640,480);
    check(fake::unpack_seen==0&&fake::unpack_pbo_seen==0&&fake::unpack_required==15*64+32,"VI upload honors actual XFB stride and resets inherited transfers");
    renderer.present_framebuffer({},640,480,true);
    check(renderer.stats().display_copies==1&&renderer.stats().presentations==2,"copy and VI counters separate");
    fake::error=GL_INVALID_OPERATION;rejects([&]{renderer.copy_display(copy,bytes.data(),bytes.size());},"copy driver error propagates");
    rejects([&]{renderer.present_framebuffer(frame,640,480);},"failed copy cannot present");
}
void repeated_presentation() {
    GlesRenderer renderer; Fixture f;
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    DisplayCopy copy;copy.width=16;copy.height=16;copy.stride=64;
    copy.filter={{0,64,0}};copy.clamp_top=true;copy.clamp_bottom=true;
    alignas(32)std::array<std::uint8_t,1024> bytes{};
    FramebufferView frame{{bytes.data(),bytes.size()},16,16,64};
    renderer.copy_display(copy,bytes.data(),bytes.size());
    check(renderer.present_framebuffer(frame,640,480) && !renderer.present_framebuffer(frame,640,480) &&
          renderer.stats().presentations==1,"direct path does not present an unchanged XFB again");
    check(renderer.present_framebuffer(frame,320,240),"a new drawable extent presents");
    alignas(32)std::array<std::uint8_t,1024> next{};
    renderer.copy_display(copy,next.data(),next.size());
    check(!renderer.present_framebuffer(frame,320,240),"a copy into another XFB leaves the shown one unchanged");
    renderer.copy_display(copy,bytes.data(),bytes.size());
    check(renderer.present_framebuffer(frame,320,240),"a display copy into the shown XFB presents again");
    check(renderer.present_framebuffer({},320,240,true) && !renderer.present_framebuffer({},320,240,true) &&
          renderer.present_framebuffer(frame,320,240),"black state is part of the presented image");
    GlesRenderer packet;
    packet.copy_display(copy,bytes.data(),bytes.size());
    check(packet.present_framebuffer(frame,640,480) && packet.present_framebuffer(frame,640,480),
          "without the direct path every retrace presents");
}
void depth_query_transfer_bounds() {
    GlesRenderer renderer;
    auto calls=fake::draws;
    rejects([&]{renderer.peek_depth(-1,0);},"negative depth query rejected");
    rejects([&]{renderer.peek_depth(640,455);},"wide depth query rejected");
    rejects([&]{renderer.peek_depth(639,456);},"tall depth query rejected");
    check(fake::draws==calls,"invalid depth query performs no GL draw");
    glPixelStorei(GL_PACK_SKIP_ROWS,5);glPixelStorei(GL_PACK_SKIP_PIXELS,9);glBindBuffer(GL_PIXEL_PACK_BUFFER,123);
    renderer.peek_depth(639,455);
    check(fake::pack_seen==0&&fake::pack_pbo_seen==0&&fake::pack_required==640u*456u*4u,"depth read resets pack state for the EFB's RGBA depth");
    GlesRenderer failing;
    fake::error=GL_INVALID_OPERATION;rejects([&]{failing.peek_depth(0,0);},"driver depth failure reported");
    rejects([&]{failing.peek_depth(0,0);},"failed depth renderer remains poisoned");
}
// Live depth peeks with no EFB write between them share one readback of the whole EFB's depth;
// an EFB write in between makes the next peek read it again.
void live_depth_peeks_share_a_readback() {
    GlesRenderer renderer; Fixture f; const auto command=f.command();
    renderer.draw_direct(f.decoder.state(),command,f.memory);
    auto reads=fake::read_pixels;
    renderer.peek_depth(10,10); renderer.peek_depth(20,30); renderer.peek_depth(600,400);
    check(fake::read_pixels==reads+1 && fake::pack_required==640u*456u*4u && renderer.stats().live_peeks==3,
          "three peeks with no EFB write between them read the EFB's depth once");
    reads=fake::read_pixels;
    renderer.draw_direct(f.decoder.state(),command,f.memory);
    renderer.peek_depth(10,10); renderer.peek_depth(11,10);
    check(fake::read_pixels==reads+1,"an EFB write between peeks makes the next one read again");
    reads=fake::read_pixels;
    EfbClear clear; clear.rectangle={0,0,640,456}; clear.color_write=true; clear.depth_write=true;
    renderer.clear_efb(clear);
    renderer.peek_depth(10,10);
    check(fake::read_pixels==reads+1,"an EFB clear counts as a write");
}
// An asynchronous depth readback: requests with no EFB write between them share one readback of
// the whole EFB, values arrive after the wait, a write in between takes another, and a released
// handle's slot is reused.
void asynchronous_depth_readback() {
    GlesRenderer renderer; Fixture f; const auto command=f.command();
    renderer.draw_direct(f.decoder.state(),command,f.memory);
    auto reads=fake::read_pixels;
    const int a=renderer.request_depth_readback(), b=renderer.request_depth_readback();
    std::uint32_t value=0;
    check(a>=0 && a==b && fake::read_pixels==reads+1 && fake::pack_required==640u*456u*4u,
          "requests with no EFB write between them share one whole-EFB readback");
    renderer.wait_depth_readback(a);
    check(renderer.depth_readback_value(a,10,10,value) && !renderer.depth_readback_value(a,640,0,value),
          "an arrived readback answers inside the EFB only");
    renderer.draw_direct(f.decoder.state(),command,f.memory);
    const int c=renderer.request_depth_readback();
    check(c>=0 && c!=a && fake::read_pixels==reads+2,"an EFB write between requests takes another readback");
    check(!renderer.depth_readback_value(c,10,10,value),"a readback not waited for answers nothing yet");
    renderer.release_depth_readback(a); renderer.release_depth_readback(b); renderer.release_depth_readback(c);
    renderer.draw_direct(f.decoder.state(),command,f.memory);
    check(renderer.request_depth_readback()==a,"a released readback's slot is reused");
}
void validation_precedes_mutation() {
    GlesRenderer renderer;
    DrawPacket incomplete;
    rejects([&] { renderer.draw(incomplete); }, "incomplete packet rejected");
    Fixture fixture;
    auto captured = fixture.capture();
    renderer.draw(packet(captured));
    check(renderer.stats().draws == 1, "pure validation failure does not corrupt renderer state");
    rejects([&] { renderer.present(0, 480); }, "invalid drawable rejected");
    renderer.present(640, 480);
}
} // namespace
void explicit_completion() {
    GlesRenderer renderer;
    renderer.wait_until_idle();
    fake::wait_result = GL_CONDITION_SATISFIED;
    renderer.wait_until_idle();
    fake::wait_result = GL_TIMEOUT_EXPIRED;
    rejects([&] { renderer.wait_until_idle(); }, "timed out GPU completion rejects");
    rejects([&] { renderer.clear({{0,0,0}}); }, "failed completion latches renderer failure");
    fake::wait_result = GL_ALREADY_SIGNALED;
}
void program_reuse_across_tev_colours() {
    GlesLimits limits; limits.direct.shading=DirectShading::fast;
    GlesRenderer renderer(limits); Fixture f; const auto command=f.command();
    const auto misses=[&]{renderer.draw_direct(f.decoder.state(),command,f.memory);renderer.wait_until_idle();return renderer.stats().direct.program_misses;};
    const auto first=misses();
    f.bp(0xe2,0x010020);
    check(misses()==first,"in-range TEV colour writes keep the program");
    f.bp(0xe2,0x0107ff);
    const auto wide=misses();
    check(wide>first,"an out-of-range TEV register selects the wrapping program");
    f.bp(0xe2,0x010020);
    check(misses()==wide,"back in range reuses the first program");
}
// Unready programs: a draw waits for the builder only up to unready_link_ms, and request
// workers take the oldest or the newest request.
void unready_program_policy() {
    using std::chrono::milliseconds;
    check(!link_unready_program(false, milliseconds(1000), 50), "a program is requested before the GL thread links it");
    check(!link_unready_program(true, milliseconds(49), 50), "draws wait while the builder has time left");
    check(link_unready_program(true, milliseconds(50), 50), "after unready_link_ms the GL thread links the program");
    check(!link_unready_program(true, milliseconds(100000), 0), "unready_link_ms 0 always waits for the builder");
    check(!link_unready_program(true, milliseconds(80), 50, milliseconds(100), 250) &&
          link_unready_program(true, milliseconds(80), 50, milliseconds(250), 250),
          "the GL thread links again only unready_link_interval_ms after its last link");
    std::deque<int> queue{1, 2, 3};
    check(take_request(queue, false) == 1 && take_request(queue, true) == 3 && queue.size() == 1 && queue.front() == 2,
          "requests are taken oldest or newest first");
    GlesRenderer renderer; Fixture f;
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
    const auto stats = renderer.stats().direct;
    check(stats.fallback_links == 0 && stats.program_waits[0] + stats.program_waits[4] == 0,
          "programs linked without a builder never waited");
}
void rejected_program_skips_draws() {
    GlesRenderer renderer; Fixture f; const auto command=f.command();
    fake::links=GL_FALSE;
    const auto before=fake::draws;
    renderer.draw_direct(f.decoder.state(),command,f.memory); renderer.wait_until_idle();
    check(fake::draws==before && renderer.stats().direct.unready_draws==1,"a program the driver rejects skips its draws");
    fake::links=GL_TRUE;
    renderer.draw_direct(f.decoder.state(),command,f.memory); renderer.wait_until_idle();
    check(fake::draws==before && renderer.stats().direct.unready_draws==2,"the rejected program is not linked again");
    f.bp(0xc1,0x08fff0);
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    check(fake::draws==before+1,"other programs still draw after a rejection");
}
// A reader that names texture sources with a write generation (MemoryReader::source_identity).
struct GenerationMemory : test::Memory {
    std::uint64_t generation = 1;
    ByteView view(std::uint32_t address, std::size_t size) override {
        if (address != 0x8000 || size > bytes.size()) return {};
        return {bytes.data(), size};
    }
    SourceIdentity source_identity(const void* data, std::size_t) override { return {data, generation}; }
};
void texture_generations() {
    GlesRenderer renderer; Fixture f; const auto command = f.command();
    GenerationMemory memory; memory.bytes = f.memory.bytes;
    renderer.draw_direct(f.decoder.state(), command, memory); renderer.wait_until_idle();
    const auto validations = renderer.stats().direct.texture_validations;
    for (int frame = 0; frame < 32; ++frame) {
        renderer.present(640, 480);
        renderer.draw_direct(f.decoder.state(), command, memory); renderer.wait_until_idle();
    }
    auto s = renderer.stats().direct;
    check(s.texture_validations - validations == 2, "an unchanged source generation skips all but one in sixteen source hashes");
    const auto mutations = s.texture_mutations;
    std::fill(memory.bytes.begin(), memory.bytes.end(), 0x42); ++memory.generation;
    renderer.present(640, 480);
    renderer.draw_direct(f.decoder.state(), command, memory); renderer.wait_until_idle();
    s = renderer.stats().direct;
    check(s.texture_mutations == mutations + 1 && fake::draw_first == 0x42, "a new source generation hashes the source and sees the change");
}
void direct_draw_path() {
    GlesRenderer renderer; Fixture f; const auto command=f.command();
    const auto before=fake::draws;
    renderer.draw_direct(f.decoder.state(),command,f.memory);
    renderer.draw_direct(f.decoder.state(),command,f.memory);
    check(fake::draws==before,"direct draws defer GL submission");
    renderer.wait_until_idle();
    auto s=renderer.stats().direct;
    check(fake::draws==before+1 && fake::element_bytes==12*4 && fake::draw_first==0xa5,
          "identical adjacent direct draws merge into one submission of both quads");
    check(s.draws==2 && s.submissions==1 && s.segments==1 && s.texture_uploads==1 && s.program_misses==1 &&
          s.uniform_blocks==1 && s.state_plans==1,"direct path plans program, state, uniforms and texture once");
    std::fill(f.memory.bytes.begin(),f.memory.bytes.end(),0x42);
    renderer.draw_direct(f.decoder.state(),command,f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.texture_uploads==1 && s.texture_validations==0,"sources are validated once per frame");
    renderer.present(640,480);
    renderer.draw_direct(f.decoder.state(),command,f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.texture_uploads==2 && s.texture_mutations==1 && fake::draw_first==0x42,"next frame detects same-address source mutation");
    f.f(0,2.0f);
    renderer.draw_direct(f.decoder.state(),command,f.memory);
    f.f(0,1.0f);
    renderer.draw_direct(f.decoder.state(),command,f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.uniform_blocks==5 && s.submissions==5 && s.program_misses==1,"matrix changes are uniform blocks, not programs");
    f.bp(0xf3,0x3f0011);
    renderer.draw_direct(f.decoder.state(),command,f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.program_misses==1 && s.state_plans==5,"alpha reference is per-draw state");
    // Texture registers written again with the same values keep the plan and resolved textures.
    renderer.draw_direct(f.decoder.state(),command,f.memory);
    const auto lookups=renderer.stats().direct.texture_lookups, plans=renderer.stats().direct.state_plans;
    f.bp(0x97,f.decoder.state().bp(0x97)); renderer.draw_direct(f.decoder.state(),command,f.memory);
    f.bp(0x97,f.decoder.state().bp(0x97)); renderer.draw_direct(f.decoder.state(),command,f.memory);
    s=renderer.stats().direct;
    check(s.texture_lookups==lookups && s.state_plans==plans,"identical register writes reuse the plan and resolved textures");
    f.bp(0x88,f.decoder.state().bp(0x88)^1); renderer.draw_direct(f.decoder.state(),command,f.memory);
    s=renderer.stats().direct;
    check(s.texture_lookups==lookups+1 && s.state_plans==plans+1,"a changed register plans and resolves again");
    // BP 0xe0-0xe7 hold the last colour or konst write: a colour change followed by an unchanged
    // konst write leaves the BP value equal, as nw4r layout materials write them.
    f.bp(0xe2,0x0ff0ff); f.bp(0xe2,(1u<<23)|0x10); renderer.draw_direct(f.decoder.state(),command,f.memory);
    const auto tev_plans=renderer.stats().direct.state_plans;
    f.bp(0xe2,0x000000); f.bp(0xe2,(1u<<23)|0x10); renderer.draw_direct(f.decoder.state(),command,f.memory);
    check(renderer.stats().direct.state_plans==tev_plans+1,"a TEV colour change behind an equal konst write plans again");
    f.bp(0xe2,0x000000); f.bp(0xe2,(1u<<23)|0x20); f.bp(0xe2,(1u<<23)|0x10); renderer.draw_direct(f.decoder.state(),command,f.memory);
    check(renderer.stats().direct.state_plans==tev_plans+1,"equal TEV colour and konst registers keep the plan");
    renderer.wait_until_idle();
    f.bp(0x43,2);
    const auto draws=fake::draws;
    rejects([&]{renderer.draw_direct(f.decoder.state(),command,f.memory);},"unsupported pixel format rejected before submission");
    check(fake::draws==draws && !renderer.failed(),"rejected direct draw issues no GL draw and leaves the renderer usable");
    f.bp(0x43,0);
    GlesLimits limits; limits.direct.verify_textures=true; GlesRenderer verified(limits); Fixture g; const auto other=g.command();
    verified.draw_direct(g.decoder.state(),other,g.memory);
    std::fill(g.memory.bytes.begin(),g.memory.bytes.end(),0x17);
    verified.draw_direct(g.decoder.state(),other,g.memory); verified.wait_until_idle();
    check(verified.stats().direct.texture_mutations==1 && verified.stats().direct.submissions==2 && fake::draw_first==0x17,
          "verification hashes every use and splits submissions on new content");
}
// The RGB XFB presentation shader's mapping (renderer.cpp) against a nearest blit's, rows
// flipped: destination pixel centres mapped to source coordinates, floored, over the extents ports use.
void present_mapping() {
    for (const auto& [source, destination] : std::vector<std::pair<int, int>>{{448, 480}, {456, 480}, {480, 480}, {640, 640},
                                                                            {640, 720}, {456, 720}, {608, 640}, {448, 1080}}) {
        for (int p = 0; p < destination; ++p) {
            const int flipped = destination - 1 - p;
            const int shader = (flipped * 2 + 1) * source / (destination * 2);
            // Blit: destination rows from destination down to 0 take source rows 0 up to source.
            const double blit = (double(destination) - (p + 0.5)) * source / destination;
            check(shader == int(std::floor(blit)), "presentation samples the texel a nearest blit samples");
            // The shader's division-free quotient: a float estimate corrected by products.
            const int n = (flipped * 2 + 1) * source, d = destination * 2;
            int q = int(float(n) / float(d));
            q += int((q + 1) * d <= n) - int(q * d > n);
            check(q == shader, "presentation's corrected float quotient is the integer quotient");
        }
    }
}
void no_blits_on_the_frame_path() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits); Fixture f; f.memory.bytes.resize(64, 0xa5);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
    const auto blits = fake::blits, draws = fake::draws;
    TextureCopy p; p.width = 16; p.height = 8; p.format = 4; p.half_scale = true; p.stride = 64;
    alignas(32) std::array<std::uint8_t, 256> bytes{};
    renderer.copy_texture_resident(p, bytes.data(), bytes.size(), 0x8000);
    renderer.wait_until_idle();
    check(fake::blits == blits && fake::draws > draws, "half-scale copies box-filter in the copy shader, not a blit");
    DisplayCopy copy; copy.width = 16; copy.height = 16; copy.stride = 64; copy.filter = {{0, 64, 0}};
    alignas(32) std::array<std::uint8_t, 1024> xfb{};
    renderer.copy_display(copy, xfb.data(), xfb.size());
    renderer.present_framebuffer({{xfb.data(), xfb.size()}, 16, 16, 64}, 640, 480);
    check(fake::blits == blits && renderer.stats().presentations == 1, "resident RGB XFBs present with a draw");
    check(std::none_of(fake::shader_sources.begin(), fake::shader_sources.end(),
                       [](const std::string& s) { return s.find("/(uDestination") != std::string::npos; }),
          "presentation divides no integers per pixel");
    check(std::any_of(fake::shader_sources.begin(), fake::shader_sources.end(),
                      [](const std::string& s) { return s.find("texture(uImage,vSource)") != std::string::npos; }),
          "resident RGB XFBs present with one nearest lookup per pixel");
}
void resident_copies() {
    GlesRenderer renderer; Fixture f; f.memory.bytes.resize(64,0xa5);
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    TextureCopy p; p.width=8; p.height=4; p.format=1; p.luminance=true; p.stride=32;
    alignas(32) std::array<std::uint8_t,64> bytes{};
    const auto reads=fake::read_pixels;
    renderer.copy_texture_resident(p,bytes.data(),bytes.size(),0x8000);
    check(fake::read_pixels==reads && bytes==std::array<std::uint8_t,64>{},"resident copy neither reads back nor writes memory");
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    auto s=renderer.stats().direct;
    check(s.resident_copies==1 && s.resident_hits==1 && s.materialized==0,"matching I8 lookup samples the resident copy");
    f.bp(0x8b,0x400c07);
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.materialized==1 && fake::read_pixels==reads+1,"incompatible sample format materializes the copy once");
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    check(renderer.stats().direct.materialized==1,"materialized copy is ordinary memory afterwards");
    f.bp(0x8b,0x100c07);
    renderer.copy_texture_resident(p,bytes.data(),bytes.size(),0x8000);
    renderer.release_resident(0x8000,32);
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.resident_hits==1 && s.materialized==1,"released copies are neither sampled nor materialized");
    GlesRenderer packet; const auto before=fake::read_pixels;
    packet.copy_texture_resident(p,bytes.data(),bytes.size(),0x8000);
    check(fake::read_pixels==before+1,"without the direct path copies keep CPU memory semantics");
    std::vector<std::uint8_t> identical(f.vertices.size());
    for(std::size_t i=0;i<identical.size();++i) identical[i]=f.vertices[i%32];
    auto flat=f.command(); flat.vertices={identical.data(),identical.size()};
    const auto zero=renderer.stats().direct.zero_area;
    renderer.draw_direct(f.decoder.state(),flat,f.memory);
    check(renderer.stats().direct.zero_area==zero+1,"byte-identical vertices are zero-area draws");
}
// A lookup of the copy's top rows with its stride: texel (x, y) is the copy's texel (x, y).
void resident_copy_crops() {
    GlesRenderer renderer; Fixture f; f.memory.bytes.resize(1024,0xa5);
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    TextureCopy p; p.width=16; p.height=8; p.format=6; p.stride=256;
    alignas(32) std::array<std::uint8_t,512> bytes{};
    const auto reads=fake::read_pixels, copies=fake::texture_copies;
    renderer.copy_texture_resident(p,bytes.data(),bytes.size(),0x8000);
    renderer.wait_until_idle();
    const auto copy_framebuffer=fake::array_framebuffer;
    f.bp(0x8b,15u|(3u<<10)|(6u<<20)); // RGBA8 16x4
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    auto s=renderer.stats().direct;
    check(s.resident_hits==1 && s.resident_crops==1 && s.materialized==0 && fake::read_pixels==reads &&
          bytes==std::array<std::uint8_t,512>{},"a shorter lookup of a resident copy neither materializes nor reads back");
    check(fake::texture_copies==copies+1 && fake::copy_source==copy_framebuffer && fake::copy_rect==std::array<GLint,4>{0,0,16,4} &&
          fake::draw_texture==fake::copy_texture && fake::color_attachments[copy_framebuffer]!=fake::copy_texture,
          "the lookup samples a GPU copy of the copy's rows 0-3");
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.resident_hits==2 && s.resident_crops==1 && fake::texture_copies==copies+1,"the crop is reused while the copy stands");
    renderer.copy_texture_resident(p,bytes.data(),bytes.size(),0x8000);
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.resident_crops==2 && fake::texture_copies==copies+2 && s.materialized==0,"a new copy at the address is cropped anew");
    f.bp(0x8b,7u|(3u<<10)|(6u<<20)); // RGBA8 8x4: a narrower tile row than the copy's stride
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.materialized==1 && s.resident_crops==2 && fake::read_pixels==reads+1,"a lookup with another stride still materializes");
}
// A downsample chain into memory holding an earlier copy: each copy replaces the start
// of the previous one, whose remaining bytes stay on the GPU until a lookup needs them.
void resident_copy_chain() {
    Fixture f; f.memory.bytes.resize(512,0xa5);
    TextureCopy rgba; rgba.width=16; rgba.height=8; rgba.format=6; rgba.stride=256; // 512 bytes
    TextureCopy wide; wide.width=16; wide.height=4; wide.format=1; wide.luminance=true; wide.stride=64; // 64 bytes
    TextureCopy narrow; narrow.width=8; narrow.height=4; narrow.format=1; narrow.luminance=true; narrow.stride=32; // 32 bytes
    const auto lookup=[&](GlesRenderer& r,std::uint32_t image){f.bp(0x8b,image);r.draw_direct(f.decoder.state(),f.command(),f.memory);r.wait_until_idle();};
    const std::uint32_t rgba_16x8=15u|(7u<<10)|(6u<<20), i8_16x4=0x100c0f, i8_8x4=0x100c07, rgba_8x4=7u|(3u<<10)|(6u<<20);
    // What a readback of a copy returns from the fake, stored as the CPU encoder stores it.
    const auto stored=[](const TextureCopy& p){
        const unsigned width=p.destination_width(),height=p.destination_height();
        std::vector<std::uint8_t> pixels(std::size_t(width)*height*4);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned k=0;k<4;++k)
            pixels[(std::size_t(y)*width+x)*4+k]=std::uint8_t(((std::size_t(y)*width+x)*4+k)%251);
        alignas(32) std::array<std::uint8_t,512> out{};
        store_texture_copy(p,{pixels.data(),pixels.size()},out.data(),out.size());
        return out;
    };
    {
        GlesRenderer renderer; lookup(renderer,i8_8x4);
        alignas(32) std::array<std::uint8_t,512> bytes{};
        const auto reads=fake::read_pixels;
        renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
        lookup(renderer,rgba_16x8);
        renderer.copy_texture_resident(wide,bytes.data(),bytes.size(),0x8000);
        lookup(renderer,i8_16x4);
        renderer.copy_texture_resident(narrow,bytes.data(),bytes.size(),0x8000);
        lookup(renderer,i8_8x4);
        auto s=renderer.stats().direct;
        check(s.resident_hits==3 && s.materialized==0 && fake::read_pixels==reads && bytes==std::array<std::uint8_t,512>{},
              "copies replacing the start of earlier copies neither read back nor write memory");
        renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
        lookup(renderer,rgba_8x4);
        s=renderer.stats().direct;
        check(s.materialized==1 && fake::read_pixels==reads+1,"a copy covering the chain retires it: a lookup materializes the new copy alone");
    }
    fake::map_pattern=true; fake::read_pattern_row=16;
    for(const bool stamps:{false,true}){
        GlesLimits limits; limits.direct.stamp_resident_copies=stamps;
        GlesRenderer renderer(limits); lookup(renderer,i8_8x4);
        alignas(32) std::array<std::uint8_t,512> bytes{};
        renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
        renderer.copy_texture_resident(wide,bytes.data(),bytes.size(),0x8000);
        const auto reads=fake::read_pixels;
        lookup(renderer,rgba_16x8);
        const auto older=stored(rgba),newer=stored(wide);
        check(renderer.stats().direct.materialized==2 && fake::read_pixels==reads+2 &&
              std::equal(bytes.begin(),bytes.begin()+64,newer.begin()) && std::equal(bytes.begin()+64,bytes.end(),older.begin()+64),
              "a lookup of the earlier copy's surviving bytes gets them exactly, beside the newer copy's");
        lookup(renderer,rgba_16x8);
        check(renderer.stats().direct.materialized==2,"materialized surviving bytes are ordinary memory afterwards");
    }
    {
        GlesLimits limits; limits.direct.stamp_resident_copies=true;
        GlesRenderer renderer(limits); lookup(renderer,i8_8x4);
        alignas(32) std::array<std::uint8_t,512> bytes{};
        renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
        renderer.copy_texture_resident(wide,bytes.data(),bytes.size(),0x8000);
        std::fill(bytes.begin()+256,bytes.begin()+288,0x11);
        const auto before=bytes;
        lookup(renderer,rgba_16x8);
        check(renderer.stats().direct.materialized==2 && std::equal(bytes.begin()+64,bytes.end(),before.begin()+64) &&
              std::equal(bytes.begin(),bytes.begin()+64,stored(wide).begin()),
              "a CPU write over the earlier copy's surviving bytes keeps the game's memory there");
        renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
        renderer.copy_texture_resident(wide,bytes.data(),bytes.size(),0x8000);
        renderer.release_resident(0x8000,512);
        const auto reads=fake::read_pixels;
        lookup(renderer,rgba_16x8);
        check(renderer.stats().direct.materialized==2 && fake::read_pixels==reads,"released ranges drop earlier copies too");
    }
    fake::map_pattern=false; fake::read_pattern_row=0;
}
// Host transfers (DMA on the game's behalf) over a resident copy see memory as the console has
// it: a read gets the copy's bytes, a write replaces them, and bytes a write leaves keep the copy's.
void dma_over_resident_copies() {
    Fixture f; f.memory.bytes.resize(512,0xa5);
    TextureCopy rgba; rgba.width=16; rgba.height=8; rgba.format=6; rgba.stride=256; // 512 bytes
    const std::uint32_t rgba_16x8=15u|(7u<<10)|(6u<<20), i8_8x4=0x100c07;
    const auto lookup=[&](GlesRenderer& r,std::uint32_t image){f.bp(0x8b,image);r.draw_direct(f.decoder.state(),f.command(),f.memory);r.wait_until_idle();};
    std::vector<std::uint8_t> pixels(16*8*4);
    for(std::size_t i=0;i<pixels.size();++i) pixels[i]=std::uint8_t(i%251);
    alignas(32) std::array<std::uint8_t,512> stored{};
    store_texture_copy(rgba,{pixels.data(),pixels.size()},stored.data(),stored.size());
    fake::map_pattern=true; fake::read_pattern_row=16;
    GlesRenderer renderer; lookup(renderer,i8_8x4);
    alignas(32) std::array<std::uint8_t,512> bytes{};
    check(!renderer.resident_in(bytes.data(),bytes.size()),"no copy is resident before the first");

    renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
    check(renderer.resident_in(bytes.data()+500,4) && !renderer.resident_in(bytes.data()+512,64),"the copy's destination is resident");
    auto reads=fake::read_pixels; auto s=renderer.stats().direct;
    renderer.before_dma_write(bytes.data(),bytes.size());
    bytes.fill(0x33);
    lookup(renderer,rgba_16x8);
    auto t=renderer.stats().direct;
    check(t.resident_hits==s.resident_hits && t.materialized==s.materialized && t.dma_overwritten==s.dma_overwritten+1 &&
          fake::read_pixels==reads && std::all_of(bytes.begin(),bytes.end(),[](std::uint8_t b){return b==0x33;}),
          "a lookup after a transfer wrote over a copy reads the transfer's bytes, not the copy");
    check(!renderer.resident_in(bytes.data(),bytes.size()),"a copy a transfer wrote over is no longer resident");

    renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
    s=renderer.stats().direct;
    renderer.before_dma_read(bytes.data()+100,4);
    t=renderer.stats().direct;
    check(t.materialized==s.materialized+1 && fake::read_pixels==reads+1 && bytes==stored,
          "a transfer reading part of a copy gets the copy's bytes");
    renderer.before_dma_read(bytes.data(),bytes.size());
    check(renderer.stats().direct.materialized==t.materialized && fake::read_pixels==reads+1,
          "a materialized copy is ordinary memory for later transfers");

    bytes.fill(0);
    renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
    renderer.before_dma_write(bytes.data()+256,64);
    std::fill(bytes.begin()+256,bytes.begin()+320,0x44);
    s=renderer.stats().direct;
    lookup(renderer,rgba_16x8);
    t=renderer.stats().direct;
    check(t.resident_hits==s.resident_hits && t.materialized==s.materialized+1 && fake::read_pixels==reads+2 &&
          std::equal(bytes.begin(),bytes.begin()+256,stored.begin()) && std::equal(bytes.begin()+320,bytes.end(),stored.begin()+320) &&
          std::all_of(bytes.begin()+256,bytes.begin()+320,[](std::uint8_t b){return b==0x44;}),
          "a lookup of a partly written copy keeps the written bytes and gets the copy's others");

    bytes.fill(0);
    renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
    renderer.before_dma_write(bytes.data()+256,256);
    check(renderer.resident_in(bytes.data(),256),"a copy's unwritten bytes stay resident");
    renderer.before_dma_write(bytes.data(),256);
    s=renderer.stats().direct;
    lookup(renderer,rgba_16x8);
    t=renderer.stats().direct;
    check(!renderer.resident_in(bytes.data(),bytes.size()) && t.materialized==s.materialized && fake::read_pixels==reads+2 &&
          bytes==std::array<std::uint8_t,512>{},"transfers writing all of a copy in parts drop it");

    renderer.copy_texture_resident(rgba,bytes.data(),bytes.size(),0x8000);
    renderer.before_dma_write(bytes.data(),64);
    std::fill(bytes.begin(),bytes.begin()+64,0x55);
    renderer.before_dma_read(bytes.data()+64,448);
    check(renderer.stats().direct.materialized==t.materialized+1 && std::equal(bytes.begin()+64,bytes.end(),stored.begin()+64) &&
          std::all_of(bytes.begin(),bytes.begin()+64,[](std::uint8_t b){return b==0x55;}),
          "a transfer reading a partly written copy gets the copy's remaining bytes beside the written ones");
    fake::map_pattern=false; fake::read_pattern_row=0;
}
void stamped_resident_copies() {
    GlesLimits limits; limits.direct.stamp_resident_copies=true;
    GlesRenderer renderer(limits); Fixture f; f.memory.bytes.resize(64,0xa5); f.bp(0x8b,0x100c0f);
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    TextureCopy p; p.width=16; p.height=4; p.format=1; p.luminance=true; p.stride=64;
    alignas(32) std::array<std::uint8_t,64> bytes{};
    const std::array<std::uint8_t,64> zero{};
    const auto reads=fake::read_pixels;
    renderer.copy_texture_resident(p,bytes.data(),bytes.size(),0x8000);
    check(fake::read_pixels==reads && bytes!=zero,"stamped copy overwrites its destination without a readback");
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    auto s=renderer.stats().direct;
    check(s.resident_hits==1 && s.materialized==0,"intact stamps sample the resident copy");
    const auto stamped=bytes;
    std::fill(bytes.begin()+32,bytes.end(),0x11);
    const auto reads_before=fake::read_pixels;
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    s=renderer.stats().direct;
    check(s.resident_hits==1 && s.materialized==1,"a later CPU write ends sampling and retires the copy");
    check(std::equal(bytes.begin(),bytes.begin()+32,stamped.begin()) &&
          std::all_of(bytes.begin()+32,bytes.end(),[](std::uint8_t b){return b==0x11;}) && fake::read_pixels==reads_before,
          "a copy with a broken stamp writes nothing and reads nothing back");
    renderer.copy_texture_resident(p,bytes.data(),bytes.size(),0x8000);
    const auto first=bytes;
    renderer.copy_texture_resident(p,bytes.data(),bytes.size(),0x8000);
    check(bytes!=first,"each copy stamps its destination anew");
    bytes.fill(0x22);
    f.bp(0x8b,0x100c07);
    renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
    check(renderer.stats().direct.materialized==2 && std::all_of(bytes.begin(),bytes.end(),[](std::uint8_t b){return b==0x22;}),
          "a fully overwritten copy materializes nothing");
}
// JFWDisplay::clearEfb on every draw path: the Z texel becomes the fragment depth, the depth
// test runs ALWAYS with writes, and the bias is a uniform of the program.
void z_texture_clear() {
    fake::track_uniforms = true;
    const auto fragment_depth = [](std::size_t first) {
        for (auto i = first; i < fake::shader_sources.size(); ++i)
            if (fake::shader_sources[i].find("gl_FragDepth=float(gxZ)/16777216.0;") != std::string::npos) return true;
        return false;
    };
    const auto bias = [] { return fake::uniform_values.at(fake::uniform_locations.at({fake::current_program, "uGxZBias"}))[0]; };
    for (const auto shading : {DirectShading::fast, DirectShading::exact}) {
        GlesLimits limits; limits.direct.shading = shading; limits.direct.background_programs = false;
        GlesRenderer renderer(limits); Fixture f; jfw_clear_efb(f, {{0x20, 0x40, 0x60, 0xff}}, shading == DirectShading::fast);
        const auto sources = fake::shader_sources.size(); const auto before = fake::draws;
        fake::depth_test = GL_FALSE; fake::depth_func = GL_NEVER; fake::depth_mask = GL_FALSE;
        renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
        check(fake::draws == before + 1 && renderer.stats().direct.texture_uploads == 1, "the JFWDisplay clear draws with its Z24X8 map");
        check(fragment_depth(sources), "the direct program writes the Z texel as fragment depth");
        check(fake::depth_test && fake::depth_func == GL_ALWAYS && fake::depth_mask, "late ALWAYS depth test with writes");
        check(bias() == 0, "zero bias uploaded");
        const auto misses = renderer.stats().direct.program_misses;
        f.bp(0xf4, 0x123456);
        renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
        check(fake::draws == before + 2 && renderer.stats().direct.program_misses == misses && bias() == 0x123456,
              "a bias change updates the uniform, not the program");
    }
    GlesRenderer renderer; Fixture f; jfw_clear_efb(f, {{0x20, 0x40, 0x60, 0xff}});
    f.bp(0xf4, 0x654321);
    const auto sources = fake::shader_sources.size(); const auto before = fake::draws;
    fake::depth_test = GL_FALSE; fake::depth_func = GL_NEVER; fake::depth_mask = GL_FALSE;
    renderer.draw(f.decoder.state(), f.command(), f.memory);
    check(fake::draws == before + 1 && fragment_depth(sources) && fake::depth_test && fake::depth_func == GL_ALWAYS &&
          fake::depth_mask && bias() == 0x654321, "the packet path draws the clear with its Z texture");
    fake::track_uniforms = false;
}
// Luigi's Mansion's intro: in an RGBA6 frame the SDK selects Z24 for a depth copy, whose
// clear leaves the EFB in the RGB8 layout, then draws RGBA6 again and copies the result.
// Exact shading converts the EFB back before that draw; fast shading never reinterprets, so
// the later RGBA6 copy reads what the fast programs drew.
void format_after_depth_copy() {
    for (const auto shading : {DirectShading::fast, DirectShading::exact}) {
        const unsigned exact = shading == DirectShading::exact;
        GlesLimits limits; limits.direct.shading = shading; limits.direct.background_programs = false;
        GlesRenderer renderer(limits); Fixture f;
        f.bp(0x41, 0xf4a8); f.bp(0x43, 0x41); // No blending: exact RGBA6 blending needs framebuffer fetch.
        renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
        check(renderer.stats().format_conversions == exact, "only exact shading converts the RGB8 EFB for RGBA6");
        TextureCopy depth; depth.width = 8; depth.height = 4; depth.format = 11; depth.depth = true; depth.stride = 64;
        EfbClear clear; clear.rectangle = {0, 452, 8, 4}; clear.color_write = true; depth.clear = clear;
        alignas(32) std::array<std::uint8_t, 64> z{};
        renderer.copy_texture_resident(depth, z.data(), z.size(), 0x8000); renderer.wait_until_idle();
        const auto cleared = renderer.stats().format_conversions;
        check(cleared == 2 * exact, "only exact shading converts for the Z24 clear");
        renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
        check(renderer.stats().format_conversions == cleared + exact, "exact shading restores the RGBA6 layout before drawing");
        TextureCopy color; color.width = 8; color.height = 4; color.format = 4; color.rgba6 = true; color.stride = 64;
        alignas(32) std::array<std::uint8_t, 64> texels{};
        renderer.copy_texture_resident(color, texels.data(), texels.size(), 0x9000); renderer.wait_until_idle();
        check(renderer.stats().format_conversions == cleared + exact, "the RGBA6 copy reinterprets nothing it was drawn in");
    }
}
// A copy whose clear rewrites the whole EFB depth leaves the depth the copy does not read
// dead before the pass ends: it is invalidated. Partial clears, clears that keep depth and
// depth copies (which read it) keep it.
void copy_clear_invalidation() {
    GlesRenderer renderer; Fixture f;
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
    alignas(32) std::array<std::uint8_t, 0x96000> bytes{};
    auto copy = [&](int width, int height, bool depth, bool depth_write, bool color_write, bool alpha_write) {
        TextureCopy p; p.width = width; p.height = height; p.format = depth ? 11 : 4; p.depth = depth; p.stride = ((width + 3) / 4) * 32;
        EfbClear clear; clear.rectangle = {0, 456 - height, width, height};
        clear.depth_write = depth_write; clear.color_write = color_write; clear.alpha_write = alpha_write; p.clear = clear;
        fake::invalidated.clear();
        renderer.copy_texture_resident(p, bytes.data(), bytes.size(), 0x8000); renderer.wait_until_idle();
        return fake::invalidated;
    };
    check(copy(640, 456, false, true, true, false) == std::vector<GLenum>{GL_DEPTH_ATTACHMENT}, "a full colour copy and clear invalidates depth");
    check(copy(320, 456, false, true, true, false).empty(), "a partial clear invalidates nothing");
    check(copy(640, 456, false, false, true, false).empty(), "a clear that keeps depth invalidates nothing");
    check(copy(640, 456, true, true, true, true) == std::vector<GLenum>{GL_COLOR_ATTACHMENT0}, "a full depth copy and colour clear invalidates colour");
    check(copy(640, 456, true, true, true, false).empty(), "colour stays while its alpha is kept");
    check(renderer.stats().invalidations == 2, "invalidations are counted");
}
void direct_batched_draws() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits); Fixture f; const auto command = f.command();
    const auto before = fake::draws;
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    f.f(3, 10.0f);
    f.bp(0xe2, 100u | (200u << 12));
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    check(s.batched_draws == 2 && s.submissions == 1 && fake::draws == before + 1,
          "draws differing in their position matrix and TEV registers merge");
    check(s.uniform_blocks == 1 && s.state_plans == 1, "batched draws share one uniform block and one state plan");
    const std::size_t stride = 64;
    check(fake::submitted_vertices.size() >= 8 * stride, "batched vertices streamed");
    auto at_float = [&](std::size_t offset) { float v; std::memcpy(&v, fake::submitted_vertices.data() + offset, 4); return v; };
    auto at_short = [&](std::size_t offset) { std::int16_t v; std::memcpy(&v, fake::submitted_vertices.data() + offset, 2); return v; };
    check(at_float(0) == 100.0f && at_float(4 * stride) == 110.0f && at_float(4 * stride + 4) == 100.0f,
          "positions are transformed by each draw's own matrix");
    check(at_short(32 + 8) == 0 && at_short(4 * stride + 32 + 8) == 100 && at_short(4 * stride + 32 + 14) == 200,
          "TEV registers travel with each draw's vertices");
    f.bp(0xf3, 0x3f0011);
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    renderer.wait_until_idle();
    s = renderer.stats().direct;
    check(s.state_plans == 2 && s.submissions == 2, "other BP state splits batches");
    f.xf(0x1040, 1u << 2); // Texgen 0 from position (ABC1).
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    renderer.wait_until_idle();
    check(renderer.stats().direct.batched_draws == 3, "position texgens keep the object-space path");
}
// Resident Evil 4 draws particles as unlit quads whose layout carries a normal; nbt: N, B and T.
void use_normals(Fixture& f, bool nbt = false) {
    f.cp(0x50, (1u << 9) | (1u << 11) | (1u << 13));
    f.cp(0x70, 1u | (4u << 1) | (std::uint32_t{nbt} << 9) | (4u << 10) | (1u << 13) | (5u << 14) | (1u << 21) | (4u << 22));
    std::vector<std::uint8_t> with;
    for (unsigned v = 0; v < 4; ++v) {
        with.insert(with.end(), f.vertices.begin() + v * 32, f.vertices.begin() + v * 32 + 12);
        for (unsigned n = 0; n < (nbt ? 3u : 1u); ++n) for (unsigned k = 0; k < 3; ++k) append_float(with, k == (2 + n) % 3 ? 1.0f : 0.0f);
        with.insert(with.end(), f.vertices.begin() + v * 32 + 12, f.vertices.begin() + v * 32 + 32);
    }
    f.vertices = std::move(with);
}
GlesLimits batching_limits() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast; limits.direct.background_programs = false;
    return limits;
}
void direct_uniform_deltas() {
    fake::track_uniforms = true;
    GlesRenderer renderer(batching_limits()); Fixture f;
    f.xf(0x1040, 1u << 2); // Object-space texgen keeps these draws out of particle batches.
    f.bp(0xe2, 100u | (200u << 12));
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    f.f(3, 10);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    renderer.wait_until_idle();
    const auto first_program = fake::current_program;
    const auto location = fake::uniform_locations.at({first_program, "uGxTevRegisters[1]"});
    check(std::abs(fake::uniform_values.at(location)[0] - 100.0 / 255) < 1e-6,
          "direct uniforms use the recorded TEV value");
    const auto updates = fake::uniform_updates;
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    renderer.wait_until_idle();
    check(fake::uniform_updates == updates, "new segments retain unchanged per-program uniforms");
    f.bp(0xf3, 0x3e0011);
    f.bp(0xe2, 80u | (160u << 12));
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    f.bp(0xf3, 0x3f0000);
    f.bp(0xe2, 100u | (200u << 12));
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    f.f(3, 20);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    renderer.wait_until_idle();
    check(fake::current_program == first_program &&
          std::abs(fake::uniform_values.at(location)[0] - 100.0 / 255) < 1e-6,
          "interleaved programs keep independent uniform delta chains");
    fake::track_uniforms = false;
}
void direct_state_plan_cache() {
    GlesRenderer renderer(batching_limits()); Fixture f;
    f.xf(0x1040, 1u << 2);
    for (unsigned frame = 0; frame < 3; ++frame) {
        for (unsigned color : {10u, 20u, 10u}) {
            f.bp(0xe2, color | (color << 12));
            renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
        }
        renderer.wait_until_idle();
    }
    const auto stats = renderer.stats().direct;
    check(stats.state_misses == 1 && stats.state_hits >= 6, "changing TEV colours reuse state plans across segments");
    f.f(0x101c, 8388608);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    check(renderer.stats().direct.state_misses == stats.state_misses + 1, "viewport change cannot reuse a cached raster plan");
}
void direct_group_results() {
    GlesRenderer renderer; Fixture f; const auto draw = f.command();
    renderer.draw_direct(f.decoder.state(), draw, f.memory);
    renderer.draw_direct(f.decoder.state(), draw, f.memory);
    auto before = renderer.stats().direct;
    check(before.state_plans == 1 && before.state_reuses == 1, "equal draws retain one immutable segment state");
    f.bp(0xf3, 0x3f0011);
    renderer.draw_direct(f.decoder.state(), draw, f.memory);
    auto after = renderer.stats().direct;
    check(after.raster_plan_hits == before.raster_plan_hits + 1 &&
          after.texture_plan_hits == before.texture_plan_hits + 1,
          "alpha-reference changes reuse independent raster and texture results");
    const auto image = f.decoder.state().bp(0x83);
    f.bp(0x83, image ^ 1);
    renderer.draw_direct(f.decoder.state(), draw, f.memory);
    before = renderer.stats().direct;
    check(before.raster_plan_hits == after.raster_plan_hits + 1 && before.texture_plan_hits == after.texture_plan_hits,
          "texture changes preserve raster results but rebuild texture descriptors");
    f.bp(0x83, image);
    renderer.draw_direct(f.decoder.state(), draw, f.memory);
    after = renderer.stats().direct;
    check(after.state_plans == before.state_plans && after.state_reuses == before.state_reuses + 1,
          "returning to a state reuses its retained segment index without another copy");
    f.bp(0x88, f.decoder.state().bp(0x88) ^ 1);
    renderer.draw_direct(f.decoder.state(), draw, f.memory);
    before = renderer.stats().direct;
    check(before.texture_plan_hits == after.texture_plan_hits + 1 && before.raster_plan_hits == after.raster_plan_hits + 1,
          "an unused texture map preserves both planning groups");
    after = before;
    f.f(0x101c, 8388608);
    renderer.draw_direct(f.decoder.state(), draw, f.memory);
    before = renderer.stats().direct;
    check(before.texture_plan_hits == after.texture_plan_hits + 1 && before.raster_plan_hits == after.raster_plan_hits,
          "viewport changes preserve texture results and rebuild raster state");
    f.bp(0xc0, f.decoder.state().bp(0xc0) ^ 1);
    renderer.draw_direct(f.decoder.state(), draw, f.memory);
    after = renderer.stats().direct;
    check(after.raster_plan_hits == before.raster_plan_hits + 1 && after.texture_plan_hits == before.texture_plan_hits + 1,
          "programs with equal raster and sampling policies share group results");
    before = after;
    f.bp(0x22, f.decoder.state().bp(0x22) ^ 1);
    f.bp(0x3e, f.decoder.state().bp(0x3e) ^ 1);
    f.bp(0x10, f.decoder.state().bp(0x10) ^ 1);
    renderer.draw_direct(f.decoder.state(), draw, f.memory);
    after = renderer.stats().direct;
    check(after.raster_plan_hits == before.raster_plan_hits + 1 && after.texture_plan_hits == before.texture_plan_hits + 1,
          "triangle line width and unused coordinate or indirect controls preserve group results");
    renderer.wait_until_idle();
}
void split_stream_geometry() {
    struct Arrays : test::Memory {
        std::vector<std::uint8_t> positions;
        std::uint64_t generation = 1;
        MemoryArrayView array_view(std::uint32_t address) override {
            if (address != 0x1000 && address != 0x2000) return {};
            return {{positions.data(), positions.size()}, 0, false, generation};
        }
        SourceIdentity source_identity(const void* data, std::size_t) override { return {data, generation}; }
    };
    GlesLimits limits; limits.direct.geometry_min_vertices = 100000;
    GlesRenderer renderer(limits); Fixture f; Arrays memory; memory.bytes = f.memory.bytes;
    const auto original = f.vertices;
    f.cp(0x50, (2u << 9) | (1u << 13)); f.cp(0xa0, 0x1000); f.cp(0xb0, 12);
    f.vertices.clear();
    for (unsigned repeat = 0; repeat < 3; ++repeat) for (unsigned v = 0; v < 4; ++v) {
        f.vertices.push_back(v);
        f.vertices.insert(f.vertices.end(), original.begin() + v * 32 + 12, original.begin() + (v + 1) * 32);
        f.vertices[f.vertices.size() - 20] ^= repeat;
    }
    for (unsigned v = 0; v < 4; ++v)
        memory.positions.insert(memory.positions.end(), original.begin() + v * 32, original.begin() + v * 32 + 12);
    auto draw = f.command(); draw.vertex_count = 12;
    const auto verify = [&] {
        renderer.draw_direct(f.decoder.state(), draw, memory); renderer.wait_until_idle();
        std::vector<std::uint8_t> vertices; std::vector<std::uint32_t> indices;
        const auto format = stream_format(draw.layout);
        check(bool(append_stream_vertices(f.decoder.state(), draw, memory, format, vertices, indices, 0)), "fresh split reference");
        check(indices.size() == fake::split_indices.size(), "persistent split index count");
        for (std::size_t i = 0; i < indices.size(); ++i)
            check(!std::memcmp(vertices.data() + indices[i] * format.stride,
                               fake::split_vertices.data() + fake::split_indices[i] * format.stride, format.stride),
                  "split vertex fetch matches fresh interleaved conversion");
    };
    const auto start = fake::split_draws;
    verify(); const auto uploads = fake::static_buffer_uploads;
    check(fake::split_draws == start + 1, "cached twelve-vertex draw uses split GPU streams");
    verify();
    check(fake::static_buffer_uploads == uploads, "unchanged static attributes and indices stay on GPU");
    memory.positions[3] ^= 1; ++memory.generation; verify();
    check(fake::static_buffer_uploads == uploads, "position changes stream only the dynamic prefix");
    f.cp(0xa0, 0x2000); verify();
    check(fake::static_buffer_uploads == uploads, "moving equal array bases retains static GPU data");
    f.vertices[1] ^= 1; ++memory.generation; verify();
    check(fake::static_buffer_uploads == uploads + 1, "payload mutation replaces immutable static data and indices");
    draw.primitive = 0x90; verify();
    check(fake::static_buffer_uploads == uploads + 2, "topology change gets independent persistent indices");
    std::vector<std::vector<std::uint8_t>> payloads(4100, f.vertices);
    for (const auto& payload : payloads) {
        auto next = draw; next.vertices = {payload.data(), payload.size()};
        renderer.draw_direct(f.decoder.state(), next, memory);
    }
    renderer.wait_until_idle();
    verify();
    check(fake::split_draws >= start + payloads.size(), "CPU cache eviction retains buffers until queued draws finish");
}
void shared_state_snapshots() {
    GlesRenderer renderer; Fixture f; const auto draw = f.command();
    for (unsigned color = 0; color < 32; ++color) {
        f.bp(0xe2, color | (color << 12));
        renderer.draw_direct(f.decoder.state(), draw, f.memory);
    }
    const auto stats = renderer.stats().direct;
    check(stats.state_plans == 32 && stats.raster_snapshots == 1 && stats.texture_snapshots == 1,
          "per-draw TEV changes share immutable raster and texture snapshots");
    check(stats.state_snapshot_bytes < stats.state_plans * sizeof(DirectDrawState) / 4,
          "retained state copies are bounded by shared groups and compact draw values");
    renderer.wait_until_idle();
}
void direct_batched_normals() {
    std::vector<std::uint8_t> plain;
    {
        GlesRenderer renderer(batching_limits()); Fixture f;
        renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
        plain.assign(fake::submitted_vertices.begin(), fake::submitted_vertices.begin() + 4 * 64);
    }
    GlesRenderer renderer(batching_limits()); Fixture f; const auto original = f.vertices; use_normals(f);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    check(s.batched_draws == 1 && s.vertex_bytes == 4 * 64, "an unlit layout carrying a normal is batched");
    check(std::equal(plain.begin(), plain.end(), fake::submitted_vertices.begin()),
          "its batched vertices drop the normal and equal those of the same draw without one");
    const auto unbatched = [&](Fixture& from, const char* message) {
        const auto before = renderer.stats().direct;
        renderer.draw_direct(from.decoder.state(), from.command(), from.memory); renderer.wait_until_idle();
        const auto after = renderer.stats().direct;
        check(after.draws == before.draws + 1 && after.batched_draws == before.batched_draws, message);
    };
    f.xf(0x100e, 1u | 2u | (1u << 2) | (2u << 7)); // RGB0 lit by light 0, clamped diffuse.
    unbatched(f, "a lit layout stays out of batches");
    f.xf(0x100e, 1); f.xf(0x1040, (1u << 7) | (1u << 2)); // Texgen 0 from the normal (ABC1).
    unbatched(f, "texgens from the normal stay out of batches");
    f.xf(0x1040, 5u << 7);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
    check(renderer.stats().direct.batched_draws == s.batched_draws + 1, "restored state batches again");
    Fixture e; use_normals(e, true);
    e.xf(0x1041, 1u << 4); // Texgen 1 embosses texgen 0 towards light 0.
    unbatched(e, "emboss stays out of batches");
    f.cp(0x50, (1u << 9) | (1u << 13));
    f.cp(0x70, 1u | (4u << 1) | (1u << 13) | (5u << 14) | (1u << 21) | (4u << 22));
    f.vertices = original;
    f.xf(0x100e, 1u | 2u | (1u << 2) | (2u << 7));
    unbatched(f, "a lit draw reading the latched normal stays out of batches");
}
void direct_batch_merging() {
    GlesRenderer renderer(batching_limits()); Fixture f;
    const auto draw = [&] { renderer.draw_direct(f.decoder.state(), f.command(), f.memory); };
    draw();
    f.bp(0x41, 0xf4a8); f.bp(0x41, 0xf4a9); // Blend mode changed and restored.
    f.xf(0x100e, 3); f.xf(0x100e, 1); // Channel control changed and restored.
    f.bp(0xfe, 0x00ffff); f.bp(0x41, 0xf4a9); // A masked write of the same value.
    f.f(3, 10.0f);
    draw();
    for (unsigned i = 0; i < 9; ++i) f.f(0x400 + i, 2.0f); // A normal matrix load.
    f.f(30 * 4, 5.0f); // A position row no texgen reads.
    f.f(3, 20.0f);
    draw();
    renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    check(s.batched_draws == 3 && s.submissions == 1, "same-value register writes and normal matrix loads keep one batch");
    check(s.state_plans == 1 && s.uniform_blocks == 1, "they share one state plan and one uniform block");
    draw();
    f.f(60 * 4, 2.0f); // The texture matrix texgens 0 and 1 read.
    draw(); renderer.wait_until_idle();
    auto t = renderer.stats().direct;
    check(t.state_plans == s.state_plans + 1 && t.uniform_blocks == s.uniform_blocks + 2 && t.submissions == s.submissions + 2,
          "a texture matrix the texgens read splits the batch");
    draw();
    f.bp(0xe0, (1u << 23) | 0x10); // Konst register 0.
    draw(); renderer.wait_until_idle();
    s = renderer.stats().direct;
    check(s.state_plans == t.state_plans + 2 && s.uniform_blocks == t.uniform_blocks + 1 && s.submissions == t.submissions + 2,
          "a konst colour change splits the batch");
}
void direct_batched_materials() {
    GlesRenderer renderer(batching_limits()); Fixture f;
    const auto sources = fake::shader_sources.size();
    f.xf(0x100e, 0); // RGB0 unlit with a register material; A0 keeps the vertex alpha.
    f.xf(0x100c, 0x11223344);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    f.xf(0x100c, 0x55667788);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    check(s.batched_draws == 2 && s.submissions == 1 && s.uniform_blocks == 1 && s.state_plans == 1,
          "draws differing in their unlit material colour merge");
    const std::size_t stride = 12 + 8 + 16 + 32;
    const auto& v = fake::submitted_vertices;
    const auto color = [&](std::size_t vertex, unsigned slot) {
        return std::array<std::uint8_t, 4>{{v[vertex * stride + 12 + slot * 4], v[vertex * stride + 13 + slot * 4],
                                            v[vertex * stride + 14 + slot * 4], v[vertex * stride + 15 + slot * 4]}};
    };
    check(v.size() >= 8 * stride && color(0, 0) == std::array<std::uint8_t, 4>{{0x11, 0x22, 0x33, 255}} &&
          color(1, 0) == std::array<std::uint8_t, 4>{{0x11, 0x22, 0x33, 192}} &&
          color(5, 0) == std::array<std::uint8_t, 4>{{0x55, 0x66, 0x77, 192}} && color(5, 1) == std::array<std::uint8_t, 4>{{255, 255, 255, 255}},
          "each draw's material colour travels in its vertex colour beside the vertex alpha");
    bool vertex_material = false;
    for (auto i = sources; i < fake::shader_sources.size(); ++i) {
        const auto& source = fake::shader_sources[i];
        if (source.find("a_register0") == std::string::npos) continue;
        vertex_material = source.find("vec4 material = round(a_color0*255.0)") != std::string::npos &&
                          source.find("u_material[0]") == std::string::npos;
    }
    check(vertex_material, "the batched program reads its material from the vertex colour");
    TransformUniforms block{};
    std::memcpy(reinterpret_cast<unsigned char*>(&block) + transform_shared_offset,
                fake::uniform_bytes.data() + fake::uniform_bindings.at(0).first, transform_shared_bytes);
    check(block.material[0] == TransformVector{} && block.material[1] == TransformVector{}, "GXTransform holds no material the program does not read");

    // Channel 0 reads vertex colour 1 when colour 0 is absent.
    Fixture g; g.cp(0x50, (1u << 9) | (1u << 15)); g.cp(0x70, 1u | (4u << 1) | (1u << 17) | (5u << 18) | (1u << 21) | (4u << 22));
    g.xf(0x100e, 0); g.xf(0x100c, 0x11223344);
    renderer.draw_direct(g.decoder.state(), g.command(), g.memory); renderer.wait_until_idle();
    check(renderer.stats().direct.batched_draws == s.batched_draws + 1 && color(1, 0) == std::array<std::uint8_t, 4>{{0x11, 0x22, 0x33, 192}} &&
          color(1, 1) == std::array<std::uint8_t, 4>{{255, 255, 255, 255}}, "colour 0 then holds what channel 0 read");

    s = renderer.stats().direct;
    f.xf(0x100e, 2); // Lit without lights: the material stays a register.
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    f.xf(0x100c, 0x11223344);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
    renderer.wait_until_idle();
    const auto lit = renderer.stats().direct;
    check(lit.batched_draws == s.batched_draws + 2 && lit.uniform_blocks == s.uniform_blocks + 1 &&
          lit.shared_blocks == s.shared_blocks + 2 && lit.submissions == s.submissions + 2,
          "a lit channel's register material splits batches with a new GXTransform block");
}
// A lit draw with a normal attribute whose texgens read rows 60 and 30: see direct_uniform_blocks.
void lit_two_matrices(Fixture& f) {
    use_normals(f);
    f.xf(0x100e, 2u | (1u << 2) | (2u << 7)); // RGB0 lit by light 0, clamped; register material and ambient.
    f.xf(0x100c, 0x11223344); f.xf(0x100a, 0x05060708);
    f.xf(0x603, 0x80604020);
    for (unsigned k = 0; k < 3; ++k) f.f(0x60a + k, 10.0f * (k + 1));
    for (unsigned i = 0; i < 9; ++i) f.f(0x400 + i, 0.5f + i);
    for (unsigned i = 0; i < 12; ++i) f.f(30 * 4 + i, 2.0f + i);
    f.xf(0x1018, (60u << 6) | (30u << 12));
}
bool same_lanes(GLintptr offset, unsigned row, const TransformVector& v, unsigned first = 0, unsigned end = 4) {
    return !std::memcmp(fake::uniform_bytes.data() + offset + row * 16 + first * 4, v.data() + first, (end - first) * 4);
}
// Unbatched draws pack the matrix rows they read into GXMatrices and share GXTransform, which
// is recorded again only when a lane the program reads changes.
void direct_uniform_blocks() {
    fake::uniform_alignment = 16; fake::uniform_bindings.clear();
    GlesRenderer renderer; Fixture f; lit_two_matrices(f);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    check(s.draws == 1 && s.batched_draws == 0 && s.uniform_blocks == 1 && s.shared_blocks == 1, "one draw records one block of each");
    const auto matrices = fake::uniform_bindings.at(2), shared = fake::uniform_bindings.at(0);
    check(matrices.second == GLsizeiptr(transform_matrix_bytes) && shared.second == GLsizeiptr(transform_shared_bytes) &&
          shared.first == 0 && matrices.first == GLintptr(transform_shared_bytes) &&
          fake::uniform_bytes.size() == transform_shared_bytes + transform_matrix_bytes && s.uniform_bytes == fake::uniform_bytes.size(),
          "blocks are bound at their declared sizes, the buffer padded to reach them");
    check(fake::uniform_bindings.count(3) == 0, "programs without post-transformed texgens bind no GXPost");
    // Position rows 0-2, normal rows 3-5, then texgen 0's rows 60-61 and texgen 1's rows 30-31.
    check(fake::matrix_latches[0] == (6u << 8 | 8u << 16 | 3u << 24) && fake::matrix_latches[1] == 0 && fake::matrix_latches[2] == 0,
          "the matrix indices name packed rows, the normal matrix in the top byte");
    TransformUniforms full; direct_transform_uniforms(f.decoder.state(), full);
    bool rows = true;
    for (unsigned k = 0; k < 3; ++k) rows = rows && same_lanes(matrices.first, k, full.position[k]) && same_lanes(matrices.first, 3 + k, full.normal[k]);
    for (unsigned k = 0; k < 2; ++k)
        rows = rows && same_lanes(matrices.first, 6 + k, full.position[60 + k]) && same_lanes(matrices.first, 8 + k, full.position[30 + k]);
    check(rows, "each packed row is, bit for bit, the row the full form holds at the original index");
    // GXTransform rows: projection 0-1, ambient 2-3, material 4-5, lights 6-45, latched vectors 46-48.
    bool read = same_lanes(0, 0, full.projection[0]) && same_lanes(0, 1, full.projection[1]) &&
                same_lanes(0, 2, full.ambient[0], 0, 3) && same_lanes(0, 4, full.material[0], 0, 3);
    for (unsigned k = 0; k < 5; ++k) read = read && same_lanes(0, 6 + k, (&full.lights[0].color)[k]);
    check(read, "GXTransform holds the projection, colours and light the program reads, bit for bit");

    // In one segment: matrix changes add only GXMatrices blocks; a read light, projection or
    // material lane adds a GXTransform block; unread lanes and lights add nothing.
    const auto before = renderer.stats().direct;
    const auto draw = [&] { renderer.draw_direct(f.decoder.state(), f.command(), f.memory); };
    for (float x : {1.0f, 2.0f, 3.0f}) { f.f(3, x); draw(); }
    f.xf(0x603, 0x10203040); draw();
    f.f(0x1020, 1.0f / 640); draw();
    f.xf(0x100c, 0x99887744); draw();
    f.xf(0x100c, 0x998877ff); draw(); // Material alpha: A0 reads the vertex colour.
    f.xf(0x613, 0x10203040); draw(); // Light 1 colour.
    renderer.wait_until_idle();
    s = renderer.stats().direct;
    check(s.draws == before.draws + 8 && s.uniform_blocks == before.uniform_blocks + 3 && s.shared_blocks == before.shared_blocks + 4,
          "GXTransform is recorded once for draws sharing it and again for each read change");
    check(s.uniform_bytes - before.uniform_bytes == 4 * transform_shared_bytes + 3 * 160, "each matrix change adds its ten packed rows");

    // Post-transformed texgens read GXPost, recorded with GXTransform's rule.
    f.xf(0x1012, 1); f.xf(0x1050, 20); f.xf(0x1051, 20);
    for (unsigned i = 0; i < 12; ++i) f.f(0x500 + 20 * 4 + i, 1.0f + i);
    const auto plain = renderer.stats().direct;
    for (float x : {4.0f, 5.0f}) { f.f(3, x); draw(); }
    f.f(0x500 + 40 * 4, 9.0f); draw(); // A post row no texgen reads.
    renderer.wait_until_idle();
    s = renderer.stats().direct;
    const auto post = fake::uniform_bindings.at(3);
    direct_transform_uniforms(f.decoder.state(), full);
    bool post_rows = post.second == GLsizeiptr(transform_post_bytes) && std::size_t(post.first) + transform_post_bytes <= fake::uniform_bytes.size();
    for (unsigned k = 0; k < 3 && post_rows; ++k) post_rows = same_lanes(post.first, 20 + k, full.post[20 + k]);
    check(post_rows && s.shared_blocks == plain.shared_blocks + 2 && s.uniform_blocks == plain.uniform_blocks + 2,
          "GXPost holds the post rows read and is recorded once for the segment");
    fake::uniform_alignment = 0;
}
// Vertices with their own matrix indices read GXMatrices in the full form.
void direct_full_matrix_block() {
    GlesRenderer renderer; Fixture f;
    f.cp(0x50, 1u | (1u << 9) | (1u << 13));
    std::vector<std::uint8_t> with;
    for (unsigned v = 0; v < 4; ++v) {
        with.push_back(0);
        with.insert(with.end(), f.vertices.begin() + v * 32, f.vertices.begin() + v * 32 + 32);
    }
    f.vertices = std::move(with);
    f.f(0x400, 3.0f); f.f(0x500, 4.0f);
    renderer.draw_direct(f.decoder.state(), f.command(), f.memory); renderer.wait_until_idle();
    TransformUniforms full; direct_transform_uniforms(f.decoder.state(), full);
    const auto matrices = fake::uniform_bindings.at(2);
    check(renderer.stats().direct.uniform_blocks >= 1 && std::size_t(matrices.first) + transform_matrix_bytes <= fake::uniform_bytes.size() &&
          !std::memcmp(fake::uniform_bytes.data() + matrices.first, &full, transform_matrix_bytes),
          "the full form is the XF's position rows then its normal rows");
    const auto format = stream_format(f.command().layout);
    bool words = fake::submitted_vertices.size() >= 4u * format.stride;
    for (unsigned v = 0; v < 4 && words; ++v) {
        std::uint32_t word; std::memcpy(&word, fake::submitted_vertices.data() + v * format.stride + format.matrix_offset(), 4);
        words = word == (0u | 60u << 8 | 60u << 16 | 64u << 24);
    }
    check(format.matrices && words, "each vertex names its normal matrix by its full-form row");
}
// The fragment or vertex source of the last batched program compiled since sources.
std::string last_batched_fragment(std::size_t sources, bool vertex = false) {
    std::string found;
    for (auto i = sources; i < fake::shader_sources.size(); ++i)
        if (fake::shader_sources[i].find(vertex ? "flat out mediump vec4 gx_register0" : "flat in mediump vec4 gx_register0") != std::string::npos)
            found = fake::shader_sources[i];
    return found;
}
void direct_batched_zero_alpha() {
    GlesRenderer renderer(batching_limits()); Fixture f;
    const auto draw = [&] { renderer.draw_direct(f.decoder.state(), f.command(), f.memory); };
    auto sources = fake::shader_sources.size();
    f.bp(0xf3, (4u << 16) | (7u << 19)); // Alpha greater than 0, the fixture's source-alpha blend without depth or alpha writes.
    draw(); f.f(3, 10.0f); draw(); renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    auto fragment = last_batched_fragment(sources);
    check(s.zero_alpha_draws == 2 && s.batched_draws == 2 && s.submissions == 1, "alpha-tested source-alpha draws batch with zero alpha");
    check(fragment.find(")) color.a=0.0;") != std::string::npos && fragment.find("discard") == std::string::npos,
          "their alpha test keeps failing fragments with alpha 0 instead of discarding them");
    f.bp(0x41, 0xf429); // Destination factor one.
    draw(); renderer.wait_until_idle();
    check(renderer.stats().direct.zero_alpha_draws == s.zero_alpha_draws + 1, "additive source-alpha blending keeps zero alpha");
    const auto discards = [&](const char* message) {
        const auto before = renderer.stats().direct;
        sources = fake::shader_sources.size();
        draw(); renderer.wait_until_idle();
        const auto after = renderer.stats().direct;
        fragment = last_batched_fragment(sources);
        check(after.batched_draws == before.batched_draws + 1 && after.zero_alpha_draws == before.zero_alpha_draws &&
              (fragment.empty() || fragment.find(")) discard;") != std::string::npos), message);
    };
    f.bp(0x41, 0xf4a9); f.bp(0x40, 0x17); // Depth tested and written.
    discards("a depth-writing draw discards");
    f.bp(0x40, 0x16); // Written without the test: GL writes no depth.
    draw(); renderer.wait_until_idle();
    check(renderer.stats().direct.zero_alpha_draws == s.zero_alpha_draws + 2, "depth writes without the depth test keep zero alpha");
    f.bp(0x40, 0x06);
    f.bp(0x41, 0xf1a9); discards("a source factor other than source alpha discards");
    f.bp(0x41, 0xf469); discards("a destination factor reading the source colour discards");
    f.bp(0x41, 0xfca9); discards("subtraction discards");
    f.bp(0x41, 0xf4a9); f.bp(0x43, 1); f.bp(0x41, 0xf4b9); discards("alpha writes discard");
    f.bp(0x41, 0xf4a9); draw(); renderer.wait_until_idle();
    check(renderer.stats().direct.zero_alpha_draws == s.zero_alpha_draws + 3, "an alpha EFB without alpha writes keeps zero alpha");
    f.bp(0x43, 0); f.bp(0xf3, 0x3f0000);
    const auto plain = renderer.stats().direct.zero_alpha_draws;
    draw(); renderer.wait_until_idle();
    check(renderer.stats().direct.zero_alpha_draws == plain, "draws without an alpha test take no variant");
}
void direct_batched_vertex_fog() {
    auto limits = batching_limits(); limits.direct.vertex_fog = true;
    GlesRenderer renderer(limits); Fixture f;
    const auto draw = [&] { renderer.draw_direct(f.decoder.state(), f.command(), f.memory); };
    const auto has = [](const std::string& source, const char* text) { return source.find(text) != std::string::npos; };
    auto sources = fake::shader_sources.size();
    f.bp(0xf1, 4u << 21); // Exponential fog; every fixture vertex has eye depth -0.5.
    draw(); f.f(3, 10.0f); draw(); renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    check(s.vertex_fog_draws == 2 && s.batched_draws == 2 && s.submissions == 1, "batched draws of one depth fog per vertex and merge");
    auto vertex = last_batched_fragment(sources, true), fragment = last_batched_fragment(sources);
    check(has(vertex, "flat out highp float gx_fog;") && has(vertex, "gl_DepthRange") && has(vertex, "gx_fog=1.0-exp2(-8.0*fogX);") &&
          has(vertex, "uGxFog.x/max(uGxFog.y-gxFogZ*uGxFog.z,1e-9)"), "the vertex evaluates the fog factor at its window depth");
    check(has(fragment, "color.rgb=mix(color.rgb,uGxFogColor,gx_fog);") && !has(fragment, "uGxFog;") && !has(fragment, "gl_FragCoord.z"),
          "the fragment mixes with the flat factor");
    const auto per_fragment = [&](const char* message) {
        const auto before = renderer.stats().direct;
        sources = fake::shader_sources.size();
        draw(); renderer.wait_until_idle();
        const auto after = renderer.stats().direct;
        fragment = last_batched_fragment(sources);
        check(after.batched_draws == before.batched_draws + 1 && after.vertex_fog_draws == before.vertex_fog_draws &&
              (fragment.empty() || has(fragment, "gl_FragCoord.z")), message);
    };
    f.vertices[2 * 32 + 8] ^= 0x01; // One vertex at another depth.
    per_fragment("a draw whose vertices differ in depth fogs per fragment");
    f.vertices[2 * 32 + 8] ^= 0x01;
    f.bp(0xe8, 1u << 10); per_fragment("range adjustment fogs per fragment");
    f.bp(0xe8, 0);
    f.bp(0xf3, (4u << 16) | (7u << 19));
    sources = fake::shader_sources.size();
    draw(); renderer.wait_until_idle();
    fragment = last_batched_fragment(sources);
    check(renderer.stats().direct.vertex_fog_draws == s.vertex_fog_draws + 1 && renderer.stats().direct.zero_alpha_draws == 1 &&
          has(fragment, "gx_fog") && has(fragment, "color.a=0.0"), "vertex fog and zero alpha combine");

    GlesRenderer plain(batching_limits()); Fixture g; g.bp(0xf1, 4u << 21);
    plain.draw_direct(g.decoder.state(), g.command(), g.memory); plain.wait_until_idle();
    check(plain.stats().direct.batched_draws == 1 && plain.stats().direct.vertex_fog_draws == 0, "vertex fog is opt-in");
    check(bool(plan_direct_program(f.decoder.state(), f.command().layout, FramebufferFetch::none, DirectShading::fast, true, 0, false,
                                   batched_vertex_fog)), "the variant plans with fog");
    f.bp(0xf1, 0);
    check(!plan_direct_program(f.decoder.state(), f.command().layout, FramebufferFetch::none, DirectShading::fast, true, 0, false,
                               batched_vertex_fog), "the variant needs fog");
}
void direct_clip_culling() {
    for (const auto shading : {DirectShading::exact, DirectShading::fast}) {
        GlesLimits limits; limits.direct.shading = shading;
        GlesRenderer renderer(limits); Fixture f; const auto command = f.command();
        renderer.draw_direct(f.decoder.state(), command, f.memory);
        renderer.wait_until_idle();
        auto s = renderer.stats().direct;
        check(s.culled_draws == 0 && s.submissions == 1, "visible draws are submitted");
        const auto bytes = s.vertex_bytes;
        const auto plans = s.state_plans;
        f.f(3, 2000.0f);
        renderer.draw_direct(f.decoder.state(), command, f.memory);
        renderer.wait_until_idle();
        s = renderer.stats().direct;
        check(s.culled_draws == 1 && s.submissions == 1 && s.vertex_bytes == bytes, "draws beyond a clip plane are dropped");
        check(s.state_plans == plans, "direct positions are culled before any state work");
        f.xf(0x1005, 1);
        renderer.draw_direct(f.decoder.state(), command, f.memory);
        renderer.wait_until_idle();
        check(renderer.stats().direct.culled_draws == 1, "clip-disabled draws are never culled");
    }
}
// A scissor reaching past the viewport is a guard-band rejection, unless the draw's
// screen bounds miss the scissor: then no pixel can be written and it is dropped silently.
void direct_guard_band_discard() {
    // The fixture quad lands on EFB x 251..314, y 200..222 inside a 200x100 viewport at 220,178.
    const auto setup = [](Fixture& f, int left, int top, int right, int bottom) {
        f.f(0x101a, 100); f.f(0x101b, -50);
        f.bp(0x20, std::uint32_t(left + 342) << 12 | std::uint32_t(top + 342));
        f.bp(0x21, std::uint32_t(right + 342) << 12 | std::uint32_t(bottom + 342));
    };
    const auto rejected = [](GlesRenderer& renderer, Fixture& f) {
        try { renderer.draw_direct(f.decoder.state(), f.command(), f.memory); }
        catch (const std::runtime_error& e) { return std::string(e.what()).find("guard-band") != std::string::npos; }
        return false;
    };
    const int misses[4][4] = {{0, 0, 240, 455}, {320, 0, 639, 455}, {0, 0, 639, 190}, {0, 230, 639, 455}};
    for (const auto shading : {DirectShading::exact, DirectShading::fast}) {
        GlesLimits limits; limits.direct.shading = shading;
        for (const auto& box : misses) {
            GlesRenderer renderer(limits); Fixture f; setup(f, box[0], box[1], box[2], box[3]);
            renderer.draw_direct(f.decoder.state(), f.command(), f.memory);
            renderer.wait_until_idle();
            const auto s = renderer.stats().direct;
            check(s.culled_draws == 1 && s.submissions == 0 && !renderer.failed(), "draws missing the scissor are dropped silently");
        }
        GlesRenderer renderer(limits); Fixture f; setup(f, 0, 0, 639, 455);
        check(rejected(renderer, f), "draws overlapping a guard-band scissor are still rejected");
        GlesRenderer behind(limits); Fixture g; setup(g, 0, 0, 240, 455);
        g.xf(0x1005, 1); g.xf(0x1026, 0);
        for (unsigned i = 0; i < 4; ++i) {
            const float z = .5f; std::uint32_t bits; std::memcpy(&bits, &z, 4);
            for (unsigned b = 0; b < 4; ++b) g.vertices[i * 32 + 8 + b] = std::uint8_t(bits >> (24 - b * 8));
        }
        check(rejected(behind, g), "draws with W <= 0 are still rejected");
    }
}
void direct_indexed_clip_culling() {
    // Particle quads index a shared corner array and move through the position matrix.
    struct Positions : Memory {
        std::vector<std::uint8_t> corners;
        MemoryArrayView array_view(std::uint32_t address) override {
            if (address != 0x9000) return {};
            return {{corners.data(), corners.size()}, 0, 1};
        }
    };
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits); Fixture f;
    Positions memory;
    const float corners[4][3] = {{100, 100, -.5f}, {300, 100, -.5f}, {300, 200, -.5f}, {100, 200, -.5f}};
    for (const auto& corner : corners) for (float value : corner) append_float(memory.corners, value);
    f.cp(0x50, (2u << 9) | (1u << 13));
    f.cp(0xa0, 0x9000); f.cp(0xb0, 12);
    std::vector<std::uint8_t> records;
    for (unsigned i = 0; i < 4; ++i) {
        records.push_back(static_cast<std::uint8_t>(i));
        records.insert(records.end(), f.vertices.begin() + i * 32 + 12, f.vertices.begin() + i * 32 + 32);
    }
    auto command = f.command();
    command.vertices = {records.data(), records.size()};
    check(command.layout.stride == 21, "indexed position layout");
    renderer.draw_direct(f.decoder.state(), command, memory);
    renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    check(s.culled_draws == 0 && s.submissions == 1, "visible indexed draws are submitted");
    const auto plans = s.state_plans;
    f.f(3, 2000.0f);
    renderer.draw_direct(f.decoder.state(), command, memory);
    renderer.wait_until_idle();
    s = renderer.stats().direct;
    check(s.culled_draws == 1 && s.state_plans == plans, "indexed positions are culled before any state work");

    // Signed bytes with one fraction bit, as particle corner arrays use.
    GlesRenderer bytes_renderer(limits); Fixture g;
    Positions small;
    const std::int8_t halves[4][3] = {{20, 20, -1}, {120, 20, -1}, {120, 120, -1}, {20, 120, -1}};
    for (const auto& corner : halves) for (auto value : corner) small.corners.push_back(static_cast<std::uint8_t>(value));
    g.cp(0x50, (2u << 9) | (1u << 13));
    g.cp(0x70, 1u | (1u << 1) | (1u << 4) | (1u << 13) | (5u << 14) | (1u << 21) | (4u << 22) | (1u << 30));
    g.cp(0xa0, 0x9000); g.cp(0xb0, 3);
    auto byte_command = g.command();
    byte_command.vertices = {records.data(), records.size()};
    bytes_renderer.draw_direct(g.decoder.state(), byte_command, small);
    bytes_renderer.wait_until_idle();
    s = bytes_renderer.stats().direct;
    check(s.culled_draws == 0 && s.submissions == 1, "visible byte positions are submitted");
    g.f(3, 2000.0f);
    bytes_renderer.draw_direct(g.decoder.state(), byte_command, small);
    bytes_renderer.wait_until_idle();
    check(bytes_renderer.stats().direct.culled_draws == 1, "byte positions are dequantized and culled early");
}
void direct_submission_thread() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits); Fixture f; const auto command = f.command();
    const auto before = fake::draws;
    std::thread::id gl_thread;
    renderer.start_submission([] {}, [&] { gl_thread = std::this_thread::get_id(); }, [] {});
    for (unsigned i = 0; i < 3; ++i) {
        f.f(3, 10.0f * i);
        renderer.draw_direct(f.decoder.state(), command, f.memory);
        renderer.wait_until_idle();
    }
    TextureCopy p; p.width = 8; p.height = 4; p.format = 1; p.luminance = true; p.stride = 32;
    alignas(32) std::array<std::uint8_t, 64> bytes{};
    renderer.copy_texture_resident(p, bytes.data(), bytes.size(), 0x8000);
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    const auto depth = renderer.peek_depth(10, 10);
    renderer.stop_submission();
    check(gl_thread != std::thread::id{} && gl_thread != std::this_thread::get_id(), "GL work ran on the submission thread");
    check(depth <= 0xffffff, "depth peeks return through the submission thread");
    const auto s = renderer.stats().direct;
    check(s.draws == 4 && s.submissions >= 2 && fake::draws > before, "recorded draws are submitted in order");
    check(s.resident_copies == 1, "resident copies are created on the submission thread");
}
void direct_texture_copy_readback() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits); Fixture f; const auto command = f.command();
    std::thread::id gl_thread;
    renderer.start_submission([] {}, [&] { gl_thread = std::this_thread::get_id(); }, [] {});
    const auto before = fake::draws;
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    TextureCopy p; p.width = 8; p.height = 4; p.format = 1; p.luminance = true; p.stride = 32;
    alignas(32) std::array<std::uint8_t, 64> bytes{};
    const auto reads = fake::read_pixels;
    renderer.copy_texture(p, bytes.data(), bytes.size());
    renderer.stop_submission();
    check(fake::read_pixels > reads && fake::read_thread == gl_thread, "direct texture copies read back on the submission thread");
    check(fake::draws_at_read > before, "direct texture copies read back after the draws recorded before them");
}
// A host worker that already runs GL; tasks run in the order they are posted.
struct HostThread {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::thread thread{[this] {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return stopping || !tasks.empty(); });
                if (tasks.empty()) return;
                task = std::move(tasks.front()); tasks.pop_front();
            }
            task();
        }
    }};
    void post(std::function<void()> task) {
        { std::lock_guard<std::mutex> lock(mutex); tasks.push_back(std::move(task)); }
        ready.notify_one();
    }
    void run(const std::function<void()>& task) {
        std::promise<void> done;
        post([&] { task(); done.set_value(); });
        done.get_future().get();
    }
    ~HostThread() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        ready.notify_one(); thread.join();
    }
};
void host_thread_submission() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    HostThread host; std::unique_ptr<GlesRenderer> renderer; std::thread::id gl_thread;
    host.run([&] {
        renderer = std::make_unique<GlesRenderer>(limits);
        renderer->start_submission_on([&host](std::function<void()> task) { host.post(std::move(task)); });
        gl_thread = std::this_thread::get_id();
    });
    renderer->adopt_thread();
    Fixture f; const auto command = f.command(); const auto before = fake::draws;
    for (unsigned i = 0; i < 3; ++i) {
        f.f(3, 10.0f * i);
        renderer->draw_direct(f.decoder.state(), command, f.memory);
        renderer->end_frame();
    }
    bool presented = false;
    // Waited for: stop_submission waits only for submitted frame work, and present_into
    // throws on the host thread once submission has stopped.
    host.run([&] { renderer->present_into(0, 608, 448, 0, 0, 640, 480); presented = std::this_thread::get_id() == gl_thread; });
    rejects([&] { renderer->present_into(0, 640, 456, 0, 0, 640, 480); }, "present_into refuses the recording thread");
    renderer->stop_submission();
    host.run([] {});
    check(presented, "present_into runs on the host thread after the frame's work");
    const auto s = renderer->stats().direct;
    check(s.draws == 3 && fake::draws > before, "draws recorded here are submitted on the host thread");
}
void shared_depth_snapshots() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits); Fixture f; const auto command = f.command();
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    const int first = renderer.snapshot_efb();
    check(first >= 0 && renderer.snapshot_efb() == first, "an unchanged EFB shares its snapshot");
    renderer.release_snapshot(first);
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    const int second = renderer.snapshot_efb();
    check(second >= 0 && second != first, "a shared snapshot stays held until every holder releases it");
    renderer.release_snapshot(first);
    check(renderer.snapshot_efb() == second, "draws end sharing only for later snapshots");
    renderer.release_snapshots();
    check(renderer.snapshot_efb() == first, "released slots are reused");
}
void snapshot_layout() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits); Fixture f; const auto command = f.command();
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    const int index = renderer.snapshot_efb();
    check(index >= 0 && fake::pack_required == 320 * 114 * 4, "snapshots read back quarter-resolution depth and colour");
    fake::map_pattern = true;
    renderer.select_snapshot(index);
    // Byte offset of EFB (x, y) in the readback: bottom-up rows of 160 depth then 160 colour texels.
    const auto at = [](int x, int y, bool color) { return std::uint32_t(((455 - y) / 4 * 320 + (color ? 160 : 0) + x / 4) * 4); };
    const auto byte = [](std::uint32_t offset) { return offset % 251; };
    const auto c = at(13, 7, true), d = at(13, 7, false), corner = at(639, 0, true);
    check(renderer.peek_color(13, 7) == (byte(c + 3) << 24 | byte(c) << 16 | byte(c + 1) << 8 | byte(c + 2)),
          "colour peeks read the second half of the row as ARGB");
    check(renderer.peek_depth(13, 7) == (byte(d) << 16 | byte(d + 1) << 8 | byte(d + 2)), "depth peeks read the first half");
    std::uint32_t value = 0;
    check(renderer.peek_color_snapshot(index, 639, 0, value) &&
          value == (byte(corner + 3) << 24 | byte(corner) << 16 | byte(corner + 1) << 8 | byte(corner + 2)),
          "the top-right pixel is the last texel of the readback");
    check(corner + 4 == 320 * 114 * 4, "the readback ends at the top-right colour texel");
    check(!renderer.peek_color_snapshot(index, 640, 0, value), "snapshot peeks outside the EFB decline");
    fake::map_pattern = false;
    renderer.release_snapshots();
    check(renderer.stats().live_peeks == 0 && renderer.stats().peeks == 2, "selected snapshots answer renderer peeks");
}
void full_resolution_snapshots() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast; limits.direct.snapshot_scale = 1;
    GlesRenderer renderer(limits); Fixture f; const auto command = f.command();
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    const int index = renderer.snapshot_efb();
    check(index >= 0 && fake::pack_required == 1280 * 456 * 4, "snapshot_scale 1 reads back every EFB pixel under fast shading");
    fake::map_pattern = true;
    // Bottom-up rows of 640 depth then 640 colour texels; neighbours are distinct samples.
    const auto at = [](int x, int y) { return std::uint32_t(((455 - y) * 1280 + 640 + x) * 4); };
    const auto byte = [](std::uint32_t offset) { return offset % 251; };
    renderer.select_snapshot(index);
    const std::uint32_t waited = renderer.peek_color(13, 7);
    std::uint32_t a = 0, b = 0;
    check(renderer.peek_color_snapshot(index, 13, 7, a) && renderer.peek_color_snapshot(index, 14, 7, b) && a != b && a == waited &&
          a == (byte(at(13, 7) + 3) << 24 | byte(at(13, 7)) << 16 | byte(at(13, 7) + 1) << 8 | byte(at(13, 7) + 2)),
          "full-resolution snapshots answer each pixel exactly");
    fake::map_pattern = false;
    renderer.release_snapshots();
    bool refused = false;
    GlesLimits bad; bad.direct.snapshot_scale = 3;
    try { GlesRenderer invalid(bad); } catch (const std::invalid_argument&) { refused = true; }
    check(refused, "snapshot scales other than 1, 2 and 4 are refused");
}
// Panfrost reads back through compute passes into buffers, at the offsets glReadPixels uses.
void panfrost_readback() {
    const char* name = fake::renderer_name;
    fake::renderer_name = "Mali-G31 (Panfrost)";
    {
        GlesLimits limits; limits.direct.shading = DirectShading::fast; limits.direct.snapshot_scale = 1;
        GlesRenderer renderer(limits); Fixture f; const auto command = f.command();
        renderer.draw_direct(f.decoder.state(), command, f.memory);
        auto reads = fake::read_pixels, dispatches = fake::dispatches;
        const int index = renderer.snapshot_efb(EfbRegion{100, 200, 16, 8});
        check(index >= 0 && fake::read_pixels == reads && fake::dispatches == dispatches + 2 &&
              fake::pack_required == ((455 - 200) * 1280 + 640 + 100 + 15) * 4 + 4,
              "a region snapshot is stored by two compute passes, one per block");
        renderer.wait_snapshot(index);
        check(renderer.snapshot_ready(index) && fake::read_pixels == reads, "its readback is collected without glReadPixels");
        renderer.release_snapshots();
    }
    {
        GlesRenderer renderer; Fixture f; f.memory.bytes.resize(64,0xa5);
        renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
        TextureCopy p; p.width=8; p.height=4; p.format=1; p.luminance=true; p.stride=32;
        alignas(32) std::array<std::uint8_t,64> bytes{};
        renderer.copy_texture_resident(p,bytes.data(),bytes.size(),0x8000);
        f.bp(0x8b,0x400c07);
        const auto reads=fake::read_pixels, dispatches=fake::dispatches;
        renderer.draw_direct(f.decoder.state(),f.command(),f.memory); renderer.wait_until_idle();
        check(renderer.stats().direct.materialized==1 && fake::read_pixels==reads && fake::dispatches==dispatches+1 &&
              fake::pack_required==8*4*4, "a materialized copy is stored by one compute pass");
    }
    fake::renderer_name = name;
}
void region_snapshots() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast; limits.direct.snapshot_scale = 1;
    GlesRenderer renderer(limits); Fixture f; const auto command = f.command();
    renderer.draw_direct(f.decoder.state(), command, f.memory);
    const EfbRegion region{100, 200, 16, 8};
    const int index = renderer.snapshot_efb(region);
    check(index >= 0 && fake::pack_required == ((455 - 200) * 1280 + 640 + 100 + 15) * 4 + 4,
          "a region snapshot transfers only the region's rows and columns");
    fake::map_pattern = true; fake::read_pattern_row = 1280;
    // What a whole snapshot answers (full_resolution_snapshots): bottom-up rows of 640
    // depth then 640 colour texels.
    const auto whole = [](int x, int y) {
        const auto at = std::uint32_t(((455 - y) * 1280 + 640 + x) * 4);
        const auto byte = [&](std::uint32_t k) { return std::uint32_t((at + k) % 251); };
        return byte(3) << 24 | byte(0) << 16 | byte(1) << 8 | byte(2);
    };
    const auto reads = fake::read_pixels;
    const auto misses = renderer.stats().region_misses;
    renderer.wait_snapshot(index);
    check(renderer.snapshot_ready(index) && fake::read_pixels == reads && renderer.stats().region_misses == misses,
          "waiting for a snapshot collects its readback without reading a pixel");
    const int second = renderer.snapshot_efb(EfbRegion{300, 100, 4, 4});
    renderer.select_snapshot(second);
    check(renderer.peek_color(3, 455) == whole(3, 455) && renderer.snapshot_ready(second),
          "a first peek outside the region still collects the region's readback");
    renderer.release_snapshot(second);
    renderer.select_snapshot(index);
    check(renderer.peek_color(107, 203) == whole(107, 203) && renderer.peek_color(115, 207) == whole(115, 207) &&
          renderer.stats().region_misses == misses + 1,
          "peeks inside the region read the readback, as a whole snapshot would");
    std::uint32_t inside = 0, outside = 0;
    check(renderer.peek_color_snapshot(index, 100, 200, inside) && inside == whole(100, 200),
          "the any-thread peek answers inside the region");
    check(!renderer.peek_color_snapshot(index, 116, 203, outside) && !renderer.peek_color_snapshot(index, 107, 208, outside),
          "the any-thread peek declines outside the region");
    check(renderer.peek_color(140, 230) == whole(140, 230) && renderer.peek_color(3, 455) == whole(3, 455) &&
          renderer.peek_color(116, 199) == whole(116, 199) && renderer.stats().region_misses == misses + 4,
          "a peek outside the region reads the retained snapshot exactly");
    fake::map_pattern = false; fake::read_pattern_row = 0;
    renderer.release_snapshots();
}
void host_bloom() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits);
    bool rejected = false;
    try { renderer.end_bloom({}); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "bloom end requires a begin");
    const auto attached = fake::color_attachments;
    const auto clears = fake::clears, draws = fake::draws;
    renderer.begin_bloom();
    rejected = false;
    try { renderer.begin_bloom(); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "blooms do not nest");
    bool swapped = false;
    for (const auto& [framebuffer, texture] : attached) {
        if (!texture) continue;
        const auto now = fake::color_attachments[framebuffer];
        if (now != texture) swapped = true;
    }
    check(swapped && fake::clears == clears + 1 && fake::draws == draws, "bloom begin swaps in a cleared colour without drawing");
    EfbBloom bloom; bloom.intensity = 128; bloom.level1_weight = 42; bloom.level2_weight = 21;
    renderer.end_bloom(bloom);
    for (const auto& [framebuffer, texture] : attached)
        check(fake::color_attachments[framebuffer] == texture, "bloom end restores the scene colour");
    check(fake::draws == draws + 4 && renderer.stats().blooms == 1, "bloom draws three levels and one additive pass");
}
void offscreen_pass() {
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits);
    bool rejected = false;
    try { renderer.end_offscreen(); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "offscreen end requires a begin");
    const auto colors = fake::color_attachments;
    const auto depths = fake::depth_attachments;
    const auto clears = fake::clears;
    renderer.begin_offscreen();
    rejected = false;
    try { renderer.begin_offscreen(); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "offscreen passes do not nest");
    rejected = false;
    try { renderer.begin_bloom(); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "native bloom does not start inside an offscreen pass");
    bool color_swapped = false, depth_swapped = false;
    for (const auto& [framebuffer, texture] : colors) if (texture && fake::color_attachments[framebuffer] != texture) color_swapped = true;
    for (const auto& [framebuffer, texture] : depths) if (texture && fake::depth_attachments[framebuffer] != texture) depth_swapped = true;
    check(color_swapped && depth_swapped && fake::clears == clears + 1, "offscreen begin attaches a cleared colour and depth");
    renderer.end_offscreen();
    for (const auto& [framebuffer, texture] : colors) check(fake::color_attachments[framebuffer] == texture, "offscreen end restores the EFB colour");
    for (const auto& [framebuffer, texture] : depths) check(fake::depth_attachments[framebuffer] == texture, "offscreen end restores the EFB depth");
}
void direct_geometry_cache() {
    struct Indexed : test::Memory {
        std::vector<std::uint8_t> positions;
        MemoryArrayView array_view(std::uint32_t address) override {
            if (address != 0x9000) return {};
            return {{positions.data(), positions.size()}, 0};
        }
    };
    Fixture f; Indexed memory; memory.bytes = f.memory.bytes;
    for (unsigned i = 0; i < 8; ++i) for (float c : {100.0f + 25 * i, 100.0f + 10 * i, -0.5f}) append_float(memory.positions, c);
    f.cp(0x50, (2u << 9) | (1u << 13)); f.cp(0xa0, 0x9000); f.cp(0xb0, 12);
    std::vector<std::uint8_t> fifo;
    for (unsigned i = 0; i < 8; ++i) {
        fifo.push_back(static_cast<std::uint8_t>(i));
        fifo.insert(fifo.end(), {255, 32, 64, 255});
        append_float(fifo, .25f); append_float(fifo, .75f); append_float(fifo, i & 1 ? 1 : 0); append_float(fifo, i & 2 ? 1 : 0);
    }
    Draw draw; draw.primitive = 0x80; draw.vertex_count = 8;
    std::string error; check(f.decoder.state().vertex_layout(0, draw.layout, error), error.c_str());
    draw.vertices = {fifo.data(), fifo.size()};
    GlesRenderer renderer;
    renderer.draw_direct(f.decoder.state(), draw, memory);
    renderer.draw_direct(f.decoder.state(), draw, memory); renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    check(s.geometry_misses == 1 && s.geometry_hits == 1 && s.geometries == 1, "unchanged indexed geometry is reused");
    check(s.vertex_bytes == 0, "cached geometry draws from resident buffers without streaming");
    memory.positions[0] ^= 1;
    renderer.draw_direct(f.decoder.state(), draw, memory); renderer.wait_until_idle();
    s = renderer.stats().direct;
    check(s.geometry_misses == 2 && s.geometry_hits == 1, "a changed consumed array span forces reconversion");
    fifo[0] = 1;
    renderer.draw_direct(f.decoder.state(), draw, memory); renderer.wait_until_idle();
    check(renderer.stats().direct.geometry_misses == 3, "changed FIFO bytes never hit");
    // Enough settled entries to be packed into an arena at a frame boundary.
    std::vector<std::vector<std::uint8_t>> variants;
    for (unsigned v = 0; v < 70; ++v) { variants.push_back(fifo); append_float(variants.back(), 0); std::memcpy(variants.back().data() + 5, &v, 4); variants.back().pop_back(); variants.back().pop_back(); variants.back().pop_back(); variants.back().pop_back(); }
    auto draw_all = [&] {
        for (auto& bytes : variants) { Draw d = draw; d.vertices = {bytes.data(), bytes.size()}; renderer.draw_direct(f.decoder.state(), d, memory); }
        renderer.wait_until_idle();
    };
    draw_all();
    const auto misses = renderer.stats().direct.geometry_misses, hits = renderer.stats().direct.geometry_hits;
    renderer.present(640, 480); renderer.present(640, 480);
    draw_all();
    s = renderer.stats().direct;
    check(s.geometry_misses == misses && s.geometry_hits == hits + 70, "packed geometry keeps hitting after an arena move");
}
// Resident draws of one geometry that change only their position matrix become one
// instanced draw whose per-instance rows are each draw's own matrix, bit for bit.
void direct_instancing() {
    struct Indexed : test::Memory {
        std::vector<std::uint8_t> positions;
        MemoryArrayView array_view(std::uint32_t address) override {
            if (address != 0x9000) return {};
            return {{positions.data(), positions.size()}, 0};
        }
    };
    Fixture f; Indexed memory; memory.bytes = f.memory.bytes;
    for (unsigned i = 0; i < 8; ++i) for (float c : {100.0f + 25 * i, 100.0f + 10 * i, -0.5f}) append_float(memory.positions, c);
    f.cp(0x50, (2u << 9) | (1u << 13)); f.cp(0xa0, 0x9000); f.cp(0xb0, 12);
    // More vertices than draws that are batched instead take.
    std::vector<std::uint8_t> fifo;
    for (unsigned i = 0; i < 68; ++i) {
        fifo.push_back(static_cast<std::uint8_t>(i % 8));
        fifo.insert(fifo.end(), {255, 32, 64, 255});
        append_float(fifo, .25f); append_float(fifo, .75f); append_float(fifo, i & 1 ? 1 : 0); append_float(fifo, i & 2 ? 1 : 0);
    }
    Draw draw; draw.primitive = 0x80; draw.vertex_count = 68;
    std::string error; check(f.decoder.state().vertex_layout(0, draw.layout, error), error.c_str());
    draw.vertices = {fifo.data(), fifo.size()};
    GlesLimits limits; limits.direct.shading = DirectShading::fast;
    GlesRenderer renderer(limits);
    const float shifts[] = {0.0f, 12.5f, -40.25f};
    const auto draws = fake::draws, instanced = fake::instanced_draws;
    for (float shift : shifts) { f.f(3, shift); renderer.draw_direct(f.decoder.state(), draw, memory); }
    renderer.wait_until_idle();
    auto s = renderer.stats().direct;
    check(s.draws == 3 && s.instanced_draws == 2 && s.submissions == 1 && s.uniform_blocks == 1,
          "matrix-only changes on resident geometry join one instanced draw with one shared block");
    check(fake::draws == draws + 1 && fake::instanced_draws == instanced + 1 && fake::instance_count == 3,
          "one instanced driver draw of three instances");
    check(fake::instance_size == 3 * 6 * 16 && std::size_t(fake::instance_offset) + std::size_t(fake::instance_size) <= fake::uniform_bytes.size(),
          "the instance rows are bound at binding 1");
    bool rows = true;
    for (unsigned i = 0; i < 3; ++i) {
        float row[12];
        std::memcpy(row, fake::uniform_bytes.data() + fake::instance_offset + i * 96, sizeof row);
        rows = rows && row[0] == 1 && row[1] == 0 && row[2] == 0 && row[3] == shifts[i] && row[5] == 1 && row[10] == 1;
    }
    check(rows, "each instance carries its own position matrix rows");
    check(std::any_of(fake::shader_sources.begin(), fake::shader_sources.end(),
                      [](const std::string& v) { return v.find("u_instance[ib]") != std::string::npos; }),
          "the instanced variant reads the position matrix per instance");
    // Rewriting unchanged state between draws keeps the run.
    renderer.present(640, 480);
    const auto before = renderer.stats().direct;
    for (float shift : shifts) { f.bp(0x41, 0xf4a9); f.f(3, shift); renderer.draw_direct(f.decoder.state(), draw, memory); }
    renderer.wait_until_idle();
    s = renderer.stats().direct;
    check(s.instanced_draws == before.instanced_draws + 2 && s.submissions == before.submissions + 1,
          "identical state written again still joins the instance run");
    // A texture or state change, or a different geometry, starts a separate draw.
    f.f(3, 7.0f); renderer.draw_direct(f.decoder.state(), draw, memory);
    f.bp(0x41, 0xf118); f.f(3, 8.0f); renderer.draw_direct(f.decoder.state(), draw, memory); renderer.wait_until_idle();
    s = renderer.stats().direct;
    check(s.draws == before.draws + 5 && s.instanced_draws == before.instanced_draws + 2 && s.submissions == before.submissions + 3,
          "a state change ends the instance run");
    // An evicted instanced variant is planned again, not reused.
    GlesLimits small = limits; small.direct.programs = 2;
    GlesRenderer evicting(small);
    f.bp(0x41, 0xf4a9);
    auto instance_pair = [&] {
        for (float shift : {1.0f, 2.0f}) { f.f(3, shift); evicting.draw_direct(f.decoder.state(), draw, memory); }
        evicting.wait_until_idle(); evicting.present(640, 480);
    };
    instance_pair();
    // The base program is used again alone, then a third program evicts the variant.
    f.f(3, 5.0f); evicting.draw_direct(f.decoder.state(), draw, memory); evicting.wait_until_idle(); evicting.present(640, 480);
    const auto first = evicting.stats().direct;
    f.bp(0xc0, 0x8f8a0); evicting.draw_direct(f.decoder.state(), draw, memory); evicting.wait_until_idle(); evicting.present(640, 480);
    f.bp(0xc0, 0x8f8af);
    instance_pair();
    const auto again = evicting.stats().direct;
    check(again.instanced_draws == first.instanced_draws + 1 && again.program_misses == first.program_misses + 2,
          "an evicted instanced variant is planned again");
}
// A reader that reports write generations per byte range lets unchanged spans skip the
// byte comparison; a new generation makes the next draw compare and reconvert.
void direct_range_generations() {
    struct Tracked : test::Memory {
        std::vector<std::uint8_t> positions;
        std::uint64_t generation = 1;
        MemoryArrayView array_view(std::uint32_t address) override {
            if (address != 0x9000) return {};
            return {{positions.data(), positions.size()}, 0};
        }
        std::uint64_t range_generation(const MemoryArrayView&, std::uint32_t address, std::size_t size) override {
            return address >= 0x9000 && address + size <= 0x9000 + positions.size() ? generation : 0;
        }
    };
    Fixture f; Tracked memory; memory.bytes = f.memory.bytes;
    for (unsigned i = 0; i < 8; ++i) for (float c : {100.0f + 25 * i, 100.0f + 10 * i, -0.5f}) append_float(memory.positions, c);
    f.cp(0x50, (2u << 9) | (1u << 13)); f.cp(0xa0, 0x9000); f.cp(0xb0, 12);
    std::vector<std::uint8_t> fifo;
    for (unsigned i = 0; i < 8; ++i) {
        fifo.push_back(static_cast<std::uint8_t>(i));
        fifo.insert(fifo.end(), {255, 32, 64, 255});
        append_float(fifo, .25f); append_float(fifo, .75f); append_float(fifo, i & 1 ? 1 : 0); append_float(fifo, i & 2 ? 1 : 0);
    }
    Draw draw; draw.primitive = 0x80; draw.vertex_count = 8;
    std::string error; check(f.decoder.state().vertex_layout(0, draw.layout, error), error.c_str());
    draw.vertices = {fifo.data(), fifo.size()};
    GlesRenderer renderer;
    renderer.draw_direct(f.decoder.state(), draw, memory); renderer.wait_until_idle();
    const auto validated = renderer.stats().direct.geometry_validated_bytes;
    for (unsigned frame = 0; frame < 8; ++frame) {
        renderer.present(640, 480);
        renderer.draw_direct(f.decoder.state(), draw, memory); renderer.wait_until_idle();
    }
    auto s = renderer.stats().direct;
    check(s.geometry_misses == 1 && s.geometry_hits == 8, "geometry with unchanged range generations is reused");
    // At most one periodic full comparison in eight frames.
    check(s.geometry_validated_bytes - validated <= memory.positions.size(), "unchanged range generations skip comparing bytes");
    memory.positions[0] ^= 1; ++memory.generation;
    renderer.present(640, 480);
    renderer.draw_direct(f.decoder.state(), draw, memory); renderer.wait_until_idle();
    check(renderer.stats().direct.geometry_misses == 2, "a new range generation with changed bytes reconverts");
    ++memory.generation;
    renderer.present(640, 480);
    renderer.draw_direct(f.decoder.state(), draw, memory); renderer.wait_until_idle();
    s = renderer.stats().direct;
    check(s.geometry_misses == 2 && s.geometry_hits == 9, "a new range generation with the same bytes compares and hits");
}
// Display-list payloads named by their source: one list drawn from several vertex arrays
// keeps an entry per array, and a list whose arrays move every frame is streamed.
void direct_source_geometry() {
    struct Sourced : test::Memory {
        std::vector<std::uint8_t> arrays;
        MemoryArrayView array_view(std::uint32_t address) override {
            if (address < 0x9000 || address >= 0x9000 + arrays.size()) return {};
            return {{arrays.data() + (address - 0x9000), arrays.size() - (address - 0x9000)}, 0};
        }
        SourceIdentity source_identity(const void* data, std::size_t) override { return {data, 1}; }
    };
    Fixture f; Sourced memory; memory.bytes = f.memory.bytes;
    for (unsigned block = 0; block < 64; ++block)
        for (unsigned i = 0; i < 8; ++i) {
            for (float c : {100.0f + 25 * i + block, 100.0f + 10 * i, -0.5f}) append_float(memory.arrays, c);
            if (i == 7) memory.arrays.resize((block + 1) * 0x100u);
        }
    f.cp(0x50, (2u << 9) | (1u << 13)); f.cp(0xb0, 12);
    auto list = [](std::uint8_t tag) {
        std::vector<std::uint8_t> fifo;
        for (unsigned i = 0; i < 8; ++i) {
            fifo.push_back(static_cast<std::uint8_t>(i));
            fifo.insert(fifo.end(), {255, 32, 64, tag});
            append_float(fifo, .25f); append_float(fifo, .75f); append_float(fifo, i & 1 ? 1 : 0); append_float(fifo, i & 2 ? 1 : 0);
        }
        return fifo;
    };
    const auto shared = list(255), moving = list(254);
    Draw draw; draw.primitive = 0x80; draw.vertex_count = 8;
    std::string error; check(f.decoder.state().vertex_layout(0, draw.layout, error), error.c_str());
    GlesRenderer renderer;
    auto draw_at = [&](const std::vector<std::uint8_t>& fifo, std::uint32_t base) {
        f.cp(0xa0, base);
        Draw d = draw; d.vertices = {fifo.data(), fifo.size()};
        renderer.draw_direct(f.decoder.state(), d, memory);
    };
    for (unsigned frame = 0; frame < 4; ++frame) {
        for (unsigned instance = 0; instance < 3; ++instance) draw_at(shared, 0x9000 + instance * 0x100);
        renderer.wait_until_idle(); renderer.present(640, 480);
    }
    auto s = renderer.stats().direct;
    check(s.geometry_misses == 3 && s.geometry_hits == 9, "one list drawn from three arrays keeps an entry for each");
    for (unsigned frame = 0; frame < 6; ++frame) {
        draw_at(moving, 0x9400 + frame * 0x100);
        renderer.wait_until_idle(); renderer.present(640, 480);
    }
    const auto streamed = renderer.stats().direct;
    check(streamed.geometry_misses == s.geometry_misses + 3 && streamed.draws == s.draws + 6 &&
          streamed.vertex_bytes > s.vertex_bytes, "a list whose arrays move every frame streams after two unreused entries");
    for (unsigned frame = 0; frame < 64; ++frame) renderer.present(640, 480);
    draw_at(moving, 0x9000 + 40 * 0x100); renderer.wait_until_idle();
    check(renderer.stats().direct.geometry_misses == streamed.geometry_misses + 1, "a streamed source is cached again after 64 frames");
    draw_at(shared, 0x9000); renderer.wait_until_idle();
    check(renderer.stats().direct.geometry_hits == streamed.geometry_hits + 1, "other sources keep hitting");
}
int main() {
    explicit_completion();
    try {
        adjacent_draw_batches(); clip_disabled_draw_behind_eye(); batch_geometry_bound(); parallel_batch_preparation(); program_uniform_values(); program_working_set(); depth_query_transfer_bounds(); captured_texture_cache(); complete_mip_cache_budget(); resident_texture_cache();
        failed_upload_poison();
        inherited_transfer_state();
        fence_failure_and_thread_owner();
        validation_precedes_mutation();
        warm_shader_validation(); rgb8_replacement_boundary(); clear_boundary(); efb_layout_boundary();
        display_copy_boundary(); repeated_presentation();
        direct_draw_path(); live_depth_peeks_share_a_readback(); asynchronous_depth_readback();
        rejected_program_skips_draws();
        unready_program_policy();
        program_reuse_across_tev_colours();
        resident_copies(); resident_copy_crops(); resident_copy_chain(); dma_over_resident_copies(); stamped_resident_copies();
        z_texture_clear();
        direct_geometry_cache();
        direct_instancing();
        direct_range_generations();
        direct_source_geometry();
    format_after_depth_copy();
    copy_clear_invalidation();
        direct_batched_draws();
        direct_batched_normals();
        direct_batch_merging();
        direct_batched_materials();
        direct_uniform_blocks();
        direct_uniform_deltas();
        direct_state_plan_cache();
        direct_group_results();
        split_stream_geometry();
        shared_state_snapshots();
        direct_full_matrix_block();
        direct_batched_zero_alpha();
        direct_batched_vertex_fog();
        direct_clip_culling();
        direct_guard_band_discard();
        direct_indexed_clip_culling();
        direct_submission_thread(); direct_texture_copy_readback();
        host_thread_submission();
        host_bloom();
        offscreen_pass();
        shared_depth_snapshots();
        snapshot_layout();
        full_resolution_snapshots();
        region_snapshots(); panfrost_readback();
        texture_generations();
        present_mapping(); no_blits_on_the_frame_path();
        std::cout << "GLES dispatch boundary tests passed (not GPU rendering proof)\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
