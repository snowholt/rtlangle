#pragma once

// Two injection seams, because one cannot do both jobs.
//
// FaultOps injects at the SYSCALL boundary, which is the only place that can
// produce the whole outcome matrix of spec section 11.1.1: a store decorator
// cannot make fsync(dirfd) fail AFTER renameat has already succeeded - the
// CommittedNotDurable row - and cannot produce an unclassifiable errno for the
// Indeterminate row.
//
// FaultStore decorates ISessionStore, which is what the controller and the app
// layer talk to, and is how their behaviour in the face of each outcome is
// tested without a filesystem.

#include "persist/atomic_write.h"
#include "persist/session_store.h"

#include <cerrno>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rtlangle::test {

// Every boundary the write protocol can fail at.
enum class FaultPoint {
  None,
  BeforeTempWrite,      // openat of the temporary file
  AfterTempBeforeSync,  // write to the temporary file
  AtFileSync,           // fsync of the temporary file
  AtRename,             // renameat, with a classifiable errno
  AtRenameUnclassified, // renameat, with an errno that does not classify
  AtDirSync,            // fsync of the directory, the rename having succeeded
  AtCsvWrite,           // the derived export, after a durable commit
};

// Builds a FileOps that fails at exactly one boundary, on the first commit that
// reaches it. `target` names which file the fault applies to, so a CSV-only
// fault can be injected without disturbing session.json.
inline FileOps faulty_ops(FaultPoint point, int forced_errno = EACCES,
                          std::string target = "session.json") {
  const FileOps& real = default_file_ops();
  FileOps ops = real;
  auto state = std::make_shared<bool>(false);   // has the fault fired yet
  auto tmp_fd = std::make_shared<int>(-1);
  auto dir_fd = std::make_shared<int>(-1);

  const std::string tmp_prefix = target + ".tmp.";

  ops.openat_fn = [real, point, forced_errno, state, tmp_fd, tmp_prefix](
                      int dirfd, const char* name, int flags, mode_t mode) {
    const bool is_target = std::string(name).rfind(tmp_prefix, 0) == 0;
    if (is_target && point == FaultPoint::BeforeTempWrite && !*state) {
      *state = true;
      errno = forced_errno;
      return -1;
    }
    const int fd = real.openat_fn(dirfd, name, flags, mode);
    if (is_target) *tmp_fd = fd;
    return fd;
  };

  ops.write_fn = [real, point, forced_errno, state, tmp_fd](int fd, const void* buf,
                                                            std::size_t count) {
    if (fd == *tmp_fd && point == FaultPoint::AfterTempBeforeSync && !*state) {
      *state = true;
      errno = forced_errno;
      return static_cast<ssize_t>(-1);
    }
    return real.write_fn(fd, buf, count);
  };

  ops.fsync_fn = [real, point, forced_errno, state, tmp_fd, dir_fd](int fd) {
    if (fd == *tmp_fd && point == FaultPoint::AtFileSync && !*state) {
      *state = true;
      errno = forced_errno;
      return -1;
    }
    if (fd == *dir_fd && point == FaultPoint::AtDirSync && !*state) {
      *state = true;
      errno = forced_errno;
      return -1;
    }
    return real.fsync_fn(fd);
  };

  ops.renameat_fn = [real, point, forced_errno, state, dir_fd, tmp_prefix](
                        int olddirfd, const char* oldname, int newdirfd, const char* newname) {
    const bool is_target = std::string(oldname).rfind(tmp_prefix, 0) == 0;
    if (is_target && !*state) {
      if (point == FaultPoint::AtRename) {
        *state = true;
        errno = forced_errno;   // a classifiable errno: the rename did not happen
        return -1;
      }
      if (point == FaultPoint::AtRenameUnclassified) {
        *state = true;
        errno = EIO;   // does not distinguish "did not happen" from "happened"
        return -1;
      }
    }
    // The directory descriptor is only knowable here, and the directory fsync
    // that follows is the CommittedNotDurable boundary.
    *dir_fd = newdirfd;
    return real.renameat_fn(olddirfd, oldname, newdirfd, newname);
  };

  return ops;
}

// A signal-delivering FileOps: the first call to each of openat, write, and
// fsync fails with EINTR, which the wrappers must retry internally so that a
// signal never becomes a CommitOutcome.
inline FileOps eintr_once_ops() {
  const FileOps& real = default_file_ops();
  FileOps ops = real;
  auto open_done = std::make_shared<bool>(false);
  auto write_done = std::make_shared<bool>(false);
  auto sync_done = std::make_shared<bool>(false);

  ops.openat_fn = [real, open_done](int dirfd, const char* name, int flags, mode_t mode) {
    if (!*open_done) {
      *open_done = true;
      errno = EINTR;
      return -1;
    }
    return real.openat_fn(dirfd, name, flags, mode);
  };
  ops.write_fn = [real, write_done](int fd, const void* buf, std::size_t count) {
    if (!*write_done) {
      *write_done = true;
      errno = EINTR;
      return static_cast<ssize_t>(-1);
    }
    return real.write_fn(fd, buf, count);
  };
  ops.fsync_fn = [real, sync_done](int fd) {
    if (!*sync_done) {
      *sync_done = true;
      errno = EINTR;
      return -1;
    }
    return real.fsync_fn(fd);
  };
  return ops;
}

