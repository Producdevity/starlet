#include "starlet/gx/pixel.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace starlet::gx {
namespace {
constexpr unsigned bayer[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
unsigned factor(unsigned selection, bool source_term, unsigned component,
                const std::array<unsigned,4>& source, const std::array<unsigned,4>& destination) {
    switch (selection) {
    case 0: return 0;
    case 1: return 255;
    case 2: return source_term ? destination[component] : source[component];
    case 3: return 255 - (source_term ? destination[component] : source[component]);
    case 4: return source[3];
    case 5: return 255-source[3];
    case 6: return destination[3];
    case 7: return 255-destination[3];
    }
    return 0;
}
unsigned product(unsigned color, unsigned coefficient) {
    return (color * (coefficient + (coefficient >> 7)) + 128) >> 8;
}
unsigned logic(unsigned operation, unsigned source, unsigned destination) {
    switch (operation) {
    case 0: return 0;
    case 1: return source & destination;
    case 2: return source & ~destination;
    case 3: return source;
    case 4: return ~source & destination;
    case 5: return destination;
    case 6: return source ^ destination;
    case 7: return source | destination;
    case 8: return ~(source | destination);
    case 9: return ~(source ^ destination);
    case 10: return ~destination;
    case 11: return source | ~destination;
    case 12: return ~source;
    case 13: return ~source | destination;
    case 14: return ~(source & destination);
    case 15: return 255;
    }
    return 0;
}
}

PixelCapture capture_pixel(const State& state, PixelPolicy policy) {
    auto reject=[](PixelError code,const char* message) { return PixelCapture{{code,message},{}}; };
    if (policy!=PixelPolicy::documented_only && policy!=PixelPolicy::candidate_v1)
        return reject(PixelError::invalid_input,"invalid pixel arithmetic policy");
    auto snapshot=std::shared_ptr<PixelSnapshot>(new PixelSnapshot);
    snapshot->words_={{state.bp(0),state.bp(0x41),state.bp(0x42),state.bp(0x43)}};
    snapshot->revision_=state.revision();
    snapshot->efb_=state.efb();
    snapshot->policy_=policy;
    const auto color=snapshot->words_[1], alpha=snapshot->words_[2], pixel=snapshot->words_[3];
    if ((snapshot->words_[0] & (1u<<9)) || (pixel&7)>1)
        return reject(PixelError::unsupported_state,"pixel arithmetic supports only non-AA RGB8/RGBA6");
    if ((color&~0xffffu) || (alpha&~0x1ffu) || (pixel&~0x7fu))
        return reject(PixelError::unsupported_state,"reserved pixel bits or YUV state");
    const bool blend=color&1, logical=color&2, subtract=color&(1u<<11);
    if ((logical && (blend || subtract)) || (subtract && !blend))
        return reject(PixelError::unsupported_state,"ambiguous blend/logic/subtract combination");
    auto& p=snapshot->plan_;
    p.format=(pixel&7)==1 ? PixelFormat::rgba6 : PixelFormat::rgb8;
    p.operation=logical ? PixelOperation::logic : subtract ? PixelOperation::subtract : blend ? PixelOperation::blend : PixelOperation::copy;
    p.source_factor=(color>>8)&7;
    p.destination_factor=(color>>5)&7;
    p.logic_operation=(color>>12)&15;
    p.color_write=color&8;
    p.alpha_write=p.format==PixelFormat::rgba6 && (color&16);
    p.dither=p.format==PixelFormat::rgba6 && (color&4);
    p.force_alpha=alpha&256;
    p.forced_alpha=alpha&255;
    if (p.format==PixelFormat::rgb8 && (p.alpha_write || p.force_alpha || p.dither ||
        (p.operation==PixelOperation::blend && (p.source_factor>=6 || p.destination_factor>=6))))
        return reject(PixelError::unsupported_state,"RGB8 alpha-plane effects or dithering are not admitted");
    if (p.format==PixelFormat::rgba6 && p.operation==PixelOperation::logic)
        return reject(PixelError::unsupported_state,"RGBA6 logic width and quantization order remain unproven");
    auto& assumptions=snapshot->assumptions_;
    if (p.color_write || p.alpha_write) {
        if (p.operation==PixelOperation::blend && (p.source_factor>1 || p.destination_factor>1)) assumptions|=product_ties_up;
        if (p.operation==PixelOperation::subtract) assumptions|=subtract_saturation;
        if (p.format==PixelFormat::rgba6) {
            if (p.operation==PixelOperation::blend || p.operation==PixelOperation::subtract) assumptions|=destination_msb_expansion;
            if (p.color_write && !p.dither) assumptions|=nondither_rgb_truncation;
            if (p.alpha_write) assumptions|=alpha_truncation;
        }
    }
    if (assumptions && policy==PixelPolicy::documented_only)
        return reject(PixelError::unproven_arithmetic,"pixel operation requires explicit candidate_v1 arithmetic");
    return {{},std::move(snapshot)};
}

PixelResult evaluate_pixel(const PixelSnapshot& snapshot, PixelColor source, PixelColor destination,
                           int x, int y, bool accepted) {
    const auto& p=snapshot.plan();
    if (x<0 || x>=EfbLayout::width || y<0 || y>=snapshot.efb().height)
        return {{PixelError::invalid_input,"pixel coordinate outside the EFB frame"},{}};
    if ((p.format==PixelFormat::rgba6 && std::any_of(destination.begin(),destination.end(),[](unsigned v){return v>63;})) ||
        (p.format==PixelFormat::rgb8 && destination[3]!=255))
        return {{PixelError::invalid_input,"destination does not use canonical EFB codes"},{}};
    PixelResult result{{},destination};
    if (!accepted) return result;
    std::array<unsigned,4> s{},d{},out{};
    for (unsigned c=0;c<4;++c) {
        s[c]=source[c];
        d[c]=p.format==PixelFormat::rgba6 ? (unsigned(destination[c])<<2)|(destination[c]>>4) : destination[c];
    }
    for (unsigned c=0;c<4;++c) {
        switch (p.operation) {
        case PixelOperation::copy: out[c]=s[c]; break;
        case PixelOperation::blend:
            out[c]=std::min(255u,product(s[c],factor(p.source_factor,true,c,s,d))+product(d[c],factor(p.destination_factor,false,c,s,d)));
            break;
        case PixelOperation::subtract: out[c]=d[c]>s[c] ? d[c]-s[c] : 0; break;
        case PixelOperation::logic: out[c]=logic(p.logic_operation,s[c],d[c])&255; break;
        }
    }
    if (p.force_alpha) out[3]=p.forced_alpha;
    for (unsigned c=0;c<4;++c) {
        if (c==3 ? !p.alpha_write : !p.color_write) continue;
        unsigned code=out[c];
        if (p.format==PixelFormat::rgba6)
            code=c<3 && p.dither ? (((code-(code>>6))<<2)+bayer[y&3][x&3])>>4 : code>>2;
        result.codes[c]=std::uint8_t(code);
    }
    return result;
}

std::string_view pixel_glsl() {
    return R"GLSL(
uint gx_pixel_factor(uint f, bool source_term, uint c, uvec4 s, uvec4 d) {
    if (f==0u) return 0u;
    if (f==1u) return 255u;
    if (f==2u) return source_term ? d[c] : s[c];
    if (f==3u) return 255u-(source_term ? d[c] : s[c]);
    if (f==4u) return s.a;
    if (f==5u) return 255u-s.a;
    if (f==6u) return d.a;
    return 255u-d.a;
}
uint gx_pixel_product(uint c, uint f) { return (c*(f+(f>>7u))+128u)>>8u; }
uvec4 gx_pixel_logic(uint op, uvec4 s, uvec4 d) {
    if (op==0u) return uvec4(0u);
    if (op==1u) return s&d;
    if (op==2u) return s&~d;
    if (op==3u) return s;
    if (op==4u) return ~s&d;
    if (op==5u) return d;
    if (op==6u) return s^d;
    if (op==7u) return s|d;
    if (op==8u) return ~(s|d);
    if (op==9u) return ~(s^d);
    if (op==10u) return ~d;
    if (op==11u) return s|~d;
    if (op==12u) return ~s;
    if (op==13u) return ~s|d;
    if (op==14u) return ~(s&d);
    return uvec4(255u);
}
uvec4 gx_pixel(uvec4 s, uvec4 destination, ivec2 efb, bool accepted, uvec4 controls) {
    if (!accepted) return destination;
    uint cm=controls.x;
    bool rgba6=controls.z==1u;
    uvec4 d=rgba6 ? (destination<<2u)|(destination>>4u) : destination;
    uvec4 color=s;
    if ((cm&2u)!=0u) color=gx_pixel_logic((cm>>12u)&15u,s,d)&uvec4(255u);
    else if ((cm&2048u)!=0u) color=uvec4(max(ivec4(d)-ivec4(s),ivec4(0)));
    else if ((cm&1u)!=0u) {
        for (uint c=0u;c<4u;++c) color[c]=min(255u,
            gx_pixel_product(s[c],gx_pixel_factor((cm>>8u)&7u,true,c,s,d))+
            gx_pixel_product(d[c],gx_pixel_factor((cm>>5u)&7u,false,c,s,d)));
    }
    if ((controls.y&256u)!=0u) color.a=controls.y&255u;
    if (rgba6) {
        const uint bayer[16]=uint[16](0u,8u,2u,10u,12u,4u,14u,6u,3u,11u,1u,9u,15u,7u,13u,5u);
        color.rgb=(cm&4u)!=0u ? (((color.rgb-(color.rgb>>6u))<<2u)+uvec3(bayer[(efb.y&3)*4+(efb.x&3)]))>>4u : color.rgb>>2u;
        color.a>>=2u;
    }
    if ((cm&8u)==0u) color.rgb=destination.rgb;
    if (!rgba6 || (cm&16u)==0u) color.a=destination.a;
    return color;
}
)GLSL";
}

