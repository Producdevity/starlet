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
    for(auto pair: {std::pair<unsigned,unsigned>{0x40,32},{0x43,128},{0x43,2},{0x43,3},{0x43,8},{0,512},{0,1u<<19},{0xf5,4},{0xf5,8},{0xf5,12}}){Fixture bad;bad.bp(pair.first,pair.second);auto c=capture_depth(bad.decoder.state());require(!c&&!c.snapshot,"unsupported capture fails atomically");}
}
}
int main(int argc,char** argv){try{ordering();ownership_and_gates();if(argc==2){std::ofstream file(argv[1]);file<<depth_glsl();if(!file)throw std::runtime_error("shader export failed");}std::cout<<"GX depth tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
