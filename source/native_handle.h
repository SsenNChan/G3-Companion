#pragma once
#include <windows.h>
#include <utility>
#include <string>

class NativeHandle {
public:
    HANDLE value=INVALID_HANDLE_VALUE;
    NativeHandle()=default;
    explicit NativeHandle(HANDLE handle):value(handle){}
    NativeHandle(const NativeHandle&)=delete;
    NativeHandle& operator=(const NativeHandle&)=delete;
    NativeHandle(NativeHandle&& other) noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if(this!=&other)reset(std::exchange(other.value,INVALID_HANDLE_VALUE));return *this;
    }
    ~NativeHandle(){reset();}
    void reset(HANDLE handle=INVALID_HANDLE_VALUE) noexcept {
        if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=handle;
    }
};

class TemporaryFile {
    std::wstring m_path;
public:
    explicit TemporaryFile(std::wstring path):m_path(std::move(path)){}
    ~TemporaryFile(){const auto error=GetLastError();DeleteFileW(m_path.c_str());SetLastError(error);}
    TemporaryFile(TemporaryFile const&)=delete;
    TemporaryFile& operator=(TemporaryFile const&)=delete;
};
