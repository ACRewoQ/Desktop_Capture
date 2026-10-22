#pragma once

#include <cstddef>
#include <string>

namespace dc::shm {

// A named, page-backed shared memory region (CreateFileMapping on Windows, shm_open elsewhere).
class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory();
    SharedMemory(SharedMemory&& other) noexcept;
    SharedMemory& operator=(SharedMemory&& other) noexcept;
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    // nameSuffix lets tests use a private name; production code passes "".
    static SharedMemory create(std::size_t bytes, const std::string& nameSuffix, std::string* error);
    static SharedMemory open(std::size_t bytes, const std::string& nameSuffix, std::string* error);

    void* data() const { return data_; }
    std::size_t size() const { return size_; }
    bool valid() const { return data_ != nullptr; }
    void reset();

private:
    void* data_ = nullptr;
    std::size_t size_ = 0;
    void* handle_ = nullptr; // HANDLE on Windows
    bool owner_ = false;
    std::string name_;       // POSIX name for shm_unlink
};

} // namespace dc::shm
