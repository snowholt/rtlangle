#include "persist/atomic_write.h"

#include "persist/path_safety.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace rtlangle {
namespace {

std::string temp_name(std::string_view stem) {
  return std::string(stem) + ".tmp." + std::to_string(::getpid());
}

// EINTR is retried inside these wrappers, and ONLY EINTR. Every other errno
// classifies to a CommitOutcome. In particular EIO from fsync is NOT retried: a
// failed fsync may already have discarded the error state that a second call
// would then report as success.
int retry_openat(const FileOps& ops, int dirfd, const char* name, int flags, mode_t mode) {
  for (;;) {
    const int fd = ops.openat_fn(dirfd, name, flags, mode);
    if (fd >= 0 || errno != EINTR) return fd;
  }
}

ssize_t retry_write(const FileOps& ops, int fd, const void* buf, std::size_t count) {
  for (;;) {
    const ssize_t n = ops.write_fn(fd, buf, count);
    if (n >= 0 || errno != EINTR) return n;
  }
}

int retry_fsync(const FileOps& ops, int fd) {
  for (;;) {
    const int rc = ops.fsync_fn(fd);
    if (rc == 0) return 0;
    if (errno != EINTR) return rc;
  }
}

}  // namespace

const FileOps& default_file_ops() {
  static const FileOps ops = [] {
    FileOps o;
    o.openat_fn = [](int dirfd, const char* name, int flags, mode_t mode) {
      return ::openat(dirfd, name, flags, mode);
    };
    o.write_fn = [](int fd, const void* buf, std::size_t count) {
      return ::write(fd, buf, count);
    };
    o.fsync_fn = [](int fd) { return ::fsync(fd); };
    o.renameat_fn = [](int olddirfd, const char* oldname, int newdirfd, const char* newname) {
      return ::renameat(olddirfd, oldname, newdirfd, newname);
    };
    o.close_fn = [](int fd) { return ::close(fd); };
    o.unlinkat_fn = [](int dirfd, const char* name, int flags) {
      return ::unlinkat(dirfd, name, flags);
    };
    return o;
  }();
  return ops;
}

bool renameat_errno_is_classifiable(int err) {
  // Every errno below means the rename did not take place. EIO is deliberately
  // absent: it can be returned after the directory entry has been written, so
  // it does not distinguish "did not happen" from "happened".
  switch (err) {
    case EACCES:
    case EBADF:
    case EBUSY:
    case EDQUOT:
    case EFAULT:
    case EINVAL:
    case EISDIR:
    case ELOOP:
    case EMLINK:
    case ENAMETOOLONG:
    case ENOENT:
    case ENOMEM:
    case ENOSPC:
    case ENOTDIR:
    case ENOTEMPTY:
    case EEXIST:
    case EPERM:
    case EROFS:
    case EXDEV:
      return true;
    default:
      return false;
  }
}

CommitResult write_atomic_at(int dirfd, std::string_view name, std::string_view bytes,
                             const FileOps& ops) {
  CommitResult result;
  const std::string target(name);
  const std::string tmp = temp_name(name);

  // Step 1: the temporary file. O_EXCL, so a predictable name cannot be
  // pre-created by anything else, and O_NOFOLLOW so it cannot be a symlink.
  const int fd = retry_openat(ops, dirfd, tmp.c_str(),
                              O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    result.outcome = CommitOutcome::NotCommitted;
    result.detail = "cannot create the temporary file \"" + tmp + "\": " + std::strerror(errno);
    return result;
  }

  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t n = retry_write(ops, fd, bytes.data() + written, bytes.size() - written);
    if (n < 0) {
      const std::string why = std::strerror(errno);
      ops.close_fn(fd);
      ops.unlinkat_fn(dirfd, tmp.c_str(), 0);
      result.outcome = CommitOutcome::NotCommitted;
      result.detail = "cannot write the temporary file \"" + tmp + "\": " + why;
      return result;
    }
    written += static_cast<std::size_t>(n);
  }

  // Step 2: make the file's contents durable before the rename publishes it.
  if (retry_fsync(ops, fd) != 0) {
    const std::string why = std::strerror(errno);
    ops.close_fn(fd);
    ops.unlinkat_fn(dirfd, tmp.c_str(), 0);
    result.outcome = CommitOutcome::NotCommitted;
    result.detail = "cannot fsync the temporary file \"" + tmp + "\": " + why;
    return result;
  }
  if (ops.close_fn(fd) != 0) {
    const std::string why = std::strerror(errno);
    ops.unlinkat_fn(dirfd, tmp.c_str(), 0);
    result.outcome = CommitOutcome::NotCommitted;
    result.detail = "cannot close the temporary file \"" + tmp + "\": " + why;
    return result;
  }

  // Step 3: THE COMMIT POINT. renameat is not interruptible on Linux, so it is
  // never reached in a retry loop and no retry can create commit ambiguity.
  if (ops.renameat_fn(dirfd, tmp.c_str(), dirfd, target.c_str()) != 0) {
    const int err = errno;
    const std::string why = std::strerror(err);
    if (renameat_errno_is_classifiable(err)) {
      ops.unlinkat_fn(dirfd, tmp.c_str(), 0);
      result.outcome = CommitOutcome::NotCommitted;
      result.detail = "the commit rename of \"" + target + "\" failed: " + why;
      return result;
    }
    // The platform does not let us tell whether the rename took place. The
    // caller must reconcile before doing anything else, and the temporary file
    // is deliberately NOT removed: it may be the committed record.
    result.outcome = CommitOutcome::Indeterminate;
    result.detail = "the commit rename of \"" + target +
                    "\" returned an error that does not distinguish whether it took place: " +
                    why + ". Reconcile against the record on disk before continuing.";
    return result;
  }

  // Step 4: make the rename itself durable. Failing here leaves a commit that
  // IS in the record and that every later read sees; it may not survive a power
  // cut. Treating that as a failure and retrying would duplicate the attempt.
  if (retry_fsync(ops, dirfd) != 0) {
    result.outcome = CommitOutcome::CommittedNotDurable;
    result.detail = "\"" + target +
                    "\" was committed, but the directory fsync failed, so the commit may not "
                    "survive a power loss: " +
                    std::strerror(errno);
    return result;
  }

  result.outcome = CommitOutcome::Committed;
  return result;
}

