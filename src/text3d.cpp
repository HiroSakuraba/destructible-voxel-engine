#include "dve/text3d.hpp"

#include "dve/master_material.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <tuple>

namespace dve {
namespace {

constexpr std::array<char, 8> kTextMagic{'D','V','E','T','E','X','T','1'};
constexpr std::uint16_t kTextMajor = 1;
constexpr std::uint16_t kTextMinor = 1;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr float kBandEpsilon = 1.0F / 1024.0F;
constexpr std::uint32_t kMaximumBandOffset = 65535U;
constexpr std::uint32_t kMaximumTextureHeight = 65535U;

[[nodiscard]] bool finite(float v) noexcept { return std::isfinite(v); }
[[nodiscard]] bool finite(Float2 v) noexcept { return finite(v.x) && finite(v.y); }
[[nodiscard]] bool finite(Float3 v) noexcept { return finite(v.x) && finite(v.y) && finite(v.z); }
[[nodiscard]] bool finite(Float4 v) noexcept { return finite(v.x) && finite(v.y) && finite(v.z) && finite(v.w); }
[[nodiscard]] Float2 add(Float2 a, Float2 b) noexcept { return {a.x+b.x,a.y+b.y}; }
[[nodiscard]] Float2 sub(Float2 a, Float2 b) noexcept { return {a.x-b.x,a.y-b.y}; }
[[nodiscard]] Float2 mul(Float2 a, float s) noexcept { return {a.x*s,a.y*s}; }
[[nodiscard]] Float2 midpoint(Float2 a, Float2 b) noexcept { return mul(add(a,b),0.5F); }
[[nodiscard]] float length_sq(Float2 a) noexcept { return a.x*a.x+a.y*a.y; }
[[nodiscard]] Float2 normalized(Float2 a) noexcept {
    const float ls=length_sq(a); if(!(ls>1.0e-20F)) return {1.0F,0.0F}; const float inv=1.0F/std::sqrt(ls); return mul(a,inv);
}
[[nodiscard]] Float2 bezier(const Text3DQuadraticCurve& c,float t) noexcept {
    const float u=1.0F-t; return add(add(mul(c.p1,u*u),mul(c.p2,2.0F*u*t)),mul(c.p3,t*t));
}
[[nodiscard]] float signed_area(std::span<const Float2> points) noexcept {
    if(points.size()<3U)return 0.0F; float sum=0.0F; for(std::size_t i=0;i<points.size();++i){const auto a=points[i],b=points[(i+1U)%points.size()];sum+=a.x*b.y-b.x*a.y;}return 0.5F*sum;
}

void hash_byte(std::uint64_t& h,std::uint8_t b) noexcept { h=(h^b)*kFnvPrime; }
template<class T> void hash_scalar(std::uint64_t& h,const T& v) noexcept {
    const auto* p=reinterpret_cast<const std::uint8_t*>(&v); for(std::size_t i=0;i<sizeof(T);++i)hash_byte(h,p[i]);
}
void hash_string(std::uint64_t& h,std::string_view s) noexcept { hash_scalar(h,static_cast<std::uint64_t>(s.size()));for(unsigned char c:s)hash_byte(h,c); }
void hash_f2(std::uint64_t& h,Float2 v) noexcept {hash_scalar(h,v.x);hash_scalar(h,v.y);} void hash_f3(std::uint64_t& h,Float3 v) noexcept {hash_scalar(h,v.x);hash_scalar(h,v.y);hash_scalar(h,v.z);} void hash_f4(std::uint64_t& h,Float4 v) noexcept {hash_scalar(h,v.x);hash_scalar(h,v.y);hash_scalar(h,v.z);hash_scalar(h,v.w);}

class BeReader {
public:
    explicit BeReader(std::span<const std::byte> data):data_(data){}
    [[nodiscard]] std::size_t size()const noexcept{return data_.size();}
    void require(std::size_t off,std::size_t n)const{if(off>data_.size()||n>data_.size()-off)throw std::runtime_error("font table is truncated");}
    [[nodiscard]] std::uint8_t u8(std::size_t o)const{require(o,1);return std::to_integer<std::uint8_t>(data_[o]);}
    [[nodiscard]] std::int8_t i8(std::size_t o)const{return static_cast<std::int8_t>(u8(o));}
    [[nodiscard]] std::uint16_t u16(std::size_t o)const{require(o,2);return static_cast<std::uint16_t>((u8(o)<<8U)|u8(o+1));}
    [[nodiscard]] std::int16_t i16(std::size_t o)const{return std::bit_cast<std::int16_t>(u16(o));}
    [[nodiscard]] std::uint32_t u32(std::size_t o)const{require(o,4);return (static_cast<std::uint32_t>(u8(o))<<24U)|(static_cast<std::uint32_t>(u8(o+1))<<16U)|(static_cast<std::uint32_t>(u8(o+2))<<8U)|u8(o+3);}
    [[nodiscard]] std::span<const std::byte> slice(std::size_t o,std::size_t n)const{require(o,n);return data_.subspan(o,n);}
private:std::span<const std::byte> data_;
};

struct Table {std::uint32_t offset{},length{};};
struct RawPoint {Float2 p{};bool on{};};

[[nodiscard]] std::uint32_t tag(std::string_view s) noexcept {return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[0]))<<24U)|(static_cast<std::uint32_t>(static_cast<unsigned char>(s[1]))<<16U)|(static_cast<std::uint32_t>(static_cast<unsigned char>(s[2]))<<8U)|static_cast<unsigned char>(s[3]);}

class TrueTypeFont {
public:
    explicit TrueTypeFont(std::vector<std::byte> bytes):bytes_(std::move(bytes)),r_(bytes_){parse();}
    [[nodiscard]] const Text3DFontInfo& info()const noexcept{return info_;}
    [[nodiscard]] std::optional<std::uint16_t> glyph_for(std::uint32_t cp)const noexcept {
        if(cmap12_){const auto& groups=*cmap12_;std::size_t lo=0,hi=groups.size();while(lo<hi){const auto m=(lo+hi)/2U;if(cp<groups[m][0])hi=m;else if(cp>groups[m][1])lo=m+1U;else return static_cast<std::uint16_t>(groups[m][2]+cp-groups[m][0]);}}
        if(cmap4_&&cp<=0xFFFFU){const auto& c=*cmap4_;for(std::size_t i=0;i<c.end.size();++i){if(cp<c.start[i]||cp>c.end[i])continue;if(c.range[i]==0U)return static_cast<std::uint16_t>((cp+c.delta[i])&0xFFFFU);const std::size_t roWord=c.rangeOffsetBase+i*2U;const std::size_t glyphWord=roWord+c.range[i]+2U*(cp-c.start[i]);if(glyphWord+2U>c.bytes.size())return std::nullopt;const auto gid=static_cast<std::uint16_t>((std::to_integer<std::uint8_t>(c.bytes[glyphWord])<<8U)|std::to_integer<std::uint8_t>(c.bytes[glyphWord+1]));return gid?static_cast<std::uint16_t>((gid+c.delta[i])&0xFFFFU):0U;}}
        return std::nullopt;
    }
    [[nodiscard]] std::pair<std::uint16_t,std::int16_t> metric(std::uint16_t gid)const{
        if(gid>=info_.glyphCount)throw std::runtime_error("glyph index exceeds font");if(gid<numHMetrics_){return {r_.u16(hmtx_.offset+static_cast<std::size_t>(gid)*4U),r_.i16(hmtx_.offset+static_cast<std::size_t>(gid)*4U+2U)};}const auto advance=r_.u16(hmtx_.offset+static_cast<std::size_t>(numHMetrics_-1U)*4U);const auto lsbOffset=hmtx_.offset+static_cast<std::size_t>(numHMetrics_)*4U+static_cast<std::size_t>(gid-numHMetrics_)*2U;return {advance,r_.i16(lsbOffset)};
    }
    [[nodiscard]] std::int16_t kerning(std::uint16_t left,std::uint16_t right)const noexcept {const std::uint32_t key=(static_cast<std::uint32_t>(left)<<16U)|right;const auto it=kern_.find(key);return it==kern_.end()?0:it->second;}
    [[nodiscard]] Text3DGlyphGeometry glyph(std::uint16_t gid,std::uint32_t cp)const {std::set<std::uint16_t> stack;return glyph_impl(gid,cp,stack,0);}
private:
    struct Cmap4 {std::vector<std::uint16_t>end,start,range;std::vector<std::int16_t>delta;std::vector<std::byte>bytes;std::size_t rangeOffsetBase{};};
    std::vector<std::byte> bytes_;BeReader r_;std::map<std::uint32_t,Table> tables_;Text3DFontInfo info_{};Table head_{},maxp_{},hhea_{},hmtx_{},loca_{},glyf_{};std::uint16_t numHMetrics_{};std::int16_t locaFormat_{};std::optional<std::vector<std::array<std::uint32_t,3>>> cmap12_;std::optional<Cmap4> cmap4_;std::unordered_map<std::uint32_t,std::int16_t> kern_;