std::string pixel_stage_glsl(const PixelPlan& p) {
    if (p.format != PixelFormat::rgba6 || p.operation == PixelOperation::logic)
        throw std::invalid_argument("specialized pixel stage admits RGBA6 copy/blend/subtract only");
    auto factor = [](unsigned f, bool source_term) -> std::string {
        const char* other = source_term ? "d" : "s";
        switch (f) {
        case 0: return "uvec4(0u)";
        case 1: return "uvec4(255u)";
        case 2: return other;
        case 3: return std::string("uvec4(255u)-") + other;
        case 4: return "s.aaaa";
        case 5: return "uvec4(255u)-s.aaaa";
        case 6: return "d.aaaa";
        default: return "uvec4(255u)-d.aaaa";
        }
    };
    std::string g = "uvec4 gxPixelProduct(uvec4 c, uvec4 f) { return (c*(f+(f>>7u))+128u)>>8u; }\n"
                    "uvec4 gxPixelStage(uvec4 s, uvec4 code, ivec2 efb, uint forced_alpha) {\n"
                    "    uvec4 d = (code<<2u)|(code>>4u);\n";
    switch (p.operation) {
    case PixelOperation::copy: g += "    uvec4 c = s;\n"; break;
    case PixelOperation::blend:
        g += "    uvec4 c = min(uvec4(255u), gxPixelProduct(s," + factor(p.source_factor, true) +
             ")+gxPixelProduct(d," + factor(p.destination_factor, false) + "));\n";
        break;
    default: g += "    uvec4 c = uvec4(max(ivec4(d)-ivec4(s),ivec4(0)));\n"; break;
    }
    if (p.force_alpha) g += "    c.a = forced_alpha;\n";
    if (p.dither)
        // Closed form of the 4x4 Bayer table in gx_pixel.
        g += "    uvec2 q = uvec2(efb)&3u; uint x = q.x^q.y;\n"
             "    uint bayer = ((x&1u)<<3u)|((q.y&1u)<<2u)|(x&2u)|((q.y&2u)>>1u);\n"
             "    c.rgb = (((c.rgb-(c.rgb>>6u))<<2u)+uvec3(bayer))>>4u;\n";
    else g += "    c.rgb >>= 2u;\n";
    g += "    c.a >>= 2u;\n";
    if (!p.color_write) g += "    c.rgb = code.rgb;\n";
    if (!p.alpha_write) g += "    c.a = code.a;\n";
    return g + "    return c;\n}\n";
}

} // namespace starlet::gx
