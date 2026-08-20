#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace rtlangle {

// Spec section 13.1 states the threat model before the mitigations, and this
// header keeps to it. These helpers are defence in depth against accidents and
// against a hostile session root - a --session-dir on a shared filesystem, a
// /tmp-like location, or a path an unrelated program also writes to. They are
// NOT a defence against an attacker who already has write access to the
// operator's home directory or to the parent of the session root; such an
// attacker can replace the whole directory before the program starts, and no
// file-open flag helps.

// Size limits, spec section 13.4.
inline constexpr std::size_t kMaxConfigFileBytes = 1024ULL * 1024;        // 1 MiB
inline constexpr std::size_t kMaxSidecarBytes = 1024ULL * 1024;           // 1 MiB
inline constexpr std::size_t kMaxSessionJsonBytes = 64ULL * 1024 * 1024;  // 64 MiB
inline constexpr std::size_t kMaxCliStringBytes = 4096;

// Opens a path for reading, refusing a symlink AT THE FINAL COMPONENT.
//
// O_NOFOLLOW refuses a symlink at the final path component only. Every
// intermediate directory is still resolved normally, symlinks included, so this
// does NOT protect an intermediate component - a symlink at `sessions/`
// redirects everything underneath and O_NOFOLLOW on the file below it never
// sees that. The property that does hold for intermediate components comes from
// holding a directory descriptor, which is what SessionDir provides.
//
// Returns -1 and sets `error` on failure, naming the path.
int open_read_no_symlink(std::string_view path, std::string& error);

// Reads a whole file through open_read_no_symlink, refusing anything above
// `size_limit` bytes with a message naming the path and both sizes.
std::optional<std::string> read_file_limited(std::string_view path, std::size_t size_limit,
                                             std::string& error);

// The final component of a path, used for a source's device_name.
std::string basename_of(std::string_view path);

// Spec section 13.3. `--label` becomes a SINGLE path component and is never
// treated as a path. The algorithm, in this order:
//
//   1  replace every character outside [A-Za-z0-9._-] with '-'
//   2  collapse runs of '-' to one, AND runs of '.' to one
//   3  strip leading and trailing '.' and '-'
//   4  truncate to 32 characters, then strip trailing '.' and '-' again,
//      because the truncation can expose one
//   5  if the result is empty, "." or "..", the label is dropped entirely
//
// Step 2's dot-run collapse is what makes ".." unrepresentable: after it no two
// dots are adjacent, so no substring ".." exists anywhere in the result, not
// merely at the start. A rule that stripped only LEADING dots would let "a..b"
// through unchanged while claiming that ".." can never appear.
//
// The guarantee: the output contains no '/', no substring "..", and is neither
// "." nor "..". An input that cannot satisfy that yields an empty string, which
// the caller drops.
std::string slugify_label(std::string_view);

// The invariant sanitisation exists to satisfy. An assembled directory name
// must contain no '/', and must be neither "." nor "..".
bool valid_directory_component(std::string_view);

}  // namespace rtlangle