    [[nodiscard]] Table need(std::string_view name)const{const auto it=tables_.find(tag(name));if(it==tables_.end())throw std::runtime_error("required TrueType table is missing: "+std::string(name));return it->second;}
    void parse(){
        if(r_.size()<12U)throw std::runtime_error("font is too small");const auto sfnt=r_.u32(0);if(sfnt==tag("ttcf"))throw std::runtime_error("TrueType collections are not supported by this cooker");if(sfnt!=0x00010000U&&sfnt!=tag("true"))throw std::runtime_error("only glyf-based TrueType fonts are supported");const auto count=r_.u16(4);if(count==0||count>256U)throw std::runtime_error("invalid TrueType table count");for(std::uint16_t i=0;i<count;++i){const std::size_t o=12U+static_cast<std::size_t>(i)*16U;r_.require(o,16);const auto t=r_.u32(o),off=r_.u32(o+8U),len=r_.u32(o+12U);r_.require(off,len);tables_[t]={off,len};}
        head_=need("head");maxp_=need("maxp");hhea_=need("hhea");hmtx_=need("hmtx");loca_=need("loca");glyf_=need("glyf");
        info_.unitsPerEm=r_.u16(head_.offset+18U);locaFormat_=r_.i16(head_.offset+50U);info_.glyphCount=r_.u16(maxp_.offset+4U);info_.ascender=r_.i16(hhea_.offset+4U);info_.descender=r_.i16(hhea_.offset+6U);info_.lineGap=r_.i16(hhea_.offset+8U);numHMetrics_=r_.u16(hhea_.offset+34U);if(info_.unitsPerEm<16U||info_.glyphCount==0U||numHMetrics_==0U||numHMetrics_>info_.glyphCount)throw std::runtime_error("invalid TrueType metrics");
        std::uint64_t h=kFnvOffset;for(auto b:bytes_)hash_byte(h,std::to_integer<std::uint8_t>(b));info_.sourceHash=h;parse_names();parse_cmap();parse_kern();
    }
    void parse_names(){const auto it=tables_.find(tag("name"));if(it==tables_.end())return;const auto t=it->second;if(t.length<6U)return;const auto count=r_.u16(t.offset+2U),storage=r_.u16(t.offset+4U);for(std::uint16_t i=0;i<count;++i){const std::size_t o=t.offset+6U+static_cast<std::size_t>(i)*12U;if(o+12U>t.offset+t.length)break;const auto platform=r_.u16(o),nameId=r_.u16(o+6U),len=r_.u16(o+8U),off=r_.u16(o+10U);if(nameId!=1U&&nameId!=2U)continue;const std::size_t p=t.offset+storage+off;if(p+len>t.offset+t.length)continue;std::string value;if(platform==0U||platform==3U){for(std::size_t q=0;q+1U<len;q+=2U){const auto c=r_.u16(p+q);if(c<128U)value.push_back(static_cast<char>(c));else value.push_back('?');}}else{for(std::size_t q=0;q<len;++q)value.push_back(static_cast<char>(r_.u8(p+q)));}if(nameId==1U&&info_.family.empty())info_.family=value;if(nameId==2U&&info_.subfamily.empty())info_.subfamily=value;}}
    void parse_cmap(){const auto t=need("cmap");if(t.length<4U)throw std::runtime_error("invalid cmap table");const auto count=r_.u16(t.offset+2U);std::optional<std::size_t> best12,best4;for(std::uint16_t i=0;i<count;++i){const std::size_t o=t.offset+4U+static_cast<std::size_t>(i)*8U;r_.require(o,8);const std::uint16_t platform=r_.u16(o);const std::uint16_t encoding=r_.u16(o+2U);const std::size_t sub=static_cast<std::size_t>(t.offset)+r_.u32(o+4U);if(sub+2U>t.offset+t.length)continue;const auto format=r_.u16(sub);if(format==12U&&(platform==0U||(platform==3U&&encoding==10U)))best12=sub;else if(format==4U&&(platform==0U||platform==3U))best4=sub;}
        if(best12){const auto o=*best12;const auto len=r_.u32(o+4U),groups=r_.u32(o+12U);if(o+len>t.offset+t.length||groups>1000000U)throw std::runtime_error("invalid cmap format 12");std::vector<std::array<std::uint32_t,3>> out;out.reserve(groups);for(std::uint32_t i=0;i<groups;++i){const auto p=o+16U+static_cast<std::size_t>(i)*12U;out.push_back({r_.u32(p),r_.u32(p+4U),r_.u32(p+8U)});}cmap12_=std::move(out);}
        if(best4){const auto o=*best4;const std::uint16_t len=r_.u16(o+2U);const std::size_t segCount=static_cast<std::size_t>(r_.u16(o+6U)/2U);if(segCount==0U||o+len>t.offset+t.length)throw std::runtime_error("invalid cmap format 4");Cmap4 c;const std::size_t endBase=o+14U,startBase=endBase+segCount*2U+2U,deltaBase=startBase+segCount*2U,rangeBase=deltaBase+segCount*2U;c.end.resize(segCount);c.start.resize(segCount);c.delta.resize(segCount);c.range.resize(segCount);for(std::size_t i=0;i<segCount;++i){c.end[i]=r_.u16(endBase+i*2U);c.start[i]=r_.u16(startBase+i*2U);c.delta[i]=r_.i16(deltaBase+i*2U);c.range[i]=r_.u16(rangeBase+i*2U);}const auto sub=r_.slice(o,len);c.bytes.assign(sub.begin(),sub.end());c.rangeOffsetBase=rangeBase-o;cmap4_=std::move(c);}if(!cmap12_&&!cmap4_)throw std::runtime_error("font has no supported Unicode cmap");
    }
    void parse_kern(){const auto it=tables_.find(tag("kern"));if(it==tables_.end())return;const auto t=it->second;if(t.length<4U||r_.u16(t.offset)!=0U)return;const auto n=r_.u16(t.offset+2U);std::size_t p=t.offset+4U;for(std::uint16_t s=0;s<n&&p+6U<=t.offset+t.length;++s){const std::uint16_t len=r_.u16(p+2U);const std::uint16_t coverage=r_.u16(p+4U);const std::uint8_t format=static_cast<std::uint8_t>(coverage>>8U);if(len<6U||p+len>t.offset+t.length)break;if(format==0U&&(coverage&1U)){const auto pairs=r_.u16(p+6U);for(std::uint16_t i=0;i<pairs;++i){const auto q=p+14U+static_cast<std::size_t>(i)*6U;if(q+6U>p+len)break;const std::uint16_t left=r_.u16(q);const std::uint16_t right=r_.u16(q+2U);const std::int16_t value=r_.i16(q+4U);kern_[(static_cast<std::uint32_t>(left)<<16U)|right]=value;}}p+=len;}}
    [[nodiscard]] std::uint32_t glyph_offset(std::uint16_t gid)const{const std::size_t p=loca_.offset+(locaFormat_==0?static_cast<std::size_t>(gid)*2U:static_cast<std::size_t>(gid)*4U);return locaFormat_==0?static_cast<std::uint32_t>(r_.u16(p))*2U:r_.u32(p);}
    [[nodiscard]] Text3DGlyphGeometry glyph_impl(std::uint16_t gid,std::uint32_t cp,std::set<std::uint16_t>& stack,unsigned depth)const{
        if(depth>16U||!stack.insert(gid).second)throw std::runtime_error("recursive composite glyph");Text3DGlyphGeometry g;g.codepoint=cp;g.glyphIndex=gid;const auto [advance,lsb]=metric(gid);const float inv=1.0F/info_.unitsPerEm;g.advanceEm=advance*inv;g.leftSideBearingEm=lsb*inv;const auto start=glyph_offset(gid),end=glyph_offset(static_cast<std::uint16_t>(gid+1U));if(end<start||end>glyf_.length)throw std::runtime_error("invalid glyf offset");if(end==start){stack.erase(gid);return g;}const std::size_t o=glyf_.offset+start;const auto contours=r_.i16(o);g.minimumEm={r_.i16(o+2U)*inv,r_.i16(o+4U)*inv};g.maximumEm={r_.i16(o+6U)*inv,r_.i16(o+8U)*inv};if(contours>=0)parse_simple(o,static_cast<std::uint16_t>(contours),inv,g);else parse_composite(o,cp,inv,g,stack,depth);stack.erase(gid);return g;
    }
    void parse_simple(std::size_t o,std::uint16_t contourCount,float inv,Text3DGlyphGeometry& g)const{
        if(contourCount==0U)return;std::vector<std::uint16_t>endpoints(contourCount);for(std::uint16_t i=0;i<contourCount;++i)endpoints[i]=r_.u16(o+10U+static_cast<std::size_t>(i)*2U);const std::size_t pointCount=static_cast<std::size_t>(endpoints.back())+1U;if(pointCount>1000000U)throw std::runtime_error("glyph point limit exceeded");std::size_t p=o+10U+static_cast<std::size_t>(contourCount)*2U;const auto instructionLength=r_.u16(p);p+=2U+instructionLength;std::vector<std::uint8_t>flags;flags.reserve(pointCount);while(flags.size()<pointCount){const auto f=r_.u8(p++);flags.push_back(f);if(f&0x08U){const auto rep=r_.u8(p++);if(flags.size()+rep>pointCount)throw std::runtime_error("invalid glyph flag repeat");for(std::uint8_t n=0;n<rep;++n)flags.push_back(f);}}
        std::vector<std::int32_t>xs(pointCount),ys(pointCount);std::int32_t x=0,y=0;for(std::size_t i=0;i<pointCount;++i){const auto f=flags[i];if(f&0x02U){const auto d=r_.u8(p++);x+=(f&0x10U)?d:-static_cast<std::int32_t>(d);}else if(!(f&0x10U)){x+=r_.i16(p);p+=2U;}xs[i]=x;}for(std::size_t i=0;i<pointCount;++i){const auto f=flags[i];if(f&0x04U){const auto d=r_.u8(p++);y+=(f&0x20U)?d:-static_cast<std::int32_t>(d);}else if(!(f&0x20U)){y+=r_.i16(p);p+=2U;}ys[i]=y;}
        std::size_t first=0;for(auto end:endpoints){std::vector<RawPoint> pts;pts.reserve(end-first+1U);for(std::size_t i=first;i<=end;++i)pts.push_back({{xs[i]*inv,ys[i]*inv},(flags[i]&1U)!=0U});g.contours.push_back(to_curves(pts));first=static_cast<std::size_t>(end)+1U;}
    }
    [[nodiscard]] static Text3DContour to_curves(const std::vector<RawPoint>& pts){Text3DContour out;if(pts.empty())return out;Float2 start;if(pts.front().on)start=pts.front().p;else if(pts.back().on)start=pts.back().p;else start=midpoint(pts.back().p,pts.front().p);Float2 current=start;std::size_t i=pts.front().on?1U:0U;std::size_t consumed=0;while(consumed<pts.size()){const auto& p=pts[i%pts.size()];if(p.on){if(length_sq(sub(p.p,current))>1.0e-20F)out.curves.push_back({current,p.p,p.p});current=p.p;++i;++consumed;}else{const auto& n=pts[(i+1U)%pts.size()];if(n.on){out.curves.push_back({current,p.p,n.p});current=n.p;i+=2U;consumed+=2U;}else{const auto m=midpoint(p.p,n.p);out.curves.push_back({current,p.p,m});current=m;++i;++consumed;}}}if(length_sq(sub(current,start))>1.0e-20F)out.curves.push_back({current,start,start});return out;}
    void parse_composite(std::size_t o,std::uint32_t cp,float inv,Text3DGlyphGeometry& g,std::set<std::uint16_t>& stack,unsigned depth)const{
        constexpr std::uint16_t ARG_WORDS=0x0001,ARGS_XY=0x0002,SCALE=0x0008,MORE=0x0020,XY_SCALE=0x0040,TWO_BY_TWO=0x0080,INSTRUCTIONS=0x0100;std::size_t p=o+10U;std::uint16_t flags{};do{flags=r_.u16(p);const auto component=r_.u16(p+2U);p+=4U;std::int16_t a1{},a2{};if(flags&ARG_WORDS){a1=r_.i16(p);a2=r_.i16(p+2U);p+=4U;}else{a1=r_.i8(p);a2=r_.i8(p+1U);p+=2U;}float m00=1,m01=0,m10=0,m11=1;if(flags&SCALE){m00=m11=static_cast<float>(r_.i16(p))/16384.0F;p+=2U;}else if(flags&XY_SCALE){m00=static_cast<float>(r_.i16(p))/16384.0F;m11=static_cast<float>(r_.i16(p+2U))/16384.0F;p+=4U;}else if(flags&TWO_BY_TWO){m00=static_cast<float>(r_.i16(p))/16384.0F;m01=static_cast<float>(r_.i16(p+2U))/16384.0F;m10=static_cast<float>(r_.i16(p+4U))/16384.0F;m11=static_cast<float>(r_.i16(p+6U))/16384.0F;p+=8U;}float tx=0,ty=0;if(flags&ARGS_XY){tx=a1*inv;ty=a2*inv;}auto child=glyph_impl(component,cp,stack,depth+1U);for(auto& contour:child.contours){for(auto& c:contour.curves){const auto xf=[&](Float2 q){return Float2{m00*q.x+m01*q.y+tx,m10*q.x+m11*q.y+ty};};c.p1=xf(c.p1);c.p2=xf(c.p2);c.p3=xf(c.p3);}g.contours.push_back(std::move(contour));}}while(flags&MORE);if(flags&INSTRUCTIONS){const auto n=r_.u16(p);(void)n;}}
};

