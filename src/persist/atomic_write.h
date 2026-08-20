#pragma once

#include "core/records.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace rtlangle {

// The syscalls the write protocol uses, behind one injectable seam.
//
// A decorator over ISessionStore cannot produce the whole outcome matrix of
// spec section 11.1.1: it cannot make fsync(dirfd) fail AFTER renameat has
// already succeeded, which is exactly the CommittedNotDurable row, and it
// cannot produce an unclassifiable errno for the Indeterminate row. Injecting
// at the syscall boundary can produce all seven.
//
// The EINTR retry loops live inside write_atomic_at's wrappers around these,
// so a signal never becomes a CommitOutcome.
struct FileOps {
  std::function<int(int dirfd, const char* name, int flags, mode_t mode)> openat_fn;
  std::function<ssize_t(int fd, const void* buf, std::size_t count)> write_fn;
  std::function<int(int fd)> fsync_fn;
  std::function<int(int olddirfd, const char* oldname, int newdirfd, const char* newname)>
      renameat_fn;
  std::function<int(int fd)> close_fn;
  std::function<int(int dirfd, const char* name, int flags)> unlinkat_fn;
};

const FileOps& default_file_ops();

// The write protocol of spec section 11.1.1, in order:
//
//   1  serialise to <name>.tmp.<pid> via openat(O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW, 0600)
//   2  fsync the temporary file
//   3  renameat(dirfd, tmp, dirfd, name)          <- THE COMMIT POINT
//   4  fsync(dirfd), which makes the rename itself durable
//
// Everything before step 3 is preparation and changes nothing anyone can
// observe; everything after it is bookkeeping on a commit that has already
// happened. Step 4 failing therefore yields CommittedNotDurable, which is NOT a
// failed commit: the commit is in the record and every later read sees it, and
// retrying would duplicate the attempt.
CommitResult write_atomic_at(int dirfd, std::string_view name, std::string_view bytes,
                             const FileOps& ops = default_file_ops());

// True when this errno from renameat means the rename definitely did not
// happen. Anything else - EIO above all - leaves the outcome Indeterminate,
// because the platform does not let us tell "did not happen" from "happened".
bool renameat_errno_is_classifiable(int err);

// The session directory, opened once and held for the lifetime of the session.
//
// O_NOFOLLOW refuses a symlink at the FINAL path component only; every
// intermediate directory is still resolved normally. The property that does
// hold for intermediate components comes from holding this descriptor: once it
// is held, replacing any component of the path - including an intermediate
// directory - cannot redirect a write, because the kernel resolves nothing but
// the final name (spec section 13.2).
class SessionDir {
 public:
  SessionDir() = default;
  ~SessionDir();
  SessionDir(SessionDir&&) noexcept;
  SessionDir& operator=(SessionDir&&) noexcept;
  SessionDir(const SessionDir&) = delete;
  SessionDir& operator=(const SessionDir&) = delete;

  // mkdir(path, 0700) then open(O_DIRECTORY|O_NOFOLLOW|O_RDONLY). mkdir fails
  // with EEXIST if anything is already there, a symlink included, so the
  // directory this program writes into is one it created.
  static std::optional<SessionDir> create(const std::filesystem::path&, std::string& error);
  static std::optional<SessionDir> open_existing(const std::filesystem::path&, std::string& error);

  int fd() const { return fd_; }
  const std::filesystem::path& path() const { return path_; }
  bool valid() const { return fd_ >= 0; }

 private:
  int                   fd_ = -1;
  std::filesystem::path path_;
};

// Reads a file relative to a held directory descriptor, refusing a symlink at
// that name and anything above `size_limit` bytes.
std::optional<std::string> read_at_limited(int dirfd, std::string_view name,
                                           std::size_t size_limit, std::string& error);

// Removes every session.json.tmp.* left behind by an interrupted write. Returns
// how many were removed.
int remove_stale_temporaries(int dirfd, std::string_view stem);

}  // namespace rtlangle