// ---------------------------------------------------------------------------
// SessionDir
// ---------------------------------------------------------------------------

SessionDir::~SessionDir() {
  if (fd_ >= 0) ::close(fd_);
}

SessionDir::SessionDir(SessionDir&& other) noexcept
    : fd_(other.fd_), path_(std::move(other.path_)) {
  other.fd_ = -1;
}

SessionDir& SessionDir::operator=(SessionDir&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    path_ = std::move(other.path_);
    other.fd_ = -1;
  }
  return *this;
}

std::optional<SessionDir> SessionDir::create(const std::filesystem::path& path,
                                             std::string& error) {
  error.clear();
  if (::mkdir(path.c_str(), 0700) != 0) {
    if (errno == EEXIST) {
      error = "refusing to use \"" + path.string() +
              "\": something already exists at that path. Session directories are always "
              "created, never reused, so a pre-existing entry - including a symbolic link - "
              "is never written through.";
    } else {
      error = "cannot create \"" + path.string() + "\": " + std::strerror(errno);
    }
    return std::nullopt;
  }
  return open_existing(path, error);
}

std::optional<SessionDir> SessionDir::open_existing(const std::filesystem::path& path,
                                                    std::string& error) {
  error.clear();
  const int fd = ::open(path.c_str(), O_DIRECTORY | O_NOFOLLOW | O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ELOOP) {
      error = "refusing to open \"" + path.string() + "\": it is a symbolic link.";
    } else {
      error = "cannot open \"" + path.string() + "\": " + std::strerror(errno);
    }
    return std::nullopt;
  }
  SessionDir dir;
  dir.fd_ = fd;
  dir.path_ = path;
  return dir;
}

std::optional<std::string> read_at_limited(int dirfd, std::string_view name,
                                           std::size_t size_limit, std::string& error) {
  error.clear();
  const std::string n(name);
  const int fd = ::openat(dirfd, n.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ELOOP) {
      error = "refusing to read \"" + n + "\": it is a symbolic link.";
    } else {
      error = "cannot open \"" + n + "\": " + std::strerror(errno);
    }
    return std::nullopt;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    error = "cannot stat \"" + n + "\": " + std::strerror(errno);
    ::close(fd);
    return std::nullopt;
  }
  if (static_cast<std::size_t>(st.st_size) > size_limit) {
    error = "refusing to read \"" + n + "\": it is " + std::to_string(st.st_size) +
            " bytes, above the limit of " + std::to_string(size_limit) + " bytes.";
    ::close(fd);
    return std::nullopt;
  }
  std::string out;
  out.resize(static_cast<std::size_t>(st.st_size));
  std::size_t got = 0;
  while (got < out.size()) {
    const ssize_t r = ::read(fd, out.data() + got, out.size() - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      error = "cannot read \"" + n + "\": " + std::strerror(errno);
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

int remove_stale_temporaries(int dirfd, std::string_view stem) {
  const int copy = ::dup(dirfd);
  if (copy < 0) return 0;
  DIR* dir = ::fdopendir(copy);
  if (dir == nullptr) {
    ::close(copy);
    return 0;
  }
  const std::string prefix = std::string(stem) + ".tmp.";
  std::vector<std::string> stale;
  while (const dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.rfind(prefix, 0) == 0) stale.push_back(name);
  }
  ::closedir(dir);

  int removed = 0;
  for (const std::string& name : stale) {
    if (::unlinkat(dirfd, name.c_str(), 0) == 0) ++removed;
  }
  return removed;
}

}  // namespace rtlangle
