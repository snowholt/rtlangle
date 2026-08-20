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
