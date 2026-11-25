#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace landrop {

// Small, dependency-free SHA-256 implementation used for end-to-end file
// integrity checks.  The class is streaming so multi-gigabyte files never need
// to be held in memory.
class Sha256 {
public:
    using Digest = std::array<uint8_t, 32>;

    Sha256();
    void reset();
    void update(const void* data, std::size_t size);
    Digest finish();

    static Digest hash(const void* data, std::size_t size);
    static std::string toHex(const Digest& digest);
    static bool fromHex(const std::string& hex, Digest& out);

private:
    void transform(const uint8_t* block);

    uint32_t state_[8];
    uint64_t bitLength_;
    uint8_t buffer_[64];
    std::size_t bufferSize_;
};

}  // namespace landrop
