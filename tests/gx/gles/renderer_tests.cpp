#include "starlet/gx/gles/renderer.hpp"
#include "draw_fixture.hpp"
#include <GLES3/gl31.h>
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
extern std::vector<unsigned char> submitted_vertices;
extern std::vector<std::uint16_t> submitted_indices;
extern unsigned submitted_index_count;
extern GLuint current_program;
extern std::map<std::pair<GLuint,std::string>,GLint> uniform_locations;
extern std::map<GLint,std::array<double,4>> uniform_values;
extern GLenum error, wait_result;
extern GLboolean links;
extern bool map_pattern;
extern unsigned clears, uploads, draws, draw_first, sampler_seen, pack_seen, unpack_seen, pack_pbo_seen, unpack_pbo_seen;
extern std::size_t pack_required, unpack_required;
extern std::map<GLuint, GLuint> color_attachments;
extern std::map<GLuint, GLuint> depth_attachments;
extern unsigned uniform_ranges, read_pixels;
extern GLsizeiptr element_bytes;
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
    check(fake::pack_seen==0&&fake::pack_pbo_seen==0&&fake::pack_required==4,"depth read resets pack state for exactly one RGBA pixel");
    fake::error=GL_INVALID_OPERATION;rejects([&]{renderer.peek_depth(0,0);},"driver depth failure reported");
    rejects([&]{renderer.peek_depth(0,0);},"failed depth renderer remains poisoned");
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
    f.bp(0x43,2);
    const auto draws=fake::draws;
    rejects([&]{renderer.draw_direct(f.decoder.state(),command,f.memory);},"unsupported pixel format rejected before submission");
    check(fake::draws==draws,"rejected direct draw issues no GL draw");
    f.bp(0x43,0);
    GlesLimits limits; limits.direct.verify_textures=true; GlesRenderer verified(limits); Fixture g; const auto other=g.command();
    verified.draw_direct(g.decoder.state(),other,g.memory);
    std::fill(g.memory.bytes.begin(),g.memory.bytes.end(),0x17);
    verified.draw_direct(g.decoder.state(),other,g.memory); verified.wait_until_idle();
    check(verified.stats().direct.texture_mutations==1 && verified.stats().direct.submissions==2 && fake::draw_first==0x17,
          "verification hashes every use and splits submissions on new content");
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
    host.post([&] { renderer->present_into(0, 608, 448, 0, 0, 640, 480); presented = std::this_thread::get_id() == gl_thread; });
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
int main() {
    explicit_completion();
    try {
        adjacent_draw_batches(); batch_geometry_bound(); parallel_batch_preparation(); program_uniform_values(); program_working_set(); depth_query_transfer_bounds(); captured_texture_cache(); complete_mip_cache_budget(); resident_texture_cache();
        failed_upload_poison();
        inherited_transfer_state();
        fence_failure_and_thread_owner();
        validation_precedes_mutation();
        warm_shader_validation(); rgb8_replacement_boundary(); clear_boundary(); efb_layout_boundary();
        display_copy_boundary(); repeated_presentation();
        direct_draw_path();
        rejected_program_skips_draws();
        program_reuse_across_tev_colours();
        resident_copies();
        direct_geometry_cache();
        direct_batched_draws();
        direct_clip_culling();
        direct_indexed_clip_culling();
        direct_submission_thread();
        host_thread_submission();
        host_bloom();
        offscreen_pass();
        shared_depth_snapshots();
        snapshot_layout();
        std::cout << "GLES dispatch boundary tests passed (not GPU rendering proof)\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
