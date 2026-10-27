#pragma once
// PSO-1: opt-in persisted Vulkan pipeline caches. No Vulkan SDK dependency.
// Driver cache bytes are never trusted as input until both our envelope AND
// Vulkan's own 32-byte VkPipelineCacheHeaderVersionOne have been checked.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#include <chrono>

namespace dve::rhi::pso {
inline constexpr std::size_t kMaximumDriverCacheBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kEnvelopeBytes = 64U;
struct Identity {
    std::uint32_t vendor{}, device{}, driver{}, api{};
    std::array<std::uint8_t,16> uuid{};
    bool operator==(const Identity&) const = default;
};
inline constexpr std::array<std::uint8_t,8> kMagic{'D','V','E','P','S','O','1',0};
inline std::uint32_t read32(std::span<const std::byte> data, std::size_t at) noexcept {
    std::uint32_t v=0;
    for(std::size_t i=0;i<4U;++i)
        v|=static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[at+i])) << (i*8U);
    return v;
}
inline void write32(std::span<std::byte> data, std::size_t at, std::uint32_t v) noexcept {
    for(std::size_t i=0;i<4U;++i)
        data[at+i]=static_cast<std::byte>((v>>(i*8U))&255U);
}
inline std::uint64_t checksum(std::span<const std::byte> data) noexcept {
    std::uint64_t hash=14695981039346656037ULL;
    for(const auto byte:data) {
        hash^=std::to_integer<std::uint8_t>(byte);
        hash*=1099511628211ULL;
    }
    return hash;
}
inline void write64(std::span<std::byte> data, std::size_t at, std::uint64_t value) noexcept {
    for(std::size_t i=0;i<8U;++i)
        data[at+i]=static_cast<std::byte>((value>>(i*8U))&255U);
}
inline std::uint64_t read64(std::span<const std::byte> data, std::size_t at) noexcept {
    std::uint64_t value=0;
    for(std::size_t i=0;i<8U;++i)
        value|=static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[at+i])) << (i*8U);
    return value;
}
inline bool valid_driver_blob(std::span<const std::byte> data,
                              const Identity& gpu) noexcept {
    // Vulkan 1.0: headerSize, headerVersion, vendorID, deviceID, UUID[16].
    if(data.size()<32U || data.size()>kMaximumDriverCacheBytes) return false;
    if(read32(data,0U)<32U || read32(data,0U)>data.size() ||
       read32(data,4U)!=1U || read32(data,8U)!=gpu.vendor ||
       read32(data,12U)!=gpu.device) return false;
    for(std::size_t i=0;i<gpu.uuid.size();++i)
        if(std::to_integer<std::uint8_t>(data[16U+i])!=gpu.uuid[i])return false;
    return true;
}
inline bool encode(const Identity& gpu, std::span<const std::byte> driver,
                   std::vector<std::byte>& encoded) {
    encoded.clear();
    if(!valid_driver_blob(driver,gpu))return false;
    std::vector<std::byte> result(kEnvelopeBytes+driver.size());
    for(std::size_t i=0;i<kMagic.size();++i) result[i]=static_cast<std::byte>(kMagic[i]);
    write32(result,8U,1U); // envelope version
    write32(result,12U,gpu.vendor);write32(result,16U,gpu.device);
    write32(result,20U,gpu.driver);write32(result,24U,gpu.api);
    for(std::size_t i=0;i<gpu.uuid.size();++i)
        result[28U+i]=static_cast<std::byte>(gpu.uuid[i]);
    write32(result,44U,static_cast<std::uint32_t>(driver.size()));
    write64(result,48U,checksum(driver));
    std::copy(driver.begin(),driver.end(),result.begin()+static_cast<std::ptrdiff_t>(kEnvelopeBytes));
    encoded=std::move(result);
    return true;
}
inline bool decode(std::span<const std::byte> encoded, const Identity& gpu,
                   std::vector<std::byte>& driver) {
    driver.clear();
    if(encoded.size()<kEnvelopeBytes+32U ||
       encoded.size()>kEnvelopeBytes+kMaximumDriverCacheBytes) return false;
    for(std::size_t i=0;i<kMagic.size();++i)
        if(std::to_integer<std::uint8_t>(encoded[i])!=kMagic[i])return false;
    if(read32(encoded,8U)!=1U || read32(encoded,12U)!=gpu.vendor ||
       read32(encoded,16U)!=gpu.device || read32(encoded,20U)!=gpu.driver ||
       read32(encoded,24U)!=gpu.api)return false;
    for(std::size_t i=0;i<gpu.uuid.size();++i)
        if(std::to_integer<std::uint8_t>(encoded[28U+i])!=gpu.uuid[i])return false;
    for(std::size_t i=56U;i<kEnvelopeBytes;++i)
        if(encoded[i]!=std::byte{})return false; // future extension
    if(read32(encoded,44U)!=encoded.size()-kEnvelopeBytes)return false;
    const auto blob=encoded.subspan(kEnvelopeBytes);
    if(read64(encoded,48U)!=checksum(blob) || !valid_driver_blob(blob,gpu))
        return false;
    driver.assign(blob.begin(),blob.end());
    return true;
}
// Silent miss is intentional: a corrupt/untrusted cache must never block graphics startup.
inline bool load(const std::filesystem::path& path, const Identity& gpu,
                 std::vector<std::byte>& driver) {
    driver.clear();
    if(path.empty())return false;
    std::error_code ec;
    const auto bytes=std::filesystem::file_size(path,ec);
    if(ec || bytes<kEnvelopeBytes+32U ||
       bytes>kEnvelopeBytes+kMaximumDriverCacheBytes)return false;
    std::ifstream input(path,std::ios::binary);
    if(!input)return false;
    std::vector<std::byte> file(static_cast<std::size_t>(bytes));
    input.read(reinterpret_cast<char*>(file.data()),static_cast<std::streamsize>(file.size()));
    if(input.gcount()!=static_cast<std::streamsize>(file.size()))return false;
    return decode(file,gpu,driver);
}
// Save by writing a sibling temporary file then replacing by rename. If the host
// OS doesn't permit atomic overwrite (notably some Windows configurations), leave
// the previous known-good cache intact; we do not remove it first.
inline bool save(const std::filesystem::path& path, const Identity& gpu,
                 std::span<const std::byte> blob) {
    if(path.empty())return false;
    std::vector<std::byte> file;
    if(!encode(gpu,blob,file))return false;
    std::error_code ec;
    if(!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(),ec);
        if(ec)return false;
    }
    const auto suffix=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto temp=path;
    temp+=".tmp."+suffix;
    {
        std::ofstream output(temp,std::ios::binary|std::ios::trunc);
        if(!output)return false;
        output.write(reinterpret_cast<const char*>(file.data()),
                     static_cast<std::streamsize>(file.size()));
        output.flush();
        if(!output){output.close();std::filesystem::remove(temp,ec);return false;}
    }
    std::filesystem::rename(temp,path,ec);
    if(ec){std::filesystem::remove(temp,ec);return false;}
    return true;
}
} // namespace dve::rhi::pso
