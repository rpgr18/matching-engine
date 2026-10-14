#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/command.hpp"
#include "lob/order_book.hpp"

namespace lob {

/// A recorded request stream plus the book configuration it must be replayed against.
///
/// On disk: a 48-byte header followed by `count` raw 32-byte Command records, in the host's byte
/// order (every platform this builds on is little-endian). Reading is a single bulk read straight
/// into the command array, so a replay starts as fast as the file can be read.
struct CommandFile {
  BookConfig config;
  std::size_t prefill = 0;  ///< Leading commands that build the starting book.
  std::vector<Command> commands;
};

namespace detail {

inline constexpr char kCommandMagic[8] = {'L', 'O', 'B', 'C', 'M', 'D', '0', '1'};

struct CommandFileHeader {
  char magic[8];
  std::uint64_t count;
  std::uint64_t prefill;
  std::int64_t min_price;
  std::int64_t max_price;
  std::uint32_t max_orders;
  std::uint8_t stp;
  std::uint8_t reserved[3];
};
static_assert(sizeof(CommandFileHeader) == 48);

struct FileCloser {
  void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

inline File open_file(const std::string& path, const char* mode) {
  File f(std::fopen(path.c_str(), mode));
  if (!f) throw std::runtime_error("cannot open " + path);
  return f;
}

}  // namespace detail

inline void write_command_file(const std::string& path, const CommandFile& file) {
  detail::CommandFileHeader h{};
  std::memcpy(h.magic, detail::kCommandMagic, sizeof(h.magic));
  h.count = file.commands.size();
  h.prefill = file.prefill;
  h.min_price = file.config.min_price;
  h.max_price = file.config.max_price;
  h.max_orders = file.config.max_orders;
  h.stp = static_cast<std::uint8_t>(file.config.stp);

  const auto f = detail::open_file(path, "wb");
  const bool ok = std::fwrite(&h, sizeof(h), 1, f.get()) == 1 &&
                  std::fwrite(file.commands.data(), sizeof(Command), file.commands.size(),
                              f.get()) == file.commands.size();
  if (!ok) throw std::runtime_error("short write to " + path);
}

inline CommandFile read_command_file(const std::string& path) {
  const auto f = detail::open_file(path, "rb");
  detail::CommandFileHeader h{};
  if (std::fread(&h, sizeof(h), 1, f.get()) != 1 ||
      std::memcmp(h.magic, detail::kCommandMagic, sizeof(h.magic)) != 0)
    throw std::runtime_error(path + " is not a command file");
  if (h.prefill > h.count || h.stp > static_cast<std::uint8_t>(StpMode::CancelBoth))
    throw std::runtime_error(path + " has a corrupt header");

  CommandFile out;
  out.config.min_price = h.min_price;
  out.config.max_price = h.max_price;
  out.config.max_orders = h.max_orders;
  out.config.stp = static_cast<StpMode>(h.stp);
  out.prefill = static_cast<std::size_t>(h.prefill);
  out.commands.resize(static_cast<std::size_t>(h.count));
  if (std::fread(out.commands.data(), sizeof(Command), out.commands.size(), f.get()) !=
      out.commands.size())
    throw std::runtime_error(path + " is truncated");
  return out;
}

}  // namespace lob
