#include "starlet/gx/pixel.hpp"
#include "starlet/gx/decoder.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace starlet::gx;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
struct Memory : MemoryReader { bool copy(std::uint32_t,std::uint8_t*,std::size_t) override{return false;} };
struct Sink : CommandSink {
 bool draw(const State&,const Draw&,MemoryReader&) override{return true;}
 bool bp_write(const State&,std::uint8_t,std::uint32_t) override{return true;}
 bool invalidate_vertices() override{return true;}
};
struct Context {
 Memory memory; Sink sink; Decoder decoder{memory,sink};
 void bp(unsigned address,unsigned value){std::uint8_t data[]={0x61,std::uint8_t(address),std::uint8_t(value>>16),std::uint8_t(value>>8),std::uint8_t(value)};
  require(bool(decoder.feed({data,sizeof(data)})),"BP fixture command");}
 PixelCapture capture(unsigned cm,unsigned alpha=0,unsigned fmt=1,PixelPolicy policy=PixelPolicy::candidate_v1){bp(0x41,cm);bp(0x42,alpha);bp(0x43,fmt);return capture_pixel(decoder.state(),policy);}
};
std::shared_ptr<const PixelSnapshot> plan(unsigned cm,unsigned alpha=0,unsigned fmt=1,PixelPolicy policy=PixelPolicy::candidate_v1){
 Context c;auto p=c.capture(cm,alpha,fmt,policy);require(bool(p),"pixel plan rejected");return p.snapshot;
}
PixelColor eval(const PixelSnapshot& p,PixelColor s,PixelColor d,int x=0,int y=0,bool accept=true){auto r=evaluate_pixel(p,s,d,x,y,accept);require(bool(r),"pixel evaluation rejected");return r.codes;}
unsigned rounded(unsigned color,unsigned f){return unsigned(std::floor(double(color)*double(f+(f>=128))/256.0+.5));}
unsigned select(unsigned code,bool source,unsigned c,PixelColor s,PixelColor d){
 const unsigned colors[]={0,255,source?d[c]:s[c],255u-(source?d[c]:s[c]),s[3],255u-s[3],d[3],255u-d[3]};return colors[code];
}
void capture_contract(){
 Context c;auto original=c.capture(8|4);require(bool(original),"capture");const auto words=original.snapshot->words();const auto revision=original.snapshot->state_revision();
 c.bp(0x41,0);c.bp(0x43,7);require(original.snapshot->words()==words && original.snapshot->state_revision()==revision,"snapshot borrowed mutable state");
 require(original.snapshot->uniforms()==std::array<std::uint32_t,4>{{12,0,1,1}},"uniform ABI");
 require(!c.capture(8,0,1,PixelPolicy::documented_only),"unknown nondither reduction admitted");
 auto documented=c.capture(8|4,0,1,PixelPolicy::documented_only);require(bool(documented) && documented.snapshot->assumptions()==0,"documented RGB dither rejected");
 require(c.capture(8|4|16,0,1,PixelPolicy::documented_only).status.error==PixelError::unproven_arithmetic,"unknown alpha reduction admitted");
 require(c.capture(8|1|4u<<8|5u<<5,0,0,PixelPolicy::documented_only).status.error==PixelError::unproven_arithmetic,"unknown product ties admitted");
 require(bool(c.capture(8|1|1u<<8|1u<<5,0,0,PixelPolicy::documented_only)),"exact zero/one factors need no rounding assumption");
 require(c.capture(8|1|2048,0,0,PixelPolicy::documented_only).status.error==PixelError::unproven_arithmetic,"subtract underflow policy not explicit");
 const auto all=plan(8|16|1|4u<<8|5u<<5,0,1);require(all->assumptions()==(product_ties_up|destination_msb_expansion|alpha_truncation|nondither_rgb_truncation),"assumption ledger missing behavior");
 for(unsigned fmt=2;fmt<8;++fmt)require(c.capture(8,0,fmt).status.error==PixelError::unsupported_state,"unsupported EFB format");
 for(unsigned cm:{3u,2048u,2u|2048u,0x10000u})require(c.capture(cm,0,0).status.error==PixelError::unsupported_state,"invalid blend/control combination");
 for(unsigned cm:{1u|6u<<8,1u|7u<<5})require(c.capture(cm,0,0).status.error==PixelError::unsupported_state,"RGB8 alpha/dither effect");
 const auto rgb8=c.capture(8|16|4,0,0,PixelPolicy::documented_only);require(bool(rgb8)&&!rgb8.snapshot->plan().alpha_write&&!rgb8.snapshot->plan().dither,"RGB8 alpha update and dither are ineffective");
 require(eval(*rgb8.snapshot,{17,99,213,7},{1,2,3,255})==PixelColor{17,99,213,255},"RGB8 color passes through with opaque storage alpha");
 require(c.capture(8,256,0).status.error==PixelError::unsupported_state,"RGB8 forced alpha");
 require(c.capture(8|2,0,1).status.error==PixelError::unsupported_state,"RGBA6 logic order guessed");
 require(c.capture(8,512,1).status.error==PixelError::unsupported_state,"YUV effect ignored");
 c.bp(0,1u<<9);require(c.capture(8).status.error==PixelError::unsupported_state,"AA effect ignored");
 require(evaluate_pixel(*documented.snapshot,{},{64,0,0,0},0,0).status.error==PixelError::invalid_input,"invalid six-bit destination");
 for(auto xy:{std::array<int,2>{-1,0},{640,0},{0,-1},{0,456}})require(!evaluate_pixel(*documented.snapshot,{},{},xy[0],xy[1]),"invalid EFB coordinate");
}
void dither(){
 const unsigned matrix[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
 const auto p=plan(8|4,0,1,PixelPolicy::documented_only);
 for(unsigned c=0;c<256;++c)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){
  const auto result=eval(*p,{std::uint8_t(c),std::uint8_t(c),std::uint8_t(c),255},{7,13,29,43},x,y);
  const auto expected=unsigned(std::floor((double(c)-std::floor(double(c)/64.0))/4.0+double(matrix[y][x])/16.0));
  require(result==PixelColor{std::uint8_t(expected),std::uint8_t(expected),std::uint8_t(expected),43},"documented 6-bit dither/alpha mask");
  require(expected<=63 && eval(*p,{std::uint8_t(c),0,0,0},{7,13,29,43},x+4,y+4)[0]==result[0],"dither range/period");
 }
 require(eval(*p,{64,64,64,255},{0,0,0,0},0,0)[0]==15 && eval(*p,{64,64,64,255},{0,0,0,0},1,0)[0]==16,"6-bit correction discontinuity");
 require(eval(*p,{2,2,2,255},{0,0,0,0},0,1)[0]==1 && eval(*p,{2,2,2,255},{0,0,0,0},0,0)[0]==0,"EFB row/column orientation");
 require(eval(*p,{255,255,255,255},{0,0,0,0},639,455)[0]==63,"last EFB pixel phase");
}
void products_and_factors(){
 const auto product=plan(8|1|4u<<8,0,0);
 for(unsigned c=0;c<256;++c)for(unsigned f=0;f<256;++f)
  require(eval(*product,{std::uint8_t(c),0,0,std::uint8_t(f)},{0,0,0,255})[0]==rounded(c,f),"all 8x8 product cases");
 require(eval(*product,{128,0,0,1},{0,0,0,255})[0]==1,"candidate tie convention");
 require(eval(*product,{255,0,0,127},{0,0,0,255})[0]==127 && eval(*product,{255,0,0,128},{0,0,0,255})[0]==128,"factor high-bit normalization boundary");
 const std::vector<PixelColor> inputs={{0,1,2,3},{127,128,129,130},{253,254,255,0},{11,239,71,167}};
 const std::vector<PixelColor> destinations={{0,1,2,3},{31,32,33,34},{63,62,61,60},{11,59,17,41}};
 for(unsigned sf=0;sf<8;++sf)for(unsigned df=0;df<8;++df){
  const auto p=plan(8|16|1|sf<<8|df<<5);
  for(auto s:inputs)for(auto codes:destinations){PixelColor d{};for(unsigned c=0;c<4;++c)d[c]=std::uint8_t(codes[c]*4+codes[c]/16);
   const auto result=eval(*p,s,codes);
   for(unsigned c=0;c<4;++c){const auto expected=std::min(255u,rounded(s[c],select(sf,true,c,s,d))+rounded(d[c],select(df,false,c,s,d)))/4;
    require(result[c]==expected,"all source/destination factor pairs, including slot-relative color and alpha");}
  }
 }
 const auto separate=plan(8|1|4u<<8|4u<<5,0,0);
 require(eval(*separate,{128,0,0,1},{128,0,0,255})[0]==2,"products rounded after sum instead of separately");
 const auto saturated=plan(8|1|1u<<8|1u<<5,0,0,PixelPolicy::documented_only);
 require(eval(*saturated,{200,200,200,255},{100,100,100,255})[0]==255,"add clamp");
}
void masks_alpha_subtract(){
 const PixelColor s={231,119,67,167},d={3,21,45,59};
 for(unsigned masks=0;masks<4;++masks)for(unsigned forced:{0u,79u,255u}){
  const auto p=plan(1|4|4u<<8|6u<<5|((masks&1)?8:0)|((masks&2)?16:0),256|forced);
  auto result=eval(*p,s,d,2,3);
  if(!(masks&1))require(result[0]==d[0] && result[1]==d[1] && result[2]==d[2],"RGB mask altered canonical destination");
  require(result[3]==((masks&2)?forced/4:d[3]),"alpha mask/override/truncation order");
  require(eval(*p,s,d,2,3,false)==d,"rejected color changed storage");
 }
 const auto ordinary=plan(8|16|1|4|4u<<8|6u<<5),forced=plan(8|16|1|4|4u<<8|6u<<5,256);
 const auto a=eval(*ordinary,s,d),b=eval(*forced,s,d);
 require(a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && b[3]==0,"forced alpha contaminated source/destination factors");
 for(unsigned sf=0;sf<8;++sf)for(unsigned df=0;df<8;++df){const auto p=plan(8|16|1|2048|sf<<8|df<<5);
  require(eval(*p,{20,90,250,255},{63,40,0,63})==PixelColor{58,18,0,0},"subtract must ignore factors and saturate");}
 const auto masked=plan(0);PixelColor history=d;for(unsigned i=0;i<1000;++i)history=eval(*masked,s,history,i%640,i%456);require(history==d,"masked repeated writes drift codes");
}
void logic_tables(){
 for(unsigned op=0;op<16;++op){const auto p=plan(8|2|op<<12,0,0,PixelPolicy::documented_only);
  for(unsigned s=0;s<256;++s)for(unsigned d=0;d<256;++d){unsigned expected=0;
   for(unsigned bit=0;bit<8;++bit){const unsigned index=2*(1-((s>>bit)&1))+(1-((d>>bit)&1));expected|=((op>>index)&1)<<bit;}
   const auto result=eval(*p,{std::uint8_t(s),0,0,91},{std::uint8_t(d),0,0,255});
   require(result[0]==expected && result[3]==255,"eight-bit logic truth table or opaque RGB8 alpha");
  }
 }
}
void word(std::ofstream& file,unsigned v){for(unsigned b=0;b<4;++b)file.put(char(v>>(8*b)));}
void fixtures(const std::string& prefix){
 std::ofstream shader(prefix+".comp"),cases(prefix+".cases",std::ios::binary),expected(prefix+".expected",std::ios::binary);
 require(bool(shader)&&bool(cases)&&bool(expected),"fixture output unavailable");
 shader<<"#version 310 es\nprecision highp int;\nprecision highp float;\n"<<pixel_glsl()<<R"(
layout(local_size_x=64) in;
struct Case { uvec4 source; uvec4 destination; uvec4 controls; uvec4 coordinate; };
layout(std430,binding=0) readonly buffer Inputs { Case cases[]; };
layout(std430,binding=1) writeonly buffer Outputs { uvec4 results[]; };
uniform uint uCount;
void main() { uint i=gl_GlobalInvocationID.x; if(i>=uCount)return;
 results[i]=gx_pixel(cases[i].source,cases[i].destination,ivec2(cases[i].coordinate.xy),cases[i].coordinate.z!=0u,cases[i].controls);
}
)";
 unsigned count=0;
 auto emit=[&](const PixelSnapshot& p,PixelColor s,PixelColor d,unsigned x,unsigned y,bool accepted=true){
  for(auto v:s)word(cases,v);for(auto v:d)word(cases,v);for(auto v:p.uniforms())word(cases,v);
  for(auto v:{x,y,unsigned(accepted),0u})word(cases,v);
  for(auto v:eval(p,s,d,x,y,accepted))word(expected,v);++count;
 };
 const auto dither_plan=plan(8|4,0,1,PixelPolicy::documented_only);
 for(unsigned c=0;c<256;++c)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)
  emit(*dither_plan,{std::uint8_t(c),std::uint8_t(255-c),127,213},{1,17,33,49},x,y);
 for(unsigned sf=0;sf<8;++sf)for(unsigned df=0;df<8;++df)for(unsigned masks=0;masks<4;++masks){
  auto p=plan(1|4|sf<<8|df<<5|((masks&1)?8:0)|((masks&2)?16:0),256);
  emit(*p,{31,157,251,167},{61,42,19,37},639,455);emit(*p,{31,157,251,167},{61,42,19,37},3,2,false);
 }
 for(unsigned op=0;op<16;++op){auto p=plan(8|2|op<<12,0,0);emit(*p,{19,123,255,71},{142,93,13,255},0,0);}
 for(unsigned c=0;c<256;++c){auto p=plan(8|16|1|2048);emit(*p,{std::uint8_t(c),0,255,167},{63,31,12,49},2,3);}
 require(bool(shader)&&bool(cases)&&bool(expected),"fixture write failed");std::cout<<count<<" GPU arithmetic cases: 64-byte LE input, 16-byte LE expected\n";
}
}
int main(int argc,char**argv){try{capture_contract();dither();products_and_factors();masks_alpha_subtract();logic_tables();
 if(argc==3 && std::string(argv[1])=="--fixtures")fixtures(argv[2]);else require(argc==1,"usage: gx_pixel_tests [--fixtures prefix]");
 std::cout<<"pixel arithmetic tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
