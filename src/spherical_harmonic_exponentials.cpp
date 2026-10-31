#include "dve/spherical_harmonic_exponentials.hpp"
#include "dve/environment_lighting.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace dve {
namespace {
constexpr double kPi = 3.1415926535897932384626433832795;
using Row = std::array<double, kSheCoefficientCount>;
using Matrix = std::array<Row, kSheCoefficientCount>;
using Channels = std::array<std::array<double, 3>, kSheCoefficientCount>;

Float3 she_add(Float3 a, Float3 b) noexcept { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Float3 sub(Float3 a, Float3 b) noexcept { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Float3 scale(Float3 a, float s) noexcept { return {a.x*s, a.y*s, a.z*s}; }
float she_dot(Float3 a, Float3 b) noexcept { return a.x*b.x+a.y*b.y+a.z*b.z; }
Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
bool finite(Float3 a) noexcept {
    return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);
}
bool direction(Float3 a) noexcept { return finite(a)&&she_dot(a,a)>1.0e-12F; }
Float3 unit(Float3 a) noexcept { return scale(a,1.0F/std::sqrt(she_dot(a,a))); }
Float3 reflect(Float3 v, Float3 n) noexcept { return sub(v,scale(n,2.0F*she_dot(v,n))); }
float fract(float x) noexcept { return x-std::floor(x); }

// Orthonormal real SH, ordered by increasing degree and m=-l,...,+l.
// Keep these polynomials in sync with shaders/common/spherical_harmonic_exponentials.hlsli.
std::array<double,25> sh4(Float3 dir) noexcept {
    const double x=dir.x, y=dir.y, z=dir.z;
    const double xx=x*x, yy=y*y, zz=z*z;
    return {{
        0.28209479177387814,
        0.4886025119029199*y, 0.4886025119029199*z, 0.4886025119029199*x,
        1.0925484305920792*x*y, 1.0925484305920792*y*z,
        0.31539156525252005*(3.0*zz-1.0), 1.0925484305920792*x*z,
        0.5462742152960396*(xx-yy),
        0.5900435899266435*y*(3.0*xx-yy), 2.890611442640554*x*y*z,
        0.4570457994644658*y*(5.0*zz-1.0),
        0.3731763325901154*z*(5.0*zz-3.0),
        0.4570457994644658*x*(5.0*zz-1.0),
        1.445305721320277*z*(xx-yy),
        0.5900435899266435*x*(xx-3.0*yy),
        2.5033429417967046*x*y*(xx-yy),
        1.7701307697799304*y*z*(3.0*xx-yy),
        0.9461746957575601*x*y*(7.0*zz-1.0),
        0.6690465435572892*y*z*(7.0*zz-3.0),
        0.10578554691520431*(35.0*zz*zz-30.0*zz+3.0),
        0.6690465435572892*x*z*(7.0*zz-3.0),
        0.47308734787878004*(xx-yy)*(7.0*zz-1.0),
        1.7701307697799304*x*z*(xx-3.0*yy),
        0.6258357354491761*(xx*xx-6.0*xx*yy+yy*yy)
    }};
}

std::optional<Row> make_row(Float3 n, Float3 v, float alpha) noexcept {
    if(!direction(n)||!direction(v)||!std::isfinite(alpha)||
       alpha<kSheMinimumAlpha||alpha>1.0F)return std::nullopt;
    n=unit(n);v=unit(v);
    if(she_dot(n,v)<1.0e-4F)return std::nullopt;
    const Float3 r=reflect(scale(v,-1.0F),n);
    if(!direction(she_add(n,r)))return std::nullopt;
    const Float3 hr=unit(she_add(n,r));
    const auto p=sh4(r),q=sh4(hr);
    Row row{};
    for(int i=0;i<25;++i){
        const int l=i<1?0:i<4?1:i<9?2:i<16?3:4;
        row[static_cast<std::size_t>(i)] =
            p[static_cast<std::size_t>(i)]*std::exp(-0.5*l*(l+1)*static_cast<double>(alpha));
    }
    for(int i=1;i<9;++i){
        const int l=i<4?1:2;
        row[static_cast<std::size_t>(24+i)] =
            q[static_cast<std::size_t>(i)]*std::exp(-0.5*l*(l+1)*static_cast<double>(alpha));
    }
    return row;
}

std::uint16_t float_to_half(float value) noexcept {
    const std::uint32_t bits=std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign=(bits>>16)&0x8000u;
    const std::uint32_t exponent=(bits>>23)&0xffu;
    std::uint32_t mantissa=bits&0x7fffffu;
    if(exponent==255u)return static_cast<std::uint16_t>(sign|(mantissa?0x7e00u:0x7c00u));
    int halfExp=static_cast<int>(exponent)-127+15;
    if(halfExp>=31)return static_cast<std::uint16_t>(sign|0x7c00u);
    if(halfExp<=0){
        if(halfExp < -10)return static_cast<std::uint16_t>(sign);
        mantissa|=0x800000u;
        const int shift=14-halfExp;
        std::uint32_t result=mantissa>>shift;
        const std::uint32_t remainder=mantissa&((1u<<shift)-1u);
        const std::uint32_t midpoint=1u<<(shift-1);
        if(remainder>midpoint||(remainder==midpoint&&(result&1u)))++result;
        return static_cast<std::uint16_t>(sign|result);
    }
    std::uint32_t result=mantissa>>13;
    const auto remainder=mantissa&0x1fffu;
    if(remainder>0x1000u||(remainder==0x1000u&&(result&1u)))++result;
    if(result==1024u){result=0; ++halfExp;}
    if(halfExp>=31)return static_cast<std::uint16_t>(sign|0x7c00u);
    return static_cast<std::uint16_t>(sign|(static_cast<std::uint32_t>(halfExp)<<10)|result);
}

float half_to_float(std::uint16_t h) noexcept {
    const std::uint32_t sign=(static_cast<std::uint32_t>(h)&0x8000u)<<16;
    std::uint32_t exponent=(h>>10)&31u;
    std::uint32_t mantissa=h&1023u;
    if(exponent==0u){
        if(mantissa==0u)return std::bit_cast<float>(sign);
        int e=-14;
        while((mantissa&1024u)==0u){mantissa<<=1;--e;}
        mantissa&=1023u;
        return std::bit_cast<float>(sign|(static_cast<std::uint32_t>(e+127)<<23)|(mantissa<<13));
    }
    if(exponent==31u)return std::bit_cast<float>(sign|0x7f800000u|(mantissa<<13));
    return std::bit_cast<float>(sign|((exponent+112u)<<23)|(mantissa<<13));
}

float radical_inverse(std::uint32_t i) noexcept {
    i=((i&0x55555555u)<<1)|((i>>1)&0x55555555u);
    i=((i&0x33333333u)<<2)|((i>>2)&0x33333333u);
    i=((i&0x0f0f0f0fu)<<4)|((i>>4)&0x0f0f0f0fu);
    i=((i&0x00ff00ffu)<<8)|((i>>8)&0x00ff00ffu);
    return static_cast<float>((i<<16)|(i>>16))*2.3283064365386963e-10F;
}
float smith_lambda(float nDot, float alpha) noexcept {
    const float nn=std::max(nDot,1.0e-5F);
    return 0.5F*(std::sqrt(1.0F+(alpha*alpha)*(1.0F-nn*nn)/(nn*nn))-1.0F);
}

Float3 monte_carlo_base(const EnvironmentCube& cube, Float3 n, Float3 v, float alpha,
                        std::uint32_t samples) {
    const Float3 axis=std::abs(n.y)<0.9F?Float3{0,1,0}:Float3{1,0,0};
    const Float3 t=unit(cross(axis,n)),b=cross(n,t);
    const float nDotV=std::max(she_dot(n,v),1.0e-4F);
    Float3 sum{};
    for(std::uint32_t j=0;j<samples;++j){
        const float xiX=(static_cast<float>(j)+0.5F)/static_cast<float>(samples);
        const float xiY=radical_inverse(j);
        const float a2=alpha*alpha;
        const float ct=std::min(1.0F,std::sqrt(std::max(0.0F,(1.0F-xiY)/
                                                     (1.0F+(a2-1.0F)*xiY))));
        const float st=std::sqrt(std::max(0.0F,1.0F-ct*ct));
        const float phi=2.0F*static_cast<float>(kPi)*xiX;
        const Float3 h=unit(she_add(she_add(scale(t,std::cos(phi)*st),scale(b,std::sin(phi)*st)),
                                scale(n,ct)));
        const float vDotH=she_dot(v,h);
        if(vDotH<=0.0F)continue;
        const Float3 l=sub(scale(h,2.0F*vDotH),v);
        const float nDotL=she_dot(n,l);
        if(nDotL<=0.0F)continue;
        const float visibility=1.0F/(1.0F+smith_lambda(nDotV,alpha)+
                                      smith_lambda(nDotL,alpha));
        const float weight=visibility*vDotH/(nDotV*std::max(ct,1.0e-5F));
        sum=she_add(sum,scale(sample_environment_cube(cube,l),weight));
    }
    return scale(sum,1.0F/static_cast<float>(samples));
}
} // namespace

bool SheProbeCoefficients::validate(std::string* error) const noexcept {
    for(const auto& channel:rgb)for(float v:channel)
        if(!std::isfinite(v)||std::abs(v)>64.0F){
            if(error)*error="SHE coefficients must be finite and in [-64,64]";
            return false;
        }
    return true;
}

bool SheBakeSettings::validate() const noexcept {
    return trainingSamples>=2*kSheCoefficientCount &&
           trainingSamples<=1000000u &&
           ggxSamplesPerTrainingPoint>=8u && ggxSamplesPerTrainingPoint<=16384u &&
           std::isfinite(regularization)&&regularization>=0.0F&&regularization<=1.0F;
}

std::optional<Float3> reference_ggx_environment_integral(
    const EnvironmentCube& source, Float3 normal, Float3 viewDirection,
    float alpha, std::uint32_t samples) {
    if(!source.validate() || !direction(normal) || !direction(viewDirection) ||
       !std::isfinite(alpha) || alpha<=0.0F || alpha>1.0F ||
       samples<16U || samples>1000000U) return std::nullopt;
    const Float3 n=unit(normal),v=unit(viewDirection);
    if(she_dot(n,v)<1.0e-4F)return std::nullopt;
    return monte_carlo_base(source,n,v,alpha,samples);
}

std::optional<Float3> evaluate_she_base_reflectance(
    const SheProbeCoefficients& probe, Float3 normal, Float3 viewDirection,
    float alpha) noexcept {
    if(!probe.validate())return std::nullopt;
    const auto row=make_row(normal,viewDirection,alpha);
    if(!row)return std::nullopt;
    Float3 value{};
    float* channel[3]={&value.x,&value.y,&value.z};
    for(std::size_t c=0;c<3;++c){
        double logValue=0.0;
        for(std::size_t i=0;i<kSheCoefficientCount;++i)
            logValue+=(*row)[i]*probe.rgb[c][i];
        *channel[c]=static_cast<float>(std::exp(std::clamp(logValue,-20.0,16.0)));
    }
    return value;
}

std::optional<SheFitResult> fit_she_probe(std::span<const SheFitSample> samples,
                                          float regularization) {
    if(samples.size()<2*kSheCoefficientCount||!std::isfinite(regularization)||
       regularization<0.0F||regularization>1.0F)return std::nullopt;
    Matrix gram{};
    Channels rhs{};
    std::uint32_t count=0;
    for(const auto& sample:samples){
        const auto row=make_row(sample.normal,sample.viewDirection,sample.alpha);
        if(!row||!finite(sample.baseReflectance))continue;
        const double targets[3]={std::log(std::max(1.0e-6F,sample.baseReflectance.x)),
                                 std::log(std::max(1.0e-6F,sample.baseReflectance.y)),
                                 std::log(std::max(1.0e-6F,sample.baseReflectance.z))};
        for(std::size_t i=0;i<kSheCoefficientCount;++i){
            for(std::size_t j=0;j<=i;++j)gram[i][j]+=(*row)[i]*(*row)[j];
            for(std::size_t c=0;c<3;++c)rhs[i][c]+=(*row)[i]*targets[c];
        }
        ++count;
    }
    if(count<2*kSheCoefficientCount)return std::nullopt;
    double trace=0.0;
    for(std::size_t i=0;i<kSheCoefficientCount;++i){
        trace+=gram[i][i];
        for(std::size_t j=i+1;j<kSheCoefficientCount;++j)gram[i][j]=gram[j][i];
    }
    // Scale regularization with the average Gram diagonal so sample count
    // changes do not unexpectedly change the fit.
    const double ridge=std::max(1.0e-12,static_cast<double>(regularization)*
                                    trace/static_cast<double>(kSheCoefficientCount));
    for(std::size_t i=0;i<kSheCoefficientCount;++i)gram[i][i]+=ridge;
    // Solve the three channels simultaneously using pivoted Gaussian elimination.
    for(std::size_t pivot=0;pivot<kSheCoefficientCount;++pivot){
        std::size_t best=pivot;
        for(std::size_t i=pivot+1;i<kSheCoefficientCount;++i)
            if(std::abs(gram[i][pivot])>std::abs(gram[best][pivot]))best=i;
        if(std::abs(gram[best][pivot])<1.0e-14)return std::nullopt;
        if(best!=pivot){std::swap(gram[best],gram[pivot]);std::swap(rhs[best],rhs[pivot]);}
        const double inv=1.0/gram[pivot][pivot];
        for(std::size_t j=pivot;j<kSheCoefficientCount;++j)gram[pivot][j]*=inv;
        for(std::size_t c=0;c<3;++c)rhs[pivot][c]*=inv;
        for(std::size_t i=pivot+1;i<kSheCoefficientCount;++i){
            const double factor=gram[i][pivot];
            for(std::size_t j=pivot;j<kSheCoefficientCount;++j)
                gram[i][j]-=factor*gram[pivot][j];
            for(std::size_t c=0;c<3;++c)rhs[i][c]-=factor*rhs[pivot][c];
        }
    }
    SheFitResult result;
    result.fittedSamples=count;
    for(std::size_t c=0;c<3;++c){
        for(int i=static_cast<int>(kSheCoefficientCount)-1;i>=0;--i){
            double v=rhs[static_cast<std::size_t>(i)][c];
            for(std::size_t j=static_cast<std::size_t>(i)+1;j<kSheCoefficientCount;++j)
                v-=gram[static_cast<std::size_t>(i)][j]*result.coefficients.rgb[c][j];
            result.coefficients.rgb[c][static_cast<std::size_t>(i)]=static_cast<float>(v);
        }
    }
    if(!result.coefficients.validate())return std::nullopt;
    double error2=0.0;
    for(const auto& s:samples){
        if(!finite(s.baseReflectance))continue;
        const auto row=make_row(s.normal,s.viewDirection,s.alpha);
        if(!row)continue;
        const float target[3]={s.baseReflectance.x,s.baseReflectance.y,s.baseReflectance.z};
        for(std::size_t c=0;c<3;++c){
            double predicted=0.0;
            for(std::size_t i=0;i<kSheCoefficientCount;++i)
                predicted+=(*row)[i]*result.coefficients.rgb[c][i];
            const double delta=predicted-std::log(std::max(target[c],1.0e-6F));
            error2+=delta*delta;
        }
    }
    result.logRmse=static_cast<float>(std::sqrt(error2/(3.0*count)));
    return result;
}

std::optional<SheFitResult> bake_she_probe(
    const EnvironmentCube& source, const SheBakeSettings& settings) {
    if(!settings.validate()||!source.validate())return std::nullopt;
    std::vector<SheFitSample> training;
    training.reserve(settings.trainingSamples);
    constexpr float goldenAngle=2.3999632297286533F;
    for(std::uint32_t i=0;i<settings.trainingSamples;++i){
        const float u=(static_cast<float>(i)+0.5F)/settings.trainingSamples;
        const float y=1.0F-2.0F*u;
        const float radius=std::sqrt(std::max(0.0F,1.0F-y*y));
        const float angle=goldenAngle*static_cast<float>(i);
        const Float3 n={radius*std::cos(angle),y,radius*std::sin(angle)};
        const Float3 axis=std::abs(n.y)<0.9F?Float3{0,1,0}:Float3{1,0,0};
        const Float3 t=unit(cross(axis,n)),b=cross(n,t);
        const float ndotv=0.05F+0.95F*fract(static_cast<float>(i)*0.75487766625F);
        const float phi=2.0F*static_cast<float>(kPi)*fract(static_cast<float>(i)*0.56984029099F);
        const float sint=std::sqrt(std::max(0.0F,1.0F-ndotv*ndotv));
        const Float3 v=unit(she_add(scale(n,ndotv),
                            she_add(scale(t,sint*std::cos(phi)),scale(b,sint*std::sin(phi)))));
        const float alpha=kSheMinimumAlpha+(1.0F-kSheMinimumAlpha)*
                          fract(static_cast<float>(i)*0.61803398875F);
        const Float3 target=monte_carlo_base(source,n,v,alpha,
                                             settings.ggxSamplesPerTrainingPoint);
        training.push_back({n,v,alpha,target});
    }
    return fit_she_probe(training,settings.regularization);
}

ShePackedProbe pack_she_probe_half(const SheProbeCoefficients& probe) noexcept {
    ShePackedProbe packed;
    if(!probe.validate())return packed;
    for(std::size_t i=0;i<kSheCoefficientCount;++i)
        for(std::size_t c=0;c<3;++c)
            packed.half[3*i+c]=float_to_half(probe.rgb[c][i]);
    return packed;
}
SheProbeCoefficients unpack_she_probe_half(const ShePackedProbe& packed) noexcept {
    SheProbeCoefficients probe;
    for(std::size_t i=0;i<kSheCoefficientCount;++i)
        for(std::size_t c=0;c<3;++c)
            probe.rgb[c][i]=half_to_float(packed.half[3*i+c]);
    return probe;
}
} // namespace dve
