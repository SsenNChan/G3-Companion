#pragma once
#include "localization.h"
#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <array>
#include <string>

namespace Startup {
enum class State { Disabled, Enabled, RepairNeeded, Error };

// The caller initializes COM and decides whether an existing entry should be repaired.
inline State Inspect(const std::wstring& path,const std::wstring& executable,const std::wstring& directory,const std::wstring& arguments=L"") {
    if(path.empty())return State::Error;
    DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){
        DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?State::Disabled:State::Error;
    }
    if(attributes&FILE_ATTRIBUTE_DIRECTORY)return State::Error;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    HRESULT hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link));
    if(FAILED(hr))return State::Error;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if(FAILED(link.As(&file)))return State::Error;
    hr=file->Load(path.c_str(),STGM_READ);
    if(FAILED(hr)){
        if(hr==E_FAIL||hr==STG_E_INVALIDHEADER||hr==STG_E_DOCFILECORRUPT||hr==STG_E_OLDFORMAT||
           hr==HRESULT_FROM_WIN32(ERROR_INVALID_DATA)||hr==HRESULT_FROM_WIN32(ERROR_BAD_FORMAT))return State::RepairNeeded;
        return State::Error;
    }
    std::array<wchar_t,32768> value{};
    if(FAILED(link->GetPath(value.data(),static_cast<int>(value.size()),nullptr,SLGP_RAWPATH)))return State::Error;
    if(CompareStringOrdinal(value.data(),-1,executable.c_str(),-1,TRUE)!=CSTR_EQUAL)return State::RepairNeeded;
    value.fill(0);
    if(FAILED(link->GetWorkingDirectory(value.data(),static_cast<int>(value.size()))))return State::Error;
    if(CompareStringOrdinal(value.data(),-1,directory.c_str(),-1,TRUE)!=CSTR_EQUAL)return State::RepairNeeded;
    value.fill(0);
    if(FAILED(link->GetArguments(value.data(),static_cast<int>(value.size()))))return State::Error;
    return arguments==value.data()?State::Enabled:State::RepairNeeded;
}

inline HRESULT Repair(const std::wstring& path,const std::wstring& executable,const std::wstring& directory,const std::wstring& arguments=L"") {
    if(path.empty()||executable.empty()||directory.empty())return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    HRESULT hr=CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link));
    if(FAILED(hr))return hr;
    if(FAILED(hr=link->SetPath(executable.c_str())))return hr;
    if(FAILED(hr=link->SetArguments(arguments.c_str())))return hr;
    if(FAILED(hr=link->SetWorkingDirectory(directory.c_str())))return hr;
    if(FAILED(hr=link->SetIconLocation(executable.c_str(),0)))return hr;
    if(FAILED(hr=link->SetDescription(Localization::Get(Localization::T278))))return hr;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if(FAILED(hr=link.As(&file)))return hr;
    return file->Save(path.c_str(),TRUE);
}
}
