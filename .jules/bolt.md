## 2026-08-30 - Slicing-by-8 CRC32 Optimization
**Learning:** CRC32 calculation in `Crc32::Compute` is on the critical path for journal appends, replays, session markers, and backing store operations. The byte-at-a-time loop can be accelerated ~4x-8x using slicing-by-8 with 8 constexpr lookup tables without changing external API or memory layout.
**Action:** Use `std::memcpy` for unaligned 32-bit loads to safely slice 8 bytes per iteration in CRC32 loops.