[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path,std::uint64_t maximum){std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)throw std::runtime_error("unable to open font file");const auto size=in.tellg();if(size<0||static_cast<std::uint64_t>(size)>maximum)throw std::runtime_error("font exceeds configured size limit");in.seekg(0);std::vector<std::byte> data(static_cast<std::size_t>(size));if(!data.empty()&&!in.read(reinterpret_cast<char*>(data.data()),size))throw std::runtime_error("unable to read font file");return data;}

[[nodiscard]] std::vector<std::uint32_t> decode_utf8(std::string_view text,std::uint32_t maximum){std::vector<std::uint32_t> out;out.reserve(std::min<std::size_t>(text.size(),maximum));for(std::size_t i=0;i<text.size();){const auto c=static_cast<std::uint8_t>(text[i]);std::uint32_t cp{};std::size_t n{};if(c<0x80U){cp=c;n=1;}else if((c&0xE0U)==0xC0U){cp=c&0x1FU;n=2;}else if((c&0xF0U)==0xE0U){cp=c&0x0FU;n=3;}else if((c&0xF8U)==0xF0U){cp=c&0x07U;n=4;}else throw std::runtime_error("invalid UTF-8 lead byte");if(i+n>text.size())throw std::runtime_error("truncated UTF-8 sequence");for(std::size_t j=1;j<n;++j){const auto d=static_cast<std::uint8_t>(text[i+j]);if((d&0xC0U)!=0x80U)throw std::runtime_error("invalid UTF-8 continuation byte");cp=(cp<<6U)|(d&0x3FU);}if((n==2&&cp<0x80U)||(n==3&&cp<0x800U)||(n==4&&cp<0x10000U)||cp>0x10FFFFU||(cp>=0xD800U&&cp<=0xDFFFU))throw std::runtime_error("non-canonical UTF-8 sequence");out.push_back(cp);if(out.size()>maximum)throw std::runtime_error("text exceeds codepoint limit");i+=n;}return out;}

