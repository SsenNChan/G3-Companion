#pragma once
#include <objbase.h>
#include "macro_storage.h"
#include "clock_compensation.h"
#include "log_storage.h"
#include <cstring>
#include <fstream>
#include <filesystem>

namespace ClockCalibration {
inline constexpr std::uint32_t TestDelayMs=10000,TimeoutMs=20000;
struct Backup {std::wstring devicePath;ManagerBackend::ParameterReport mappings{};ManagerBackend::MacroStorage macros{};};
inline std::wstring RecoveryPath(std::wstring const& root){return LogStorage::Path(root,L"clock-calibration-recovery.bin");}
inline std::uint64_t Hash(std::vector<std::uint8_t> const& bytes,std::size_t length){std::uint64_t hash=14695981039346656037ULL;for(std::size_t i=0;i<length;++i){hash^=bytes[i];hash*=1099511628211ULL;}return hash;}
inline bool SaveBackup(std::wstring const& path,Backup const& backup){
    if(backup.devicePath.size()>32768)return false;std::vector<std::uint8_t> bytes={'G','3','C','L','K','R','0','1'};
    const auto append=[&](void const* data,std::size_t size){auto first=static_cast<std::uint8_t const*>(data);bytes.insert(bytes.end(),first,first+size);};
    const auto length=static_cast<std::uint32_t>(backup.devicePath.size());append(&length,sizeof(length));append(backup.devicePath.data(),length*sizeof(wchar_t));append(backup.mappings.data(),backup.mappings.size());append(backup.macros.data(),backup.macros.size());const auto hash=Hash(bytes,bytes.size());append(&hash,sizeof(hash));
    GUID id{};const auto created=CoCreateGuid(&id);if(FAILED(created)){SetLastError(ERROR_GEN_FAILURE);return false;}
    wchar_t suffix[40]{};StringFromGUID2(id,suffix,40);const auto temporary=path+L"."+suffix+L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    TemporaryFile cleanup(temporary);
    DWORD written=0;const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);CloseHandle(file);
    return ok&&MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
inline bool LoadBackup(std::wstring const& path,Backup& backup){
    std::ifstream file(std::filesystem::path(path),std::ios::binary|std::ios::ate);if(!file)return false;const auto length=file.tellg();if(length<0||length>100000)return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());if(!file||bytes.size()<12+520+4096+8||std::memcmp(bytes.data(),"G3CLKR01",8))return false;
    std::uint32_t characters=0;std::memcpy(&characters,bytes.data()+8,4);if(characters>32768||bytes.size()!=12+characters*sizeof(wchar_t)+520+4096+8)return false;
    std::uint64_t hash=0;std::memcpy(&hash,bytes.data()+bytes.size()-8,8);if(hash!=Hash(bytes,bytes.size()-8))return false;
    Backup decoded;decoded.devicePath.resize(characters);std::memcpy(decoded.devicePath.data(),bytes.data()+12,characters*sizeof(wchar_t));auto offset=12+characters*sizeof(wchar_t);
    std::memcpy(decoded.mappings.data(),bytes.data()+offset,520);std::memcpy(decoded.macros.data(),bytes.data()+offset+520,4096);
    if(!ManagerBackend::ValidateMappingReply(decoded.mappings,ManagerBackend::MappingReplyLength))return false;backup=std::move(decoded);return true;
}
inline bool BuildPreparation(Backup const& backup,ManagerBackend::MacroStorage& macros,ManagerBackend::ParameterReport& mappings,std::wstring& error){
    ManagerBackend::Settings draft;ManagerBackend::ButtonMappings original{},resolved{};
    if(!ManagerBackend::ParseMappings(backup.mappings,ManagerBackend::MappingReplyLength,original,error))return false;draft.buttons=original;
    MacroEditor::Profile profile;profile.name=L"ClockCalibration";profile.steps={{MacroEditor::StepKind::Keyboard,0x49,true},{MacroEditor::StepKind::Delay,TestDelayMs,true},{MacroEditor::StepKind::Keyboard,0x49,false}};
    MacroEditor::Assignment assignment;assignment.name=profile.name;assignment.executionMode=1;assignment.repeatCount=1;
    if(!MacroEditor::Encode(profile,"ClockCalibration",assignment.data,error))return false;draft.macroAssignments[5]=assignment;
    if(!ManagerBackend::BuildMacroStorage(backup.macros,backup.mappings,draft,macros,resolved,error))return false;auto baseline=original;baseline[5]={0xff,0xff,0xff,0xff};ManagerBackend::ButtonMappings expected;
    return ManagerBackend::BuildMappingWrite(backup.mappings,resolved,baseline,mappings,expected,error);
}
struct Measurement {
    enum class Status { Armed,Running,Complete,UnexpectedInput,PointerOutside,TimedOut };
    Status status=Status::Armed;std::int64_t start=0,frequency=1;double elapsedMs=0,factor=0;
    void Reset(std::int64_t counterFrequency){*this={};frequency=counterFrequency;}
    void Fail(Status reason=Status::UnexpectedInput){if(status==Status::Armed||status==Status::Running)status=reason;}
    void Button(bool target,bool inside,bool down,bool up,bool other,std::int64_t counter){
        if(status!=Status::Armed&&status!=Status::Running)return;
        if(!inside){Fail(Status::PointerOutside);return;}
        if(!target||other||down==up){Fail();return;}
        if(status==Status::Armed){if(!down){Fail();return;}start=counter;status=Status::Running;return;}
        if(down){Fail();return;}elapsedMs=(counter-start)*1000.0/frequency;
        if(elapsedMs<5000||elapsedMs>=TimeoutMs){Fail();return;}factor=TestDelayMs/elapsedMs;status=Status::Complete;
    }
    void Tick(std::int64_t counter){if(status==Status::Running&&(counter-start)*1000.0/frequency>=TimeoutMs)status=Status::TimedOut;}
};
}
