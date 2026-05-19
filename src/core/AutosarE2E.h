#pragma once

#include "core/BusMessage.h"

#include <array>
#include <cstdint>

// CRC-8H2F lookup table — polynomial 0x2F, non-reflected.
// Used by AUTOSAR E2E Profile 2 (and Profile 4).
namespace detail
{

consteval std::array<uint8_t, 256> makeCrc8H2FTable() noexcept
{
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i)
    {
        uint8_t crc = static_cast<uint8_t>(i);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80u) ? static_cast<uint8_t>((crc << 1) ^ 0x2Fu)
                                : static_cast<uint8_t>(crc << 1);
        t[i] = crc;
    }
    return t;
}

inline constexpr auto kCrc8H2FTable = makeCrc8H2FTable();

} // namespace detail

inline uint8_t crc8h2f_byte(uint8_t crc, uint8_t byte) noexcept
{
    return detail::kCrc8H2FTable[crc ^ byte];
}

// Compute AUTOSAR E2E Profile 2 CRC over a BusMessage.
//
// CRC input order (per AUTOSAR_SWS_E2ELibrary):
//   DataID low byte, DataID high byte, data[0]=0x00 (CRC byte zeroed),
//   data[1..length-1]
//
// The message must have DLC >= 2 (byte 0 = CRC, byte 1 = counter nibble).
// The caller is responsible for writing the returned value into byte 0.
inline uint8_t e2e_p2_compute_crc(const BusMessage &msg, uint16_t dataID) noexcept
{
    uint8_t crc = 0xFFu;
    crc = crc8h2f_byte(crc, static_cast<uint8_t>(dataID & 0xFFu));
    crc = crc8h2f_byte(crc, static_cast<uint8_t>(dataID >> 8u));
    crc = crc8h2f_byte(crc, 0x00u); // CRC byte position treated as zero
    const uint8_t len = msg.getLength();
    for (uint8_t i = 1u; i < len; ++i)
        crc = crc8h2f_byte(crc, msg.getByte(i));
    return crc ^ 0xFFu;
}
