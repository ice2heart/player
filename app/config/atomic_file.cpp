#include "atomic_file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace oap {

namespace {
constexpr mode_t kFileMode = 0600; // config.yaml holds the server password

bool fail(const std::filesystem::path& file, const char* what) {
    std::fprintf(stderr, "cannot write %s: %s: %s\n", file.c_str(), what, std::strerror(errno));
    return false;
}

bool writeAll(int fd, const std::string& contents) {
    const char* p = contents.data();
    size_t left = contents.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        left -= size_t(n);
    }
    return true;
}

// Makes the rename itself survive a power cut.
void syncFolder(const std::filesystem::path& folder) {
    int fd = ::open(folder.empty() ? "." : folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}
} // namespace

bool writeFileAtomically(const std::filesystem::path& file, const std::string& contents) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
    if (fd < 0) return fail(tmp, "open");
    // A temp file left over from an earlier crash keeps its old mode; open() only sets it on creation.
    bool ok = ::fchmod(fd, kFileMode) == 0 && writeAll(fd, contents) && ::fsync(fd) == 0;
    int savedErrno = errno;
    if (::close(fd) != 0 && ok) ok = false, savedErrno = errno;
    if (ok && ::rename(tmp.c_str(), file.c_str()) != 0) ok = false, savedErrno = errno;
    if (!ok) {
        errno = savedErrno;
        fail(file, "write");
        ::unlink(tmp.c_str());
        return false;
    }
    syncFolder(file.parent_path());
    return true;
}

std::optional<std::string> readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace oap
