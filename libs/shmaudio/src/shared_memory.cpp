#include "dc/shmaudio/shared_memory.hpp"
#include "dc/shmaudio/layout.hpp"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace dc::shm {

SharedMemory::~SharedMemory() { reset(); }

SharedMemory::SharedMemory(SharedMemory&& other) noexcept { *this = std::move(other); }

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        handle_ = std::exchange(other.handle_, nullptr);
        owner_ = std::exchange(other.owner_, false);
        name_ = std::move(other.name_);
    }
    return *this;
}

#ifdef _WIN32

namespace {
std::wstring mappingName(const std::string& suffix) {
    std::wstring name = kWindowsMappingName;
    for (char c : suffix) name += static_cast<wchar_t>(c);
    return name;
}

std::string lastErrorText(const char* what) { return std::string(what) + " failed, error " + std::to_string(GetLastError()); }
} // namespace

SharedMemory SharedMemory::create(std::size_t bytes, const std::string& nameSuffix, std::string* error) {
    SharedMemory m;
    const auto name = mappingName(nameSuffix);
    const auto size64 = static_cast<unsigned long long>(bytes);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size64 >> 32),
                                  static_cast<DWORD>(size64 & 0xFFFFFFFFull), name.c_str());
    if (!h) {
        if (error) *error = lastErrorText("CreateFileMappingW");
        return m;
    }
    void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (!p) {
        if (error) *error = lastErrorText("MapViewOfFile");
        CloseHandle(h);
        return m;
    }
    m.data_ = p;
    m.size_ = bytes;
    m.handle_ = h;
    m.owner_ = true;
    return m;
}

SharedMemory SharedMemory::open(std::size_t bytes, const std::string& nameSuffix, std::string* error) {
    SharedMemory m;
    const auto name = mappingName(nameSuffix);
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!h) {
        if (error) *error = lastErrorText("OpenFileMappingW");
        return m;
    }
    void* p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, bytes);
    if (!p) {
        if (error) *error = lastErrorText("MapViewOfFile");
        CloseHandle(h);
        return m;
    }
    m.data_ = p;
    m.size_ = bytes;
    m.handle_ = h;
    return m;
}

void SharedMemory::reset() {
    if (data_) UnmapViewOfFile(data_);
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    data_ = nullptr;
    handle_ = nullptr;
    size_ = 0;
    owner_ = false;
}

#else

SharedMemory SharedMemory::create(std::size_t bytes, const std::string& nameSuffix, std::string* error) {
    SharedMemory m;
    const std::string name = std::string(kPosixMappingName) + nameSuffix;
    const int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        if (error) *error = std::string("shm_open: ") + std::strerror(errno);
        return m;
    }
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        if (error) *error = std::string("ftruncate: ") + std::strerror(errno);
        close(fd);
        return m;
    }
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        if (error) *error = std::string("mmap: ") + std::strerror(errno);
        return m;
    }
    m.data_ = p;
    m.size_ = bytes;
    m.owner_ = true;
    m.name_ = name;
    return m;
}

SharedMemory SharedMemory::open(std::size_t bytes, const std::string& nameSuffix, std::string* error) {
    SharedMemory m;
    const std::string name = std::string(kPosixMappingName) + nameSuffix;
    const int fd = shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        if (error) *error = std::string("shm_open: ") + std::strerror(errno);
        return m;
    }
    void* p = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        if (error) *error = std::string("mmap: ") + std::strerror(errno);
        return m;
    }
    m.data_ = p;
    m.size_ = bytes;
    return m;
}

void SharedMemory::reset() {
    if (data_) munmap(data_, size_);
    if (owner_ && !name_.empty()) shm_unlink(name_.c_str());
    data_ = nullptr;
    size_ = 0;
    owner_ = false;
    name_.clear();
}

#endif

} // namespace dc::shm