struct CurveRef{std::uint16_t x{},y{};float maxSort{};};
[[nodiscard]] std::uint32_t append_curve(SlugAtlas& atlas,const Text3DQuadraticCurve& c){const auto index=static_cast<std::uint32_t>(atlas.curveTexels.size());atlas.curveTexels.push_back({c.p1.x,c.p1.y,c.p2.x,c.p2.y});atlas.curveTexels.push_back({c.p3.x,c.p3.y,0,0});return index;}
[[nodiscard]] std::array<std::uint16_t,2> curve_location(std::uint32_t index){const auto x=index%SlugAtlas::kTextureWidth,y=index/SlugAtlas::kTextureWidth;if(x>65534U||y>65535U)throw std::runtime_error("Slug curve texture exceeds RG16 location range");return {static_cast<std::uint16_t>(x),static_cast<std::uint16_t>(y)};}

[[nodiscard]] SlugGlyphInstance append_slug_glyph(SlugAtlas& atlas,const Text3DGlyphGeometry& glyph,Float2 origin,const Text3DStyle& style){
    SlugGlyphInstance instance;instance.codepoint=glyph.codepoint;instance.glyphIndex=glyph.glyphIndex;instance.originMeters=origin;instance.minimumEm=glyph.minimumEm;instance.maximumEm=glyph.maximumEm;instance.color=style.faceColor;
    const std::uint16_t hx=std::clamp<std::uint16_t>(style.horizontalBands,1,255),vy=std::clamp<std::uint16_t>(style.verticalBands,1,255);const float w=std::max(glyph.maximumEm.x-glyph.minimumEm.x,1.0e-5F),h=std::max(glyph.maximumEm.y-glyph.minimumEm.y,1.0e-5F);instance.bandTransform={hx/w,vy/h,-glyph.minimumEm.x*hx/w,-glyph.minimumEm.y*vy/h};
    std::vector<std::pair<Text3DQuadraticCurve,CurveRef>> curves;for(const auto& contour:glyph.contours)for(const auto& c:contour.curves){const auto idx=append_curve(atlas,c);const auto loc=curve_location(idx);curves.push_back({c,{loc[0],loc[1],0}});}const std::uint32_t glyphStart=static_cast<std::uint32_t>(atlas.bandTexels.size());const std::uint32_t descriptorCount=static_cast<std::uint32_t>(hx)+vy;atlas.bandTexels.resize(atlas.bandTexels.size()+descriptorCount);
    auto emit=[&](bool horizontal,std::uint16_t band){const float lo=(horizontal?glyph.minimumEm.y:glyph.minimumEm.x)+(horizontal?h:w)*band/(horizontal?hx:vy);const float hi=(horizontal?glyph.minimumEm.y:glyph.minimumEm.x)+(horizontal?h:w)*(band+1U)/(horizontal?hx:vy);std::vector<CurveRef> selected;for(const auto& [c,ref]:curves){const float minv=horizontal?std::min({c.p1.y,c.p2.y,c.p3.y}):std::min({c.p1.x,c.p2.x,c.p3.x});const float maxv=horizontal?std::max({c.p1.y,c.p2.y,c.p3.y}):std::max({c.p1.x,c.p2.x,c.p3.x});const bool parallel=horizontal?(std::abs(c.p1.y-c.p2.y)<1e-8F&&std::abs(c.p2.y-c.p3.y)<1e-8F):(std::abs(c.p1.x-c.p2.x)<1e-8F&&std::abs(c.p2.x-c.p3.x)<1e-8F);if(!parallel&&maxv+kBandEpsilon>=lo&&minv-kBandEpsilon<=hi){auto r=ref;r.maxSort=horizontal?std::max({c.p1.x,c.p2.x,c.p3.x}):std::max({c.p1.y,c.p2.y,c.p3.y});selected.push_back(r);}}std::sort(selected.begin(),selected.end(),[](const auto&a,const auto&b){return a.maxSort>b.maxSort;});if(selected.size()>65535U)throw std::runtime_error("Slug band contains too many curves");const auto listStart=static_cast<std::uint32_t>(atlas.bandTexels.size());const auto offset=listStart-glyphStart;if(offset>kMaximumBandOffset)throw std::runtime_error("Slug per-glyph band data exceeds RG16 offset range");const std::uint32_t descriptor=glyphStart+(horizontal?band:static_cast<std::uint32_t>(hx)+band);atlas.bandTexels[descriptor]={static_cast<std::uint16_t>(selected.size()),static_cast<std::uint16_t>(offset)};for(const auto& r:selected)atlas.bandTexels.push_back({r.x,r.y});};for(std::uint16_t b=0;b<hx;++b)emit(false,b); // vertical descriptors follow horizontal descriptors but x bands use vertical rays.
    for(std::uint16_t b=0;b<vy;++b)emit(true,b);
    const auto gx=glyphStart%SlugAtlas::kTextureWidth,gy=glyphStart/SlugAtlas::kTextureWidth;if(gy>65535U)throw std::runtime_error("Slug band texture exceeds coordinate range");instance.glyphData={static_cast<std::int32_t>(gx),static_cast<std::int32_t>(gy),static_cast<std::int32_t>(hx-1U),static_cast<std::int32_t>(vy-1U)|(style.fillRule==Text3DFillRule::EvenOdd?0x1000:0)};return instance;
}

void add_side_geometry(CookedPolygonAsset& mesh,const Text3DGlyphGeometry& glyph,Float2 origin,const Text3DStyle& style){const float z0=-style.extrusionDepthMeters*0.5F,z1=style.extrusionDepthMeters*0.5F,scale=style.emSizeMeters;for(const auto& contour:glyph.contours){std::vector<Float2> pts;for(const auto& c:contour.curves){for(std::uint16_t i=0;i<style.curveSubdivision;++i)pts.push_back(bezier(c,static_cast<float>(i)/style.curveSubdivision));}if(pts.size()<3U)continue;const bool ccw=signed_area(pts)>0.0F;for(std::size_t i=0;i<pts.size();++i){const auto a=pts[i],b=pts[(i+1U)%pts.size()];const auto d=normalized(sub(b,a));const Float2 n=ccw?Float2{d.y,-d.x}:Float2{-d.y,d.x};const auto base=static_cast<std::uint32_t>(mesh.vertices.size());const auto pos=[&](Float2 p,float z){return Float3{origin.x+p.x*scale,origin.y+p.y*scale,z};};const Float3 normal{n.x,n.y,0};for(auto [p,z,u,v]:std::array<std::tuple<Float2,float,float,float>,4>{{{a,z0,0,0},{b,z0,1,0},{b,z1,1,1},{a,z1,0,1}}}){PolygonVertex pv;pv.position=pos(p,z);pv.normal=normal;pv.tangent={d.x,d.y,0,1};pv.texcoord={u,v};pv.color=style.sideColor;mesh.vertices.push_back(pv);}mesh.indices.insert(mesh.indices.end(),{base,base+1U,base+2U,base,base+2U,base+3U});}}
}

[[nodiscard]] VoxelMaterialDefinition make_side_material(const Text3DStyle& style){VoxelMaterialDefinition m;m.name="Engine/Materials/Text3DSide";m.baseColor=style.sideColor;m.metallic=0.0F;m.roughness=0.58F;m.specular=kStandardSurfaceSpecular;m.shadingModel=MaterialShadingModel::StandardPBR;m.blendMode=MaterialBlendMode::Opaque;return m;}
void finalize_side_mesh(CookedPolygonAsset& mesh){mesh.submeshes.clear();if(!mesh.indices.empty())mesh.submeshes.push_back({"Text3D sides",0U,static_cast<std::uint32_t>(mesh.indices.size()),0U});if(mesh.vertices.empty()){mesh.bounds={{0,0,0},{0,0,0}};}else{mesh.bounds.minimum=mesh.bounds.maximum=mesh.vertices.front().position;for(const auto& v:mesh.vertices){mesh.bounds.minimum.x=std::min(mesh.bounds.minimum.x,v.position.x);mesh.bounds.minimum.y=std::min(mesh.bounds.minimum.y,v.position.y);mesh.bounds.minimum.z=std::min(mesh.bounds.minimum.z,v.position.z);mesh.bounds.maximum.x=std::max(mesh.bounds.maximum.x,v.position.x);mesh.bounds.maximum.y=std::max(mesh.bounds.maximum.y,v.position.y);mesh.bounds.maximum.z=std::max(mesh.bounds.maximum.z,v.position.z);}}mesh.contentHash=polygon_asset_content_hash(mesh);}

