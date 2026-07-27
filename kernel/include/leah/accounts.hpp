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
// Identity changes flow through root. An ordinary user may only become root;
// becoming a *different* ordinary user means going to root first and coming
// back down. Root may become anyone - but every step needs the target account's
// own password, root included, so holding root is not the same as holding
// everyone's identity.
//
// That is a policy about who you may *become*, not a barrier around files: uid
// 0 still bypasses permission checks, so root can read anyone's data directly.
// Making it otherwise would leave nobody able to administer the system.
bool login(const char* user, const char* password, char* home_out,
           usize home_size);

// Create an account, generating a salt and hashing the password. Root only.
// A uid of 0 means "allocate the next free one" - a second root is never what
// was meant, and a uid that is already taken would silently make two accounts
// the same person, since permission checks work on the number and not the name.
// False if the name or uid is taken, the caller is not root, or the files
// cannot be written.
bool create(const char* user, const char* password, u32 uid, u32 gid,
            const char* home);

// Change an account's password. Root may change anyone's without knowing the
// old one; anyone else may change only their own, and must prove it.
bool set_password(const char* user, const char* old_password,
                  const char* new_password);

// True when an account of this name exists.
bool exists(const char* user);

// Look up an account without authenticating: what `whoami` and a prompt need.
bool lookup_uid(u32 uid, char* name_out, usize name_size);

} // namespace accounts
