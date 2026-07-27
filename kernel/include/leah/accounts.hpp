#pragma once

#include <leah/types.hpp>

// The account database, read by the kernel rather than by the program asking.
//
// A conventional UNIX puts the hashes in /etc/shadow, readable only by root,
// and lets an unprivileged `su` reach them by being setuid-root. There is no
// setuid-on-exec here, so that route is closed - and making the hashes
// world-readable to work around it would give away the thing worth protecting.
//
// Instead authentication happens inside the kernel. It reads the shadow file
// with the privileges it already has, checks the password, and switches the
// caller's credentials only if it matches. The password crosses the syscall
// boundary and no further; the hash is never in a user process at all.

namespace accounts {

constexpr usize kMaxName = 32;
constexpr usize kMaxHome = 128;

// Verify `password` for `user` and, if it is right, switch the calling task to
// that user's uid and gid. Writes the account's home directory to `home_out`.
//
// Root is not asked for a password - it can already become anyone by other
// means, and pretending otherwise would be theatre.
bool login(const char* user, const char* password, char* home_out,
           usize home_size);

// Look up an account without authenticating: what `whoami` and a prompt need.
bool lookup_uid(u32 uid, char* name_out, usize name_size);

} // namespace accounts
