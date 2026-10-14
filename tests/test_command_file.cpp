#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "lob/command_file.hpp"
#include "lob/workload.hpp"

namespace {

using namespace lob;

std::string temp_path(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

TEST(CommandFile, RoundTripsCommandsAndConfig) {
  WorkloadSpec spec;
  spec.requests = 5'000;
  spec.prefill = 300;
  const Workload w = generate(spec);

  CommandFile out;
  out.config = book_config_for(spec);
  out.config.stp = StpMode::CancelBoth;
  out.prefill = w.prefill;
  out.commands = w.commands;
  const std::string path = temp_path("lob_roundtrip.bin");
  write_command_file(path, out);

  const CommandFile in = read_command_file(path);
  EXPECT_EQ(in.prefill, out.prefill);
  EXPECT_EQ(in.config.min_price, out.config.min_price);
  EXPECT_EQ(in.config.max_price, out.config.max_price);
  EXPECT_EQ(in.config.max_orders, out.config.max_orders);
  EXPECT_EQ(in.config.stp, StpMode::CancelBoth);
  EXPECT_EQ(in.commands, out.commands);
  EXPECT_EQ(std::filesystem::file_size(path), 48 + 32 * out.commands.size());
  std::filesystem::remove(path);
}

TEST(CommandFile, RejectsForeignAndTruncatedFiles) {
  const std::string junk = temp_path("lob_junk.bin");
  {
    std::ofstream f(junk, std::ios::binary);
    f << "definitely not a command file, but long enough to hold a header";
  }
  EXPECT_THROW(read_command_file(junk), std::runtime_error);

  CommandFile out;
  out.commands.resize(10);
  const std::string cut = temp_path("lob_cut.bin");
  write_command_file(cut, out);
  std::filesystem::resize_file(cut, 48 + 32 * 9 + 5);
  EXPECT_THROW(read_command_file(cut), std::runtime_error);

  EXPECT_THROW(read_command_file(temp_path("lob_does_not_exist.bin")), std::runtime_error);
  std::filesystem::remove(junk);
  std::filesystem::remove(cut);
}

}  // namespace