// Counts fsync calls, so a test can assert that EIO is not retried.
inline FileOps counting_fsync_ops(std::shared_ptr<int> calls, int forced_errno) {
  const FileOps& real = default_file_ops();
  FileOps ops = real;
  auto tmp_fd = std::make_shared<int>(-1);
  ops.openat_fn = [real, tmp_fd](int dirfd, const char* name, int flags, mode_t mode) {
    const int fd = real.openat_fn(dirfd, name, flags, mode);
    if (std::string(name).find(".tmp.") != std::string::npos) *tmp_fd = fd;
    return fd;
  };
  ops.fsync_fn = [real, calls, tmp_fd, forced_errno](int fd) {
    if (fd == *tmp_fd) {
      ++*calls;
      errno = forced_errno;
      return -1;
    }
    return real.fsync_fn(fd);
  };
  return ops;
}

// The ISessionStore decorator. It records every call and can force a chosen
// outcome from any of the five methods that return a CommitResult, which is how
// the caller-behaviour requirements of spec section 11.1.1 are tested.
class FaultStore final : public ISessionStore {
 public:
  explicit FaultStore(ISessionStore& inner) : inner_(inner) {}

  // Forced outcomes. An unset optional means "pass through to the inner store".
  std::optional<CommitOutcome> commit_outcome;
  std::optional<CommitOutcome> reload_outcome;
  std::optional<CommitOutcome> pause_outcome;
  std::optional<CommitOutcome> finalize_outcome;
  std::optional<CommitOutcome> abort_outcome;

  // Call counts, so a test can assert that the controller never calls pause,
  // finalize, or abort, and that a retry costs exactly one commit_visit.
  int begin_segment_calls = 0;
  int commit_calls = 0;
  int reload_calls = 0;
  int pause_calls = 0;
  int finalize_calls = 0;
  int abort_calls = 0;

  std::vector<VisitCommit> commits;

  const SessionRecord& record() const override { return inner_.record(); }

  std::string begin_receiver_segment(const SourceInfo& applied) override {
    ++begin_segment_calls;
    return inner_.begin_receiver_segment(applied);
  }

  CommitResult commit_visit(const VisitCommit& commit) override {
    ++commit_calls;
    commits.push_back(commit);
    if (commit_outcome.has_value()) {
      CommitResult r;
      r.outcome = *commit_outcome;
      r.detail = "injected " + std::string(to_string(*commit_outcome));
      // A forced NotCommitted must leave the record untouched, so the inner
      // store is not called at all.
      if (*commit_outcome == CommitOutcome::CommittedNotDurable) {
        (void)inner_.commit_visit(commit);
      }
      return r;
    }
    return inner_.commit_visit(commit);
  }

  CommitResult reload_from_disk() override {
    ++reload_calls;
    if (reload_outcome.has_value()) {
      CommitResult r;
      r.outcome = *reload_outcome;
      r.detail = "injected " + std::string(to_string(*reload_outcome));
      return r;
    }
    return inner_.reload_from_disk();
  }

  CommitResult pause(const SessionSummary& partial) override {
    ++pause_calls;
    if (pause_outcome.has_value()) {
      CommitResult r;
      r.outcome = *pause_outcome;
      r.detail = "injected " + std::string(to_string(*pause_outcome));
      return r;
    }
    return inner_.pause(partial);
  }

  CommitResult finalize(const SessionSummary& summary) override {
    ++finalize_calls;
    if (finalize_outcome.has_value()) {
      CommitResult r;
      r.outcome = *finalize_outcome;
      r.detail = "injected " + std::string(to_string(*finalize_outcome));
      return r;
    }
    return inner_.finalize(summary);
  }

  CommitResult abort(const SessionSummary& partial, std::string_view reason) override {
    ++abort_calls;
    if (abort_outcome.has_value()) {
      CommitResult r;
      r.outcome = *abort_outcome;
      r.detail = "injected " + std::string(to_string(*abort_outcome));
      return r;
    }
    return inner_.abort(partial, reason);
  }

  void flush_exports() override { inner_.flush_exports(); }

 private:
  ISessionStore& inner_;
};

}  // namespace rtlangle::test
