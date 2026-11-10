#include "starlet/gx/depth.hpp"
#include "starlet/gx/decoder.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace starlet::gx;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct Memory:MemoryReader{bool copy(std::uint32_t,std::uint8_t*,std::size_t)override{return false;}};
struct Sink:CommandSink{bool draw(const State&,const Draw&,MemoryReader&)override{return true;}bool bp_write(const State&,std::uint8_t,std::uint32_t)override{return true;}bool invalidate_vertices()override{return true;}};
struct Fixture{Memory memory;Sink sink;Decoder decoder{memory,sink};void bp(unsigned reg,unsigned value){require(bool(decoder.write_u8(0x61)),"opcode");require(bool(decoder.write_u32((reg<<24)|value)),"payload");}};
void ordering(){
    // Expected predicates for incoming below/equal/above stored, in GX enum order.
    const unsigned truth[8]={0,1,2,3,4,5,6,7};
    unsigned checks=0;
    for(unsigned format:{0u,1u})for(unsigned early:{0u,1u})for(unsigned enable:{0u,1u})for(unsigned write:{0u,1u})for(unsigned alpha:{0u,1u})for(unsigned fn=0;fn<8;++fn){
        Fixture f;f.bp(0x40,enable|(fn<<1)|(write<<4));f.bp(0x43,format|(early<<6));auto captured=capture_depth(f.decoder.state());require(bool(captured),"valid capture");
        for(unsigned stored:{1u,0x7fffffu,0x800000u,0xfffffeu})for(unsigned relation=0;relation<3;++relation){
            const auto incoming=stored-1+relation;const bool zpass=!enable||(truth[fn]&(1u<<relation));const bool changes=enable&&write&&zpass&&(early||alpha);
            const auto r=evaluate_depth(*captured.snapshot,incoming,stored,alpha);require(bool(r),"valid evaluation");require(r.decision.color_accepted==(zpass&&alpha),"color acceptance");require(r.decision.depth_write==changes,"write acceptance");require(r.decision.depth_code==(changes?incoming:stored),"retained/new code");++checks;
        }
        for(unsigned edge:{0u,depth_code_max}){const auto r=evaluate_depth(*captured.snapshot,edge,edge,alpha);require(bool(r),"endpoints accepted");}
        require(!evaluate_depth(*captured.snapshot,depth_code_max+1,0,alpha),"incoming overflow rejected");require(!evaluate_depth(*captured.snapshot,0,depth_code_max+1,alpha),"stored overflow rejected");
    }
    std::cout<<"depth ordering checks="<<checks<<'\n';
}
void ownership_and_gates(){
    Fixture f;f.bp(0x40,0x13);f.bp(0x43,65);auto before=capture_depth(f.decoder.state());require(bool(before),"early less");
    f.bp(0x40,0x1f);f.bp(0x43,0);auto after=capture_depth(f.decoder.state());require(bool(after),"late always");require(before.snapshot->state_revision!=after.snapshot->state_revision,"revision ownership");
    auto a=evaluate_depth(*before.snapshot,7,9,false),b=evaluate_depth(*after.snapshot,7,9,false);require(a.decision.depth_write&&!a.decision.color_accepted&&!b.decision.depth_write,"alpha reject preserves early write only");
    for(auto pair: {std::pair<unsigned,unsigned>{0x40,32},{0x43,128},{0x43,2},{0x43,3},{0x43,8},{0,512},{0,1u<<19},{0xf5,12},{0xf5,7},{0xf5,0x1a}}){Fixture bad;bad.bp(pair.first,pair.second);auto c=capture_depth(bad.decoder.state());require(!c&&!c.snapshot,"unsupported capture fails atomically");}
}
void z_texture_decode(){
    ZTexture z;
    for(unsigned type=0;type<4;++type){require(decode_z_texture(0x123456,type|0xfff0u,z)&&!z.active()&&z.type==0&&z.bias==0,"inactive Z texture ignores its other bits");}
    for(unsigned op=1;op<3;++op)for(unsigned type=0;type<3;++type){
        require(decode_z_texture(0xabcdef,(op<<2)|type,z)&&z.active()&&z.op==op&&z.type==type&&z.bias==0xabcdef,"op, type and 24-bit bias decode");
    }
    require(!decode_z_texture(0,12,z)&&!z.active(),"reserved op fails");
    require(!decode_z_texture(0,4|3,z),"reserved type fails while active");
    require(!decode_z_texture(0,8|16,z),"reserved bit fails while active");
}
void z_texture_values(){
    // GX manual Appendix D: Z8 is an I8 texel, Z16 IA8 with A holding Z[15:8], Z24X8 RGBA8 with RGB holding Z[23:0].
    const std::array<std::uint8_t,4> texel{{0x12,0x34,0x56,0x78}};
    const std::uint32_t expected[3]={0x78,0x7812,0x123456};
    for(unsigned type=0;type<3;++type){
        require(z_texture_depth({2,std::uint8_t(type),0},texel,0x777777)==expected[type],"replace takes the texel Z");
        require(z_texture_depth({2,std::uint8_t(type),0x100},texel,0x777777)==expected[type]+0x100,"replace adds the bias");
        require(z_texture_depth({1,std::uint8_t(type),0x100},texel,0x1000)==expected[type]+0x1100,"add offsets the reference Z");
    }
    require(z_texture_depth({1,2,0xffffff},{{0xff,0xff,0xff,0}},2)==0x000000,"the adders wrap to 24 bits");
    require(z_texture_depth({2,2,0},{{0xff,0xff,0xff,0}},0)==0xffffff,"JFWDisplay's clear texel is the far plane");
    // An active Z texture makes the comparison late: alpha failure keeps the old depth.
    Fixture f;f.bp(0x40,0x13);f.bp(0x43,64);f.bp(0xf5,10);auto captured=capture_depth(f.decoder.state());
    require(bool(captured)&&captured.snapshot->z_texture.op==2&&captured.snapshot->z_texture.type==2,"Z texture captured with depth");
    auto r=evaluate_depth(*captured.snapshot,7,9,false);require(r&&!r.decision.depth_write&&r.decision.depth_code==9,"Z texture forces the late depth test");
    r=evaluate_depth(*captured.snapshot,7,9,true);require(r&&r.decision.depth_write&&r.decision.color_accepted&&r.decision.depth_code==7,"late test writes the Z-texture depth");
}
void z_texture_shader_text(){
    require(z_texture_glsl({},"t").globals.empty()&&z_texture_glsl({},"t").depth.empty(),"no Z texture, no code");
    const char* lanes[3]={"gxZ=(gxZTexel.a+uGxZBias","gxZ=((gxZTexel.a<<8u|gxZTexel.r)+uGxZBias","gxZ=((gxZTexel.r<<16u|gxZTexel.g<<8u|gxZTexel.b)+uGxZBias"};
    for(unsigned op=1;op<3;++op)for(unsigned type=0;type<3;++type)for(bool depth:{false,true}){
        const auto g=z_texture_glsl({std::uint8_t(op),std::uint8_t(type),depth},"lookup");
        require(g.globals=="uniform highp uint uGxZBias;\n","bias uniform");
        require(g.depth.find("highp uvec4 gxZTexel=uvec4(lookup);")!=std::string::npos&&g.depth.find(lanes[type])!=std::string::npos,"type selects the texel lanes");
        require((g.depth.find("+uGxZBias+gxZReference)&16777215u;")!=std::string::npos)==(op==1),"add offsets the reference Z");
        require((g.reference.find("dFdx(gl_FragCoord.z)")!=std::string::npos)==(op==1),"add derives the quad reference Z");
        require((g.depth.find("gl_FragDepth=float(gxZ)/16777216.0;")!=std::string::npos)==depth,"gl_FragDepth only with depth");
    }
}
}
int main(int argc,char** argv){try{ordering();ownership_and_gates();z_texture_decode();z_texture_values();z_texture_shader_text();if(argc==2){std::ofstream file(argv[1]);file<<depth_glsl();if(!file)throw std::runtime_error("shader export failed");}std::cout<<"GX depth tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
