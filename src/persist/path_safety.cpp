#include "persist/path_safety.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rtlangle {
namespace {

// Restarted on EINTR, and on EINTR only. A signal must never become a failure
// the caller has to classify.
int retry_open(const char* path, int flags) {
  for (;;) {
    const int fd = ::open(path, flags);
    if (fd >= 0 || errno != EINTR) return fd;
  }
}

ssize_t retry_read(int fd, void* buf, std::size_t n) {
  for (;;) {
    const ssize_t r = ::read(fd, buf, n);
    if (r >= 0 || errno != EINTR) return r;
  }
}

}  // namespace

int open_read_no_symlink(std::string_view path, std::string& error) {
  const std::string p(path);
  const int fd = retry_open(p.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ELOOP) {
      error = "refusing to open \"" + p +
              "\": the final path component is a symbolic link.";
    } else {
      error = "cannot open \"" + p + "\": " + std::strerror(errno);
    }
    return -1;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    error = "cannot stat \"" + p + "\": " + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    error = "refusing to read \"" + p + "\": it is not a regular file.";
    ::close(fd);
    return -1;
  }
  return fd;
}

std::optional<std::string> read_file_limited(std::string_view path, std::size_t size_limit,
                                             std::string& error) {
  error.clear();
  const int fd = open_read_no_symlink(path, error);
  if (fd < 0) return std::nullopt;

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    error = "cannot stat \"" + std::string(path) + "\": " + std::strerror(errno);
    ::close(fd);
    return std::nullopt;
  }
  if (static_cast<std::size_t>(st.st_size) > size_limit) {
    error = "refusing to read \"" + std::string(path) + "\": it is " +
            std::to_string(st.st_size) + " bytes, above the limit of " +
            std::to_string(size_limit) + " bytes.";
    ::close(fd);
    return std::nullopt;
  }

  std::string out;
  out.resize(static_cast<std::size_t>(st.st_size));
  std::size_t got = 0;
  while (got < out.size()) {
    const ssize_t r = retry_read(fd, out.data() + got, out.size() - got);
    if (r < 0) {
      error = "cannot read \"" + std::string(path) + "\": " + std::strerror(errno);
      ::close(fd);
      return std::nullopt;
    }
    if (r == 0) break;
    got += static_cast<std::size_t>(r);
  }
  out.resize(got);
  ::close(fd);
  return out;
}

std::string basename_of(std::string_view path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string_view::npos) return std::string(path);
  return std::string(path.substr(slash + 1));
}

}  // namespace rtlangle

namespace rtlangle {
namespace {

bool allowed(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '.' || c == '_' || c == '-';
}

void strip_ends(std::string& s) {
  std::size_t first = 0;
  while (first < s.size() && (s[first] == '.' || s[first] == '-')) ++first;
  std::size_t last = s.size();
  while (last > first && (s[last - 1] == '.' || s[last - 1] == '-')) --last;
  s = s.substr(first, last - first);
}

}  // namespace

std::string slugify_label(std::string_view label) {
  // 1: every character outside the allowed set becomes '-', which removes '/'
  //    and every other separator.
  std::string s;
  s.reserve(label.size());
  for (char c : label) s.push_back(allowed(c) ? c : '-');

  // 2: collapse runs of '-' AND runs of '.'. The dot-run collapse is what makes
  //    ".." unrepresentable anywhere in the output.
  std::string collapsed;
  collapsed.reserve(s.size());
  for (char c : s) {
    if ((c == '-' || c == '.') && !collapsed.empty() && collapsed.back() == c) continue;
    collapsed.push_back(c);
  }

  // 3, 4: strip, truncate, strip again because the truncation can expose an end
  //       character.
  strip_ends(collapsed);
  if (collapsed.size() > 32) collapsed.resize(32);
  strip_ends(collapsed);

  // 5.
  if (collapsed.empty() || collapsed == "." || collapsed == "..") return {};
  return collapsed;
}

bool valid_directory_component(std::string_view name) {
  if (name.empty()) return false;
  if (name == "." || name == "..") return false;
  return name.find('/') == std::string_view::npos;
}

}  // namespace rtlangle
