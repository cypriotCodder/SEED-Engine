#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace seed {
class Bytes {
public:
    void u8(std::uint8_t n) { data.push_back(n); }
    void u16(std::uint16_t n) {
        for (unsigned i = 0; i < 2; ++i)
            u8(static_cast<std::uint8_t>(n >> (8 * i)));
    }
    void u32(std::uint32_t n) {
        for (unsigned i = 0; i < 4; ++i)
            u8(static_cast<std::uint8_t>(n >> (8 * i)));
    }
    void u64(std::uint64_t n) {
        for (unsigned i = 0; i < 8; ++i)
            u8(static_cast<std::uint8_t>(n >> (8 * i)));
    }
    std::vector<std::uint8_t> data;
};
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint8_t u8() {
        if (offset_ == bytes_.size()) throw std::runtime_error("Truncated binary record");
        return bytes_[offset_++];
    }
    std::uint16_t u16() {
        std::uint16_t n = 0;
        for (unsigned i = 0; i < 2; ++i)
            n |= std::uint16_t(u8()) << (8 * i);
        return n;
    }
    std::uint32_t u32() {
        std::uint32_t n = 0;
        for (unsigned i = 0; i < 4; ++i)
            n |= std::uint32_t(u8()) << (8 * i);
        return n;
    }
    std::uint64_t u64() {
        std::uint64_t n = 0;
        for (unsigned i = 0; i < 8; ++i)
            n |= std::uint64_t(u8()) << (8 * i);
        return n;
    }
    bool done() const { return offset_ == bytes_.size(); }
    std::size_t remaining() const { return bytes_.size() - offset_; }
    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > bytes_.size() - offset_) throw std::runtime_error("Truncated binary payload");
        const auto result = bytes_.subspan(offset_, count);
        offset_ += count;
        return result;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};
inline std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = ~0U;
    for (auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}
} // namespace seed