class LeWriter{public:void u8(std::uint8_t v){d.push_back(static_cast<std::byte>(v));}void u16(std::uint16_t v){u8(v);u8(v>>8U);}void i16(std::int16_t v){u16(std::bit_cast<std::uint16_t>(v));}void u32(std::uint32_t v){for(unsigned s=0;s<32;s+=8)u8(v>>s);}void i32(std::int32_t v){u32(std::bit_cast<std::uint32_t>(v));}void u64(std::uint64_t v){for(unsigned s=0;s<64;s+=8)u8(v>>s);}void f(float v){u32(std::bit_cast<std::uint32_t>(v));}void bytes(const void*p,std::size_t n){const auto*b=static_cast<const std::byte*>(p);d.insert(d.end(),b,b+static_cast<std::ptrdiff_t>(n));}void str(std::string_view s){if(s.size()>1U<<24U)throw std::runtime_error("DTEXT string limit exceeded");u32(static_cast<std::uint32_t>(s.size()));bytes(s.data(),s.size());}std::vector<std::byte>d;};
class LeReader{public:explicit LeReader(std::span<const std::byte>s):s_(s){}void req(std::size_t n)const{if(n>s_.size()-p_)throw std::runtime_error("truncated DTEXT file");}std::uint8_t u8(){req(1);return std::to_integer<std::uint8_t>(s_[p_++]);}std::uint16_t u16(){const auto a=u8(),b=u8();return static_cast<std::uint16_t>(a|(b<<8U));}std::int16_t i16(){return std::bit_cast<std::int16_t>(u16());}std::uint32_t u32(){std::uint32_t v=0;for(unsigned q=0;q<32;q+=8)v|=static_cast<std::uint32_t>(u8())<<q;return v;}std::int32_t i32(){return std::bit_cast<std::int32_t>(u32());}std::uint64_t u64(){std::uint64_t v=0;for(unsigned q=0;q<64;q+=8)v|=static_cast<std::uint64_t>(u8())<<q;return v;}float f(){const auto v=std::bit_cast<float>(u32());if(!finite(v))throw std::runtime_error("DTEXT contains non-finite float");return v;}std::string str(){const auto n=u32();req(n);std::string x(reinterpret_cast<const char*>(s_.data()+p_),n);p_+=n;return x;}void bytes(void*out,std::size_t n){req(n);std::memcpy(out,s_.data()+p_,n);p_+=n;}std::size_t remaining()const{return s_.size()-p_;}private:std::span<const std::byte>s_;std::size_t p_{};};
void wf2(LeWriter&w,Float2 v){w.f(v.x);w.f(v.y);}void wf3(LeWriter&w,Float3 v){w.f(v.x);w.f(v.y);w.f(v.z);}void wf4(LeWriter&w,Float4 v){w.f(v.x);w.f(v.y);w.f(v.z);w.f(v.w);}Float2 rf2(LeReader&r){return {r.f(),r.f()};}Float3 rf3(LeReader&r){return {r.f(),r.f(),r.f()};}Float4 rf4(LeReader&r){return {r.f(),r.f(),r.f(),r.f()};}

[[nodiscard]] float calc_coverage_cpu(const CookedText3DAsset& asset,const SlugGlyphInstance& g,Float2 coord,float pixelsPerEm){
    const auto texel=[&](std::int32_t x,std::int32_t y)->SlugBandTexel{const auto idx=static_cast<std::size_t>(y)*SlugAtlas::kTextureWidth+static_cast<std::size_t>(x);return idx<asset.atlas.bandTexels.size()?asset.atlas.bandTexels[idx]:SlugBandTexel{};};const auto curve=[&](SlugBandTexel loc)->Text3DQuadraticCurve{const auto idx=static_cast<std::size_t>(loc.y)*SlugAtlas::kTextureWidth+loc.x;if(idx+1U>=asset.atlas.curveTexels.size())return {};const auto a=asset.atlas.curveTexels[idx],b=asset.atlas.curveTexels[idx+1U];return {{a.x,a.y},{a.z,a.w},{b.x,b.y}};};const auto roots=[&](const Text3DQuadraticCurve& c,bool horizontal){const float a1=horizontal?c.p1.y:c.p1.x,a2=horizontal?c.p2.y:c.p2.x,a3=horizontal?c.p3.y:c.p3.x;const float aa=a1-2*a2+a3,bb=a1-a2,cc=a1;std::array<float,2>t{};if(std::abs(aa)<1.0F/65536.0F){const float v=std::abs(bb)>1e-12F?cc/(2*bb):0;t={v,v};}else{const float d=std::sqrt(std::max(bb*bb-aa*cc,0.0F));t={(bb-d)/aa,(bb+d)/aa};}return t;};const int maxX=g.glyphData[2],maxY=g.glyphData[3]&0xFF;const int bx=std::clamp(static_cast<int>(coord.x*g.bandTransform.x+g.bandTransform.z),0,maxX),by=std::clamp(static_cast<int>(coord.y*g.bandTransform.y+g.bandTransform.w),0,maxY);const int gx=g.glyphData[0],gy=g.glyphData[1];float xcov=0,xw=0,ycov=0,yw=0;const auto process=[&](bool horizontal,int band,float&cov,float&w){const auto desc=texel(gx+(horizontal?band:maxY+1+band),gy);std::uint32_t linear=static_cast<std::uint32_t>(gy)*SlugAtlas::kTextureWidth+gx+desc.y;for(std::uint16_t i=0;i<desc.x;++i){const auto loc=asset.atlas.bandTexels[linear+i];auto c=curve(loc);c.p1=sub(c.p1,coord);c.p2=sub(c.p2,coord);c.p3=sub(c.p3,coord);const float sorted=horizontal?std::max({c.p1.x,c.p2.x,c.p3.x}):std::max({c.p1.y,c.p2.y,c.p3.y});if(sorted*pixelsPerEm<-0.5F)break;const auto t=roots(c,horizontal);for(int k=0;k<2;++k){if(t[k]<0||t[k]>1)continue;const float axis=horizontal?bezier(c,t[k]).x:bezier(c,t[k]).y;const float other=horizontal?bezier(c,t[k]).y:bezier(c,t[k]).x;const float derivative=horizontal?2*((1-t[k])*(c.p2.y-c.p1.y)+t[k]*(c.p3.y-c.p2.y)):2*((1-t[k])*(c.p2.x-c.p1.x)+t[k]*(c.p3.x-c.p2.x));if(std::abs(other)>1e-3F)continue;const float contribution=std::clamp(axis*pixelsPerEm+0.5F,0.0F,1.0F);if(horizontal)cov+=derivative>0?contribution:-contribution;else cov+=derivative>0?-contribution:contribution;w=std::max(w,std::clamp(1.0F-std::abs(axis*pixelsPerEm)*2.0F,0.0F,1.0F));}}};process(true,by,xcov,xw);process(false,bx,ycov,yw);float coverage=std::max(std::abs(xcov*xw+ycov*yw)/std::max(xw+yw,1.0F/65536.0F),std::min(std::abs(xcov),std::abs(ycov)));if((g.glyphData[3]&0x1000)==0)coverage=std::clamp(coverage,0.0F,1.0F);else coverage=1.0F-std::abs(1.0F-std::fmod(std::abs(coverage)*0.5F,1.0F)*2.0F);return coverage;
}

} // namespace

