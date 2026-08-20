#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace rtlangle::test {

// A temporary directory that removes itself, so a failing assertion cannot
// leave files behind for the next run to trip over.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("rtlangle-test-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path child(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

inline std::string read_whole(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void write_whole(const std::filesystem::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

}  // namespace rtlangle::test
