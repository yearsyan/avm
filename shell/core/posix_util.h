// SPDX-License-Identifier: MIT

#ifndef MACMU_SHELL_POSIX_UTIL_H
#define MACMU_SHELL_POSIX_UTIL_H

#include <fcntl.h>

#include <string>

namespace macmu::shell {

inline std::string path_join(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs.back() == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

inline bool set_close_on_exec(int fd, bool enabled = true) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return false;
    }
    const int nextFlags = enabled ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
    return fcntl(fd, F_SETFD, nextFlags) == 0;
}

}  // namespace macmu::shell

#endif  // MACMU_SHELL_POSIX_UTIL_H