Text3DCookResult cook_text3d(const std::filesystem::path& fontPath,std::string_view textUtf8,const Text3DCookOptions& options){Text3DCookResult result;try{
    if(!(options.style.emSizeMeters>0)||!finite(options.style.emSizeMeters))throw std::runtime_error("invalid text em size");if(!(options.style.extrusionDepthMeters>=0)||!finite(options.style.extrusionDepthMeters)||options.style.horizontalBands==0||options.style.verticalBands==0||options.style.curveSubdivision<2U)throw std::runtime_error("invalid 3D text style");const auto codepoints=decode_utf8(textUtf8,options.maximumCodepoints);auto bytes=read_file(fontPath,options.maximumFontBytes);TrueTypeFont font(std::move(bytes));result.asset.objectId=options.objectId;result.asset.textUtf8=std::string(textUtf8);result.asset.style=options.style;result.asset.font=font.info();result.asset.sideMesh.objectId=options.objectId;result.asset.sideMesh.materials={make_side_material(options.style)};result.asset.sideMesh.materialBindings.resize(1);
    std::map<std::uint16_t,Text3DGlyphGeometry> glyphs;std::vector<std::uint16_t> stream;stream.reserve(codepoints.size());for(auto cp:codepoints){if(cp=='\n'){stream.push_back(0xFFFFU);continue;}auto gid=font.glyph_for(cp);if(!gid||*gid==0U){if(options.replaceMissingGlyphs){gid=font.glyph_for(options.replacementCodepoint);if(!gid||*gid==0U)gid=font.glyph_for('?');result.warnings.push_back("missing glyph replaced for U+"+std::to_string(cp));}if(!gid||*gid==0U)throw std::runtime_error("font is missing a requested glyph");}stream.push_back(*gid);if(!glyphs.contains(*gid)){if(glyphs.size()>=options.maximumUniqueGlyphs)throw std::runtime_error("unique glyph limit exceeded");glyphs.emplace(*gid,font.glyph(*gid,cp));}}
    std::vector<float> lineWidths{0};std::size_t line=0;std::optional<std::uint16_t> previous;for(auto gid:stream){if(gid==0xFFFFU){lineWidths.push_back(0);++line;previous.reset();continue;}const auto& g=glyphs.at(gid);if(previous)lineWidths[line]+=font.kerning(*previous,gid)/static_cast<float>(font.info().unitsPerEm);lineWidths[line]+=g.advanceEm+options.style.letterSpacingEm;previous=gid;}
    float x=0,y=0;line=0;previous.reset();auto aligned=[&](std::size_t l){if(options.style.alignment==Text3DHorizontalAlignment::Center)return -lineWidths[l]*0.5F;if(options.style.alignment==Text3DHorizontalAlignment::Right)return -lineWidths[l];return 0.0F;};x=aligned(0);std::uint64_t curveCount=0;for(auto gid:stream){if(gid==0xFFFFU){++line;x=aligned(line);y-=options.style.lineSpacingEm;previous.reset();continue;}auto g=glyphs.at(gid);if(previous)x+=font.kerning(*previous,gid)/static_cast<float>(font.info().unitsPerEm);const Float2 origin{x*options.style.emSizeMeters,y*options.style.emSizeMeters};if(!g.contours.empty()){for(const auto& c:g.contours)curveCount+=c.curves.size();if(curveCount>options.maximumCurves)throw std::runtime_error("text curve limit exceeded");result.asset.glyphInstances.push_back(append_slug_glyph(result.asset.atlas,g,origin,options.style));add_side_geometry(result.asset.sideMesh,g,origin,options.style);}x+=g.advanceEm+options.style.letterSpacingEm;previous=gid;}
    if(result.asset.glyphInstances.empty())throw std::runtime_error("text contains no renderable glyphs");
    for(auto& [id,g]:glyphs)result.asset.glyphGeometry.push_back(std::move(g));finalize_side_mesh(result.asset.sideMesh);result.asset.atlas.curveTextureHeight=static_cast<std::uint32_t>((result.asset.atlas.curveTexels.size()+SlugAtlas::kTextureWidth-1U)/SlugAtlas::kTextureWidth);result.asset.atlas.bandTextureHeight=static_cast<std::uint32_t>((result.asset.atlas.bandTexels.size()+SlugAtlas::kTextureWidth-1U)/SlugAtlas::kTextureWidth);if(result.asset.atlas.curveTextureHeight>kMaximumTextureHeight||result.asset.atlas.bandTextureHeight>kMaximumTextureHeight)throw std::runtime_error("Slug texture height exceeds format limit");result.asset.bounds=result.asset.sideMesh.bounds;if(result.asset.sideMesh.vertices.empty()){result.asset.bounds={{0,0,-options.style.extrusionDepthMeters*0.5F},{0,0,options.style.extrusionDepthMeters*0.5F}};}result.asset.contentHash=text3d_content_hash(result.asset);std::string error;if(!validate_text3d_asset(result.asset,&error))throw std::runtime_error(error);return result;
}catch(const std::exception&e){result.code=Text3DErrorCode::InvalidAsset;result.error=e.what();return result;}}

Text3DRenderPacket build_text3d_render_packet(const CookedText3DAsset& asset) {
    Text3DRenderPacket packet;
    packet.faceVertices.reserve(asset.glyphInstances.size() * 8U);
    packet.faceIndices.reserve(asset.glyphInstances.size() * 12U);
    const float scale = asset.style.emSizeMeters;
    const Float4 jacobian{1.0F / scale, 0.0F, 0.0F, 1.0F / scale};
    for (const auto& glyph : asset.glyphInstances) {
        const float x0 = glyph.originMeters.x + glyph.minimumEm.x * scale;
        const float x1 = glyph.originMeters.x + glyph.maximumEm.x * scale;
        const float y0 = glyph.originMeters.y + glyph.minimumEm.y * scale;
        const float y1 = glyph.originMeters.y + glyph.maximumEm.y * scale;
        const std::array<Float2,4> normals{{{-1,-1},{1,-1},{1,1},{-1,1}}};
        const std::array<Float2,4> coords{{{glyph.minimumEm.x,glyph.minimumEm.y},{glyph.maximumEm.x,glyph.minimumEm.y},{glyph.maximumEm.x,glyph.maximumEm.y},{glyph.minimumEm.x,glyph.maximumEm.y}}};
        const std::array<Float2,4> positions{{{x0,y0},{x1,y0},{x1,y1},{x0,y1}}};
        const std::array<std::uint32_t,4> data{{static_cast<std::uint32_t>(glyph.glyphData[0]),static_cast<std::uint32_t>(glyph.glyphData[1]),static_cast<std::uint32_t>(glyph.glyphData[2]),static_cast<std::uint32_t>(glyph.glyphData[3])}};
        for (int face=0; face<2; ++face) {
            const float z = (face==0?1.0F:-1.0F) * asset.style.extrusionDepthMeters * 0.5F;
            const auto base = static_cast<std::uint32_t>(packet.faceVertices.size());
            for (std::size_t i=0;i<4U;++i) packet.faceVertices.push_back({{positions[i].x,positions[i].y,z},normals[i],coords[i],jacobian,glyph.bandTransform,data,glyph.color});
            if (face==0) packet.faceIndices.insert(packet.faceIndices.end(),{base,base+1U,base+2U,base,base+2U,base+3U});
            else packet.faceIndices.insert(packet.faceIndices.end(),{base,base+2U,base+1U,base,base+3U,base+2U});
        }
    }
    std::uint64_t h=kFnvOffset;
    for(const auto&v:packet.faceVertices){hash_f3(h,v.position);hash_f2(h,v.dilationNormal);hash_f2(h,v.renderCoordinate);hash_f4(h,v.inverseJacobian);hash_f4(h,v.bandTransform);for(auto d:v.glyphData)hash_scalar(h,d);hash_f4(h,v.color);}
    for(auto i:packet.faceIndices)hash_scalar(h,i);
    packet.contentHash=h;
    return packet;
}

