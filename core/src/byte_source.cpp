#include "plm/byte_source.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace plm {

FdByteSource::FdByteSource(int fd, bool owns) : fd_(fd), owns_(owns) {
    struct stat st {};
    if (fd_ >= 0 && fstat(fd_, &st) == 0 && S_ISREG(st.st_mode)) {
        size_ = static_cast<std::uint64_t>(st.st_size);
        size_known_ = true;
    }
}

FdByteSource::~FdByteSource() {
    if (owns_ && fd_ >= 0) {
        close(fd_);
    }
}

std::unique_ptr<FdByteSource> FdByteSource::open(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    return std::make_unique<FdByteSource>(fd, true);
}

bool FdByteSource::read_at(std::uint64_t offset, std::span<std::uint8_t> out) {
    if (!valid() || offset > size_ || out.size() > size_ - offset) {
        return false;
    }
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = pread(fd_, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace plm
