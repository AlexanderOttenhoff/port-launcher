#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace plm {

// Random-access read-only bytes: a ROM file, a zip, or anything the platform layer can hand over.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual std::uint64_t size() const = 0;
    // Reads exactly out.size() bytes at offset; false on error or short read.
    virtual bool read_at(std::uint64_t offset, std::span<std::uint8_t> out) = 0;
};

// A POSIX file descriptor read with pread(). Android passes descriptors from the Storage Access Framework,
// so the core never sees content:// URIs.
class FdByteSource final : public ByteSource {
public:
    // Takes ownership of fd when owns is true.
    FdByteSource(int fd, bool owns);
    ~FdByteSource() override;
    FdByteSource(const FdByteSource&) = delete;
    FdByteSource& operator=(const FdByteSource&) = delete;

    static std::unique_ptr<FdByteSource> open(const std::string& path);

    bool valid() const { return fd_ >= 0 && size_known_; }
    std::uint64_t size() const override { return size_; }
    bool read_at(std::uint64_t offset, std::span<std::uint8_t> out) override;

private:
    int fd_;
    bool owns_;
    bool size_known_ = false;
    std::uint64_t size_ = 0;
};

} // namespace plm