namespace {
std::uint64_t text3d_content_hash_impl(const CookedText3DAsset& a, bool includeMaterialIds) noexcept {
    std::uint64_t h=kFnvOffset;
    hash_scalar(h,a.objectId);
    hash_string(h,a.textUtf8);
    hash_scalar(h,a.style.emSizeMeters);
    hash_scalar(h,a.style.extrusionDepthMeters);
    hash_scalar(h,a.style.letterSpacingEm);
    hash_scalar(h,a.style.lineSpacingEm);
    hash_scalar(h,a.style.horizontalBands);
    hash_scalar(h,a.style.verticalBands);
    hash_scalar(h,a.style.curveSubdivision);
    hash_scalar(h,static_cast<std::uint8_t>(a.style.alignment));
    hash_scalar(h,static_cast<std::uint8_t>(a.style.fillRule));
    if (includeMaterialIds) {
        hash_scalar(h,a.style.faceMaterialId);
        hash_scalar(h,a.style.sideMaterialId);
    }
    hash_f4(h,a.style.faceColor);
    hash_f4(h,a.style.sideColor);
    hash_string(h,a.font.family);
    hash_string(h,a.font.subfamily);
    hash_scalar(h,a.font.unitsPerEm);
    hash_scalar(h,a.font.sourceHash);
    for(const auto&g:a.glyphInstances){
        hash_scalar(h,g.codepoint);hash_scalar(h,g.glyphIndex);hash_f2(h,g.originMeters);
        hash_f2(h,g.minimumEm);hash_f2(h,g.maximumEm);hash_f4(h,g.bandTransform);
        for(auto v:g.glyphData)hash_scalar(h,v);hash_f4(h,g.color);
    }
    for(const auto&t:a.atlas.curveTexels)hash_f4(h,t);
    for(const auto&t:a.atlas.bandTexels){hash_scalar(h,t.x);hash_scalar(h,t.y);}
    hash_scalar(h,a.sideMesh.contentHash);
    hash_f3(h,a.bounds.minimum);hash_f3(h,a.bounds.maximum);
    return h;
}
}

std::uint64_t text3d_content_hash(const CookedText3DAsset& a) noexcept {
    return text3d_content_hash_impl(a, true);
}

bool validate_text3d_asset(const CookedText3DAsset&a,std::string*error)noexcept{const auto fail=[&](std::string m){if(error)*error=std::move(m);return false;};if(a.objectId==0)return fail("DTEXT object ID must be nonzero");if(!(a.style.emSizeMeters>0)||a.style.extrusionDepthMeters<0||a.style.faceMaterialId==0||a.style.sideMaterialId==0||!finite(a.style.faceColor)||!finite(a.style.sideColor))return fail("DTEXT style is invalid");if(a.font.unitsPerEm==0)return fail("DTEXT font units-per-em is zero");if(a.atlas.curveTexels.size()%2U!=0U)return fail("DTEXT curve texture must store two texels per curve");if(a.atlas.curveTextureHeight!=(a.atlas.curveTexels.size()+SlugAtlas::kTextureWidth-1U)/SlugAtlas::kTextureWidth)return fail("DTEXT curve texture height drift");if(a.atlas.bandTextureHeight!=(a.atlas.bandTexels.size()+SlugAtlas::kTextureWidth-1U)/SlugAtlas::kTextureWidth)return fail("DTEXT band texture height drift");for(const auto&g:a.glyphInstances){if(g.glyphData[0]<0||g.glyphData[1]<0||static_cast<std::uint32_t>(g.glyphData[1])>=a.atlas.bandTextureHeight)return fail("DTEXT glyph band location is invalid");if(!finite(g.originMeters)||!finite(g.minimumEm)||!finite(g.maximumEm)||!finite(g.bandTransform))return fail("DTEXT glyph contains non-finite values");}const auto side=validate_polygon_asset(a.sideMesh);if(!side)return fail("DTEXT side mesh invalid: "+side.message);if(text3d_content_hash(a)!=a.contentHash)return fail("DTEXT content hash mismatch");return true;}

bool write_dtext(const std::filesystem::path&path,const CookedText3DAsset&input,std::string*error){try{auto a=input;a.contentHash=text3d_content_hash(a);std::string validation;if(!validate_text3d_asset(a,&validation))throw std::runtime_error(validation);LeWriter w;w.bytes(kTextMagic.data(),kTextMagic.size());w.u16(kTextMajor);w.u16(kTextMinor);w.u64(a.objectId);w.u64(a.contentHash);w.str(a.textUtf8);w.str(a.font.family);w.str(a.font.subfamily);w.u16(a.font.unitsPerEm);w.i16(a.font.ascender);w.i16(a.font.descender);w.i16(a.font.lineGap);w.u16(a.font.glyphCount);w.u64(a.font.sourceHash);w.f(a.style.emSizeMeters);w.f(a.style.extrusionDepthMeters);w.f(a.style.letterSpacingEm);w.f(a.style.lineSpacingEm);w.u16(a.style.horizontalBands);w.u16(a.style.verticalBands);w.u16(a.style.curveSubdivision);w.u8(static_cast<std::uint8_t>(a.style.alignment));w.u8(static_cast<std::uint8_t>(a.style.fillRule));w.u32(a.style.faceMaterialId);w.u32(a.style.sideMaterialId);wf4(w,a.style.faceColor);wf4(w,a.style.sideColor);w.u32(static_cast<std::uint32_t>(a.glyphInstances.size()));w.u32(static_cast<std::uint32_t>(a.atlas.curveTexels.size()));w.u32(static_cast<std::uint32_t>(a.atlas.bandTexels.size()));w.u64(a.sideMesh.vertices.size());w.u64(a.sideMesh.indices.size());wf3(w,a.bounds.minimum);wf3(w,a.bounds.maximum);for(const auto&g:a.glyphInstances){w.u32(g.codepoint);w.u16(g.glyphIndex);w.u16(0);wf2(w,g.originMeters);wf2(w,g.minimumEm);wf2(w,g.maximumEm);wf4(w,g.bandTransform);for(auto v:g.glyphData)w.i32(v);wf4(w,g.color);}for(auto t:a.atlas.curveTexels)wf4(w,t);for(auto t:a.atlas.bandTexels){w.u16(t.x);w.u16(t.y);}for(const auto&v:a.sideMesh.vertices){wf3(w,v.position);wf3(w,v.normal);wf4(w,v.tangent);wf2(w,v.texcoord);wf2(w,v.texcoord1);wf4(w,v.color);}for(auto i:a.sideMesh.indices)w.u32(i);std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("unable to create DTEXT file");if(!w.d.empty()&&!out.write(reinterpret_cast<const char*>(w.d.data()),static_cast<std::streamsize>(w.d.size())))throw std::runtime_error("unable to write DTEXT file");return true;}catch(const std::exception&e){if(error)*error=e.what();return false;}}

Text3DReadResult read_dtext(const std::filesystem::path&path,std::uint64_t maximumBytes){Text3DReadResult result;try{auto data=read_file(path,maximumBytes);LeReader r(data);std::array<char,8>magic{};r.bytes(magic.data(),magic.size());if(magic!=kTextMagic)throw std::runtime_error("DTEXT magic mismatch");const auto major=r.u16(),minor=r.u16();if(major!=kTextMajor||minor>kTextMinor){result.code=Text3DErrorCode::UnsupportedVersion;result.error="unsupported DTEXT version";return result;}auto&a=result.asset;a.objectId=r.u64();const auto stored=r.u64();a.textUtf8=r.str();a.font.family=r.str();a.font.subfamily=r.str();a.font.unitsPerEm=r.u16();a.font.ascender=r.i16();a.font.descender=r.i16();a.font.lineGap=r.i16();a.font.glyphCount=r.u16();a.font.sourceHash=r.u64();a.style.emSizeMeters=r.f();a.style.extrusionDepthMeters=r.f();a.style.letterSpacingEm=r.f();a.style.lineSpacingEm=r.f();a.style.horizontalBands=r.u16();a.style.verticalBands=r.u16();a.style.curveSubdivision=r.u16();a.style.alignment=static_cast<Text3DHorizontalAlignment>(r.u8());a.style.fillRule=static_cast<Text3DFillRule>(r.u8());if(minor>=1){a.style.faceMaterialId=r.u32();a.style.sideMaterialId=r.u32();}a.style.faceColor=rf4(r);a.style.sideColor=rf4(r);const auto glyphCount=r.u32(),curveCount=r.u32(),bandCount=r.u32();const auto vertexCount=r.u64(),indexCount=r.u64();if(glyphCount>1U<<20U||curveCount>1U<<25U||bandCount>1U<<26U||vertexCount>1U<<26U||indexCount>1U<<28U)throw std::runtime_error("DTEXT count limit exceeded");a.bounds.minimum=rf3(r);a.bounds.maximum=rf3(r);a.glyphInstances.resize(glyphCount);for(auto&g:a.glyphInstances){g.codepoint=r.u32();g.glyphIndex=r.u16();(void)r.u16();g.originMeters=rf2(r);g.minimumEm=rf2(r);g.maximumEm=rf2(r);g.bandTransform=rf4(r);for(auto&v:g.glyphData)v=r.i32();g.color=rf4(r);}a.atlas.curveTexels.resize(curveCount);for(auto&t:a.atlas.curveTexels)t=rf4(r);a.atlas.bandTexels.resize(bandCount);for(auto&t:a.atlas.bandTexels){t.x=r.u16();t.y=r.u16();}a.atlas.curveTextureHeight=static_cast<std::uint32_t>((curveCount+SlugAtlas::kTextureWidth-1U)/SlugAtlas::kTextureWidth);a.atlas.bandTextureHeight=static_cast<std::uint32_t>((bandCount+SlugAtlas::kTextureWidth-1U)/SlugAtlas::kTextureWidth);a.sideMesh.objectId=a.objectId;a.sideMesh.materials={make_side_material(a.style)};a.sideMesh.materialBindings.resize(1);a.sideMesh.vertices.resize(vertexCount);for(auto&v:a.sideMesh.vertices){v.position=rf3(r);v.normal=rf3(r);v.tangent=rf4(r);v.texcoord=rf2(r);v.texcoord1=rf2(r);v.color=rf4(r);}a.sideMesh.indices.resize(indexCount);for(auto&i:a.sideMesh.indices)i=r.u32();if(r.remaining())throw std::runtime_error("DTEXT has trailing data");finalize_side_mesh(a.sideMesh);
        if (minor == 0U) {
            if (text3d_content_hash_impl(a, false) != stored) {
                result.code=Text3DErrorCode::HashMismatch;
                result.error="DTEXT legacy content hash mismatch";
                return result;
            }
            a.contentHash=text3d_content_hash(a);
        } else {
            a.contentHash=stored;
        }
        std::string validation;if(!validate_text3d_asset(a,&validation)){result.code=validation.find("hash")!=std::string::npos?Text3DErrorCode::HashMismatch:Text3DErrorCode::InvalidAsset;result.error=validation;return result;}return result;}catch(const std::exception&e){result.code=Text3DErrorCode::InvalidAsset;result.error=e.what();return result;}}

bool write_text3d_preview_ppm(const std::filesystem::path&path,const CookedText3DAsset&a,const Text3DPreviewOptions&o,std::string*error){try{std::string validation;if(!validate_text3d_asset(a,&validation))throw std::runtime_error(validation);if(o.width<16||o.height<16||!(o.pixelsPerMeter>0))throw std::runtime_error("invalid preview dimensions");std::vector<Float4>pixels(static_cast<std::size_t>(o.width)*o.height,o.background);std::vector<float>depth(pixels.size(),-std::numeric_limits<float>::infinity());const float cx=o.width*0.5F-(a.bounds.minimum.x+a.bounds.maximum.x)*0.5F*o.pixelsPerMeter,cy=o.height*0.58F+(a.bounds.minimum.y+a.bounds.maximum.y)*0.5F*o.pixelsPerMeter;const auto project=[&](Float3 p){return Float3{cx+(p.x+p.z*o.extrusionScreenOffset)*o.pixelsPerMeter,cy-(p.y+p.z*o.extrusionScreenOffset)*o.pixelsPerMeter,p.z+p.x*0.001F-p.y*0.001F};};const auto blend=[&](std::size_t idx,Float4 c,float alpha,float z){if(z<depth[idx])return;depth[idx]=z;pixels[idx]={pixels[idx].x*(1-alpha)+c.x*alpha,pixels[idx].y*(1-alpha)+c.y*alpha,pixels[idx].z*(1-alpha)+c.z*alpha,1};};for(std::size_t ti=0;ti+2U<a.sideMesh.indices.size();ti+=3U){const auto p0=project(a.sideMesh.vertices[a.sideMesh.indices[ti]].position),p1=project(a.sideMesh.vertices[a.sideMesh.indices[ti+1U]].position),p2=project(a.sideMesh.vertices[a.sideMesh.indices[ti+2U]].position);const float minX=std::floor(std::min({p0.x,p1.x,p2.x})),maxX=std::ceil(std::max({p0.x,p1.x,p2.x})),minY=std::floor(std::min({p0.y,p1.y,p2.y})),maxY=std::ceil(std::max({p0.y,p1.y,p2.y}));const float area=(p1.x-p0.x)*(p2.y-p0.y)-(p1.y-p0.y)*(p2.x-p0.x);if(std::abs(area)<1e-8F)continue;const auto n=a.sideMesh.vertices[a.sideMesh.indices[ti]].normal;const float light=0.25F+0.75F*std::max(0.0F,n.x*o.lightDirection.x+n.y*o.lightDirection.y+n.z*o.lightDirection.z);const Float4 c{a.style.sideColor.x*light,a.style.sideColor.y*light,a.style.sideColor.z*light,a.style.sideColor.w};for(int y=std::max(0,static_cast<int>(minY));y<std::min(static_cast<int>(o.height),static_cast<int>(maxY)+1);++y)for(int x=std::max(0,static_cast<int>(minX));x<std::min(static_cast<int>(o.width),static_cast<int>(maxX)+1);++x){const float px=x+0.5F,py=y+0.5F;const float w0=((p1.x-px)*(p2.y-py)-(p1.y-py)*(p2.x-px))/area,w1=((p2.x-px)*(p0.y-py)-(p2.y-py)*(p0.x-px))/area,w2=1-w0-w1;if(w0>=-1e-5F&&w1>=-1e-5F&&w2>=-1e-5F)blend(static_cast<std::size_t>(y)*o.width+x,c,c.w,w0*p0.z+w1*p1.z+w2*p2.z);}}
        const float z=a.style.extrusionDepthMeters*0.5F;for(const auto&g:a.glyphInstances){const Float3 lo=project({g.originMeters.x+g.minimumEm.x*a.style.emSizeMeters,g.originMeters.y+g.minimumEm.y*a.style.emSizeMeters,z}),hi=project({g.originMeters.x+g.maximumEm.x*a.style.emSizeMeters,g.originMeters.y+g.maximumEm.y*a.style.emSizeMeters,z});const int x0=std::max(0,static_cast<int>(std::floor(std::min(lo.x,hi.x)))-2),x1=std::min(static_cast<int>(o.width)-1,static_cast<int>(std::ceil(std::max(lo.x,hi.x)))+2),y0=std::max(0,static_cast<int>(std::floor(std::min(lo.y,hi.y)))-2),y1=std::min(static_cast<int>(o.height)-1,static_cast<int>(std::ceil(std::max(lo.y,hi.y)))+2);for(int sy=y0;sy<=y1;++sy)for(int sx=x0;sx<=x1;++sx){const float wx=(sx+0.5F-cx)/o.pixelsPerMeter-z*o.extrusionScreenOffset,wy=(cy-(sy+0.5F))/o.pixelsPerMeter-z*o.extrusionScreenOffset;const Float2 em{(wx-g.originMeters.x)/a.style.emSizeMeters,(wy-g.originMeters.y)/a.style.emSizeMeters};const float cov=calc_coverage_cpu(a,g,em,a.style.emSizeMeters*o.pixelsPerMeter);if(cov>0.001F)blend(static_cast<std::size_t>(sy)*o.width+sx,a.style.faceColor,cov*a.style.faceColor.w,z);}}
        std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("unable to create text preview");out<<"P6\n"<<o.width<<' '<<o.height<<"\n255\n";for(auto c:pixels){const auto convert=[](float v){v=std::clamp(v,0.0F,1.0F);v=v<=0.0031308F?12.92F*v:1.055F*std::pow(v,1.0F/2.4F)-0.055F;return static_cast<unsigned char>(std::clamp(std::lround(v*255.0F),0L,255L));};const std::array<unsigned char,3>rgb{convert(c.x),convert(c.y),convert(c.z)};out.write(reinterpret_cast<const char*>(rgb.data()),3);}return static_cast<bool>(out);}catch(const std::exception&e){if(error)*error=e.what();return false;}}

} // namespace dve
