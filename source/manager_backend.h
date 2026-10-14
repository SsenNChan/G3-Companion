#pragma once
#include "localization.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include "history.h"
#include "alpha_device.h"
#include "manager_model.h"
#include "feature_response.h"
#include "button_mapping.h"
#include "macro_storage.h"
#include "clock_calibration.h"

namespace ManagerBackend {
inline const auto EmptyMacros=std::make_shared<const std::vector<MacroConfiguration>>();
struct Snapshot {
    Device::Result status;
    Settings settings;
    bool buttonsValid=false;
    std::shared_ptr<const std::vector<MacroConfiguration>> macros=EmptyMacros;
    bool configurationValid=false;
    bool parameterReadFailed=false;
    bool operationFinished=false;
    bool applied=false;
    DWORD error=0;
    std::wstring message;
    ApplyMode calibrationOperation=ApplyMode::EditedSettings;
    bool calibrationSucceeded=false,calibrationRecoveryPending=false;
};

namespace Detail {
constexpr DWORD PollInterval=600000;
inline HANDLE stopEvent=nullptr,refreshEvent=nullptr,applyEvent=nullptr,worker=nullptr;
inline std::atomic<bool> topologyChanged{true};
inline HWND target=nullptr;
inline UINT resultMessage=0;
inline std::wstring directory;
inline Snapshot initial;
inline Device::Recorder recorder;
inline std::mutex requestMutex;
inline Settings requestedDraft,requestedBaseline;
inline ApplyMode requestedMode=ApplyMode::EditedSettings;

inline void RecordConfigurationRead(const char* stage,DWORD value,DWORD error) {
    if(!ProgramLog::Enabled())return;
    std::string event=stage;
    const auto separator=event.find('-');const std::string phase=separator==std::string::npos?"":event.substr(separator+1);
    if(separator!=std::string::npos)event[separator]='_';
    ProgramLog::Write(event.c_str(),std::string(phase=="send"?"duration_ms=":"bytes=")+std::to_string(value),error);
}

template<class Resume> struct FeatureInputGuard {
    Resume& resume;
    ~FeatureInputGuard(){resume();}
};

template<class Identify,class Connection>
inline bool CheckWirelessSession(bool& identifierAttempted,bool& online,DWORD& error,Identify identify,Connection connection) {
    online=false;
    if(!identifierAttempted){
        identifierAttempted=true;
        if(identify(online,error))return true;
        if(error==ERROR_OPERATION_ABORTED)return false;
    }
    BYTE state=0;
    if(!connection(state,error))return false;
    if(state>1){error=ERROR_INVALID_DATA;return false;}
    online=state==1;error=0;return true;
}

inline void Publish(const Snapshot& snapshot,bool logStatus=false) {
    if(WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)return;
    auto copy=std::make_unique<Snapshot>(snapshot);
    
    if(logStatus){
        copy->status.recorded=recorder.Record(copy->status);
    }
    if(ProgramLog::Enabled()&&(snapshot.calibrationOperation==ApplyMode::PrepareClockCalibration||snapshot.calibrationOperation==ApplyMode::RestoreClockCalibration))
        ProgramLog::Write(snapshot.calibrationOperation==ApplyMode::PrepareClockCalibration?"clock_calibration_prepare_completed":"clock_calibration_restore_completed",std::string("success=")+(snapshot.calibrationSucceeded?"1":"0"),snapshot.error);
    else if(ProgramLog::Enabled()&&snapshot.operationFinished)ProgramLog::Write("settings_apply_completed",std::string("applied=")+(snapshot.applied?"1":"0"),snapshot.error);
    if(PostMessageW(target,resultMessage,0,reinterpret_cast<LPARAM>(copy.get())))copy.release();
}

using Device::Enumerate;

inline bool ReadFeature(HANDLE device,ParameterReport& report,DWORD& received,DWORD& error,DWORD timeout=2000) {
    report.fill(0);report[0]=9;
    Device::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!event.value){error=GetLastError();return false;}
    OVERLAPPED overlapped{};overlapped.hEvent=event.value;received=0;
    BOOL ok=DeviceIoControl(device,0x000b0192,report.data(),static_cast<DWORD>(report.size()),
        report.data(),static_cast<DWORD>(report.size()),&received,&overlapped);
    if(!ok){
        error=GetLastError();if(error!=ERROR_IO_PENDING)return false;
        HANDLE waits[]={stopEvent,event.value};const DWORD wait=WaitForMultipleObjects(2,waits,FALSE,timeout);
        if(wait!=WAIT_OBJECT_0+1){
            const DWORD waitError=wait==WAIT_FAILED?GetLastError():0;
            CancelIoEx(device,&overlapped);GetOverlappedResult(device,&overlapped,&received,TRUE);
            error=wait==WAIT_OBJECT_0?ERROR_OPERATION_ABORTED:wait==WAIT_FAILED?waitError:ERROR_TIMEOUT;return false;
        }
        if(!GetOverlappedResult(device,&overlapped,&received,FALSE)){error=GetLastError();return false;}
    }
    error=0;return true;
}
template<class Matches>
inline bool ReadMatchingFeature(HANDLE device,ParameterReport& report,DWORD& received,DWORD& error,Matches matches) {
    const auto deadline=GetTickCount64()+2000;
    const bool ok=AwaitMatchingFeatureResponse(
        [&](std::uint32_t timeout){return ReadFeature(device,report,received,error,timeout);},
        [&](std::uint32_t delay){
            if(WaitForSingleObject(stopEvent,delay)==WAIT_TIMEOUT)return true;
            error=ERROR_OPERATION_ABORTED;return false;
        },
        [&](){return matches(report,received);},[](){return GetTickCount64();},deadline);
    if(!ok&&!error)error=ERROR_TIMEOUT;
    return ok;
}
inline bool QueryByte(HANDLE device,const std::array<BYTE,8>& header,BYTE& value,DWORD& error) {
    ParameterReport report{};std::copy(header.begin(),header.end(),report.begin());
    const bool connection=header[2]==7;
    const char* sendStage=connection?"connection-send":"battery-send";
    const char* readStage=connection?"connection-read":"battery-read";
    const ULONGLONG started=GetTickCount64();
    if(!HidD_SetFeature(device,report.data(),static_cast<ULONG>(report.size()))){
        error=GetLastError();RecordConfigurationRead(sendStage,static_cast<DWORD>(GetTickCount64()-started),error);return false;
    }
    RecordConfigurationRead(sendStage,static_cast<DWORD>(GetTickCount64()-started),0);
    if(WaitForSingleObject(stopEvent,10)==WAIT_OBJECT_0){error=ERROR_OPERATION_ABORTED;return false;}
    DWORD received=0;const bool read=ReadMatchingFeature(device,report,received,error,[&](const auto& reply,DWORD length){
        return MouseState::QueryReply(reply.data(),length,header.data());
    });
    RecordConfigurationRead(readStage,received,error);if(!read)return false;
    value=report[8];return true;
}
inline bool ReadDeviceIdentifier(HANDLE device,bool& online,DWORD& error,std::wstring& message) {
    online=false;ParameterReport report{};std::copy(IdentifierQuery.begin(),IdentifierQuery.end(),report.begin());
    const ULONGLONG started=GetTickCount64();
    if(!HidD_SetFeature(device,report.data(),static_cast<ULONG>(report.size()))){
        error=GetLastError();RecordConfigurationRead("identifier-send",static_cast<DWORD>(GetTickCount64()-started),error);
        message=Localization::Get(Localization::T297);return false;
    }
    RecordConfigurationRead("identifier-send",static_cast<DWORD>(GetTickCount64()-started),0);
    if(WaitForSingleObject(stopEvent,10)==WAIT_OBJECT_0){error=ERROR_OPERATION_ABORTED;return false;}
    DWORD received=0;
    const bool read=ReadMatchingFeature(device,report,received,error,[](const auto& reply,DWORD length){
        return ValidateIdentifierReply(reply,length);
    });
    RecordConfigurationRead("identifier-read",received,error);
    if(!read){message=Localization::Get(Localization::T298);return false;}
    online=HasDeviceIdentifier(report);error=0;return true;
}
inline bool ReadParameters(HANDLE device,ParameterReport& report,Settings& settings,DWORD& error,std::wstring& message) {
    constexpr std::array<BYTE,8> header={9,0,0x44,0,1,0,0x90,1};
    report.fill(0);std::copy(header.begin(),header.end(),report.begin());
    const ULONGLONG started=GetTickCount64();
    if(!HidD_SetFeature(device,report.data(),static_cast<ULONG>(report.size()))){
        error=GetLastError();RecordConfigurationRead("parameter-send",static_cast<DWORD>(GetTickCount64()-started),error);
        message=Localization::Get(Localization::T299);return false;
    }
    RecordConfigurationRead("parameter-send",static_cast<DWORD>(GetTickCount64()-started),0);
    if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0){error=ERROR_OPERATION_ABORTED;return false;}
    DWORD received=0;
    const bool read=ReadMatchingFeature(device,report,received,error,[](const auto& reply,DWORD length){
        return ValidateParameterReply(reply,length);
    });
    RecordConfigurationRead("parameter-read",received,error);
    if(!read){message=error==ERROR_TIMEOUT?Localization::Get(Localization::T300):Localization::Get(Localization::T301);return false;}
    if(!ParseParameters(report,received,settings,message)){error=ERROR_INVALID_DATA;RecordConfigurationRead("parameter-parse",received,error);return false;}
    error=0;return true;
}
inline void ClearOperation(Snapshot& snapshot) {
    snapshot.operationFinished=false;snapshot.applied=false;snapshot.parameterReadFailed=false;snapshot.error=0;snapshot.message.clear();
}

inline bool ReadMappings(HANDLE device,ParameterReport& report,ButtonMappings& buttons,DWORD& error,std::wstring& message) {
    report.fill(0);std::copy(MappingQuery.begin(),MappingQuery.end(),report.begin());
    const auto started=GetTickCount64();
    if(!HidD_SetFeature(device,report.data(),static_cast<ULONG>(report.size()))){
        error=GetLastError();RecordConfigurationRead("mapping-send",static_cast<DWORD>(GetTickCount64()-started),error);
        message=Localization::Get(Localization::T302);return false;
    }
    RecordConfigurationRead("mapping-send",static_cast<DWORD>(GetTickCount64()-started),0);
    if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0){error=ERROR_OPERATION_ABORTED;return false;}
    DWORD received=0;
    const bool read=ReadMatchingFeature(device,report,received,error,[](const auto& reply,DWORD length){return ValidateMappingReply(reply,length);});
    RecordConfigurationRead("mapping-read",received,error);
    if(!read){message=Localization::Get(Localization::T303);return false;}
    if(!ParseMappings(report,received,buttons,message)){error=ERROR_INVALID_DATA;return false;}
    error=0;return true;
}
inline bool ReadCalibrationMacros(HANDLE device,MacroStorage& storage,DWORD& error){
    for(std::uint8_t page=0;page<8;++page){ParameterReport query{};const std::array<BYTE,8> header={9,0,0x43,0,1,page,0,2};std::copy(header.begin(),header.end(),query.begin());DWORD length=0;
        if(!HidD_SetFeature(device,query.data(),static_cast<ULONG>(query.size()))){error=GetLastError();return false;}
        if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0){error=ERROR_OPERATION_ABORTED;return false;}
        if(!ReadMatchingFeature(device,query,length,error,[&](auto const& response,DWORD count){return ValidateMacroPage(response,count,page);}))return false;
        std::copy_n(query.begin()+8,512,storage.begin()+page*512);
    }return true;
}
inline bool WriteCalibrationMacros(HANDLE device,MacroStorage const& storage,DWORD& error){
    for(std::uint8_t page=0;page<8;++page){ParameterReport report{};report[0]=9;report[2]=3;report[4]=8;report[5]=page;report[7]=2;std::copy_n(storage.begin()+page*512,512,report.begin()+8);report[1]=ParameterChecksum(report);
        if(!HidD_SetFeature(device,report.data(),static_cast<ULONG>(report.size()))){error=GetLastError();return false;}
        if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0){error=ERROR_OPERATION_ABORTED;return false;}
    }
    MacroStorage readback{};if(!ReadCalibrationMacros(device,readback,error))return false;if(storage!=readback){error=ERROR_INVALID_DATA;return false;}return true;
}
inline bool WriteCalibrationMapping(HANDLE device,ParameterReport const& report,DWORD& error){
    if(!HidD_SetFeature(device,const_cast<std::uint8_t*>(report.data()),static_cast<ULONG>(report.size()))){error=GetLastError();return false;}
    if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0){error=ERROR_OPERATION_ABORTED;return false;}
    ParameterReport reply{};ButtonMappings buttons{};std::wstring message;if(!ReadMappings(device,reply,buttons,error,message))return false;
    if(!EqualMappingPayload(report,reply)){error=ERROR_INVALID_DATA;return false;}return true;
}
inline bool RunClockCalibration(HANDLE device,std::wstring const& devicePath,std::wstring const& recovery,ApplyMode mode,DWORD& error,std::wstring& message){
    ClockCalibration::Backup backup;
    const bool pending=GetFileAttributesW(recovery.c_str())!=INVALID_FILE_ATTRIBUTES;
    if(pending){if(!ClockCalibration::LoadBackup(recovery,backup)){error=ERROR_INVALID_DATA;message=Localization::Get(Localization::T304);return false;}
        if(backup.devicePath!=devicePath){error=ERROR_DEVICE_NOT_CONNECTED;message=Localization::Get(Localization::T305);return false;}}
    else if(mode==ApplyMode::RestoreClockCalibration){error=0;return true;}
    if(mode==ApplyMode::PrepareClockCalibration){
        if(!pending){backup.devicePath=devicePath;ButtonMappings buttons{};if(!ReadMappings(device,backup.mappings,buttons,error,message)||!ReadCalibrationMacros(device,backup.macros,error))return false;}
        MacroStorage prepared{};ParameterReport mappings{};
        if(!ClockCalibration::BuildPreparation(backup,prepared,mappings,message)){error=ERROR_INVALID_DATA;return false;}
        if(!pending&&!ClockCalibration::SaveBackup(recovery,backup)){error=GetLastError();if(!error)error=ERROR_WRITE_FAULT;message=Localization::Get(Localization::T306);return false;}
        return WriteCalibrationMacros(device,prepared,error)&&WriteCalibrationMapping(device,mappings,error);
    }
    ParameterReport fresh{},write{};ButtonMappings current{},desired{},expected{};
    if(!ReadMappings(device,fresh,current,error,message))return false;desired=current;std::copy_n(backup.mappings.begin()+8+5*4,4,desired[5].begin());
    if(!BuildMappingWrite(fresh,desired,current,write,expected,message)){error=ERROR_INVALID_DATA;return false;}
    if(!WriteCalibrationMacros(device,backup.macros,error)||!WriteCalibrationMapping(device,write,error))return false;
    if(!DeleteFileW(recovery.c_str())){error=GetLastError();return false;}error=0;return true;
}
inline std::vector<MacroConfiguration> ReadMacroConfigurations(HANDLE device,const ButtonMappings& buttons,const std::vector<MacroConfiguration>& known={}) {
    auto configurations=known;
    for(const auto& code:buttons)if(code[0]==3&&std::none_of(configurations.begin(),configurations.end(),
        [&](const auto& configuration){return configuration.binding==code;}))configurations.push_back({UnknownButtonName(code),code});
    if(configurations.empty())return configurations;
    MacroStorage storage{};bool readable=true;
    for(std::uint8_t page=0;page<8;++page){
        ParameterReport report{};const std::array<BYTE,8> header={9,0,0x43,0,1,page,0,2};
        std::copy(header.begin(),header.end(),report.begin());DWORD error=0,received=0;
        if(!HidD_SetFeature(device,report.data(),static_cast<ULONG>(report.size()))){readable=false;break;}
        if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0){readable=false;break;}
        if(!ReadMatchingFeature(device,report,received,error,[&](const auto& reply,DWORD length){return ValidateMacroPage(reply,length,page);})){readable=false;break;}
        std::copy_n(report.begin()+8,512,storage.begin()+page*512);
    }
    if(readable){
    configurations.erase(std::remove_if(configurations.begin(),configurations.end(),[&](const auto& configuration){
        std::string name;
        return !MacroNameBytes(storage,configuration.binding[3],name)&&
            std::find(buttons.begin(),buttons.end(),configuration.binding)==buttons.end();
    }),configurations.end());
    for(auto& configuration:configurations){
        std::string name;
        if(!MacroNameBytes(storage,configuration.binding[3],name)||name.empty())continue;
        const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),nullptr,0);
        if(count<=0)continue;
        std::wstring decoded(static_cast<std::size_t>(count),L'\0');
        if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),decoded.data(),count)==count)
            {
            configuration.name=std::move(decoded);const auto offset=4*configuration.binding[3];
            const auto start=static_cast<std::size_t>(storage[offset])+(static_cast<std::size_t>(storage[offset+1])<<8);
            const auto length=static_cast<std::size_t>(storage[offset+2])+(static_cast<std::size_t>(storage[offset+3])<<8);
            configuration.data.assign(storage.begin()+start,storage.begin()+start+length);
        }
    }
    }
    return configurations;
}

inline DWORD RunSession() {
    recorder.Reset(directory,initial.status);
    Snapshot snapshot=initial;
    const auto root=std::filesystem::path(directory).parent_path().wstring();const auto settingsPath=LogStorage::Path(root,L"settings-0.0.7-alpha.ini");const auto recoveryPath=ClockCalibration::RecoveryPath(root);
    const auto clock=ClockCompensation::Read(settingsPath);snapshot.settings.clockCompensationEnabled=clock.enabled;snapshot.settings.clockCompensationFactor=clock.factor;
    snapshot.calibrationRecoveryPending=GetFileAttributesW(recoveryPath.c_str())!=INVALID_FILE_ATTRIBUTES;
    Device::Handle device,inputEvent(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!inputEvent.value){snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=GetLastError();Publish(snapshot,true);return 1;}
    OVERLAPPED input{};input.hEvent=inputEvent.value;
    std::array<BYTE,8> inputBytes{};
    bool pending=false,wired=false,identityReady=false,identifierAttempted=false,mouseOnline=false;
    MouseState::BatteryReportWait batteryWait;
    bool onlineRefreshRequested=false,queryBatteryAfterConfigurationRetry=true;
    std::wstring activePath;
    ULONGLONG nextPoll=GetTickCount64()+PollInterval;
    ULONGLONG nextConfigurationRetry=MAXULONGLONG;
    unsigned configurationRetryCount=0;
    auto cancelInput=[&](){
        if(pending){
            DWORD count=0;CancelIoEx(device.value,&input);
            const BOOL completed=GetOverlappedResult(device.value,&input,&count,TRUE);pending=false;
            if(completed && !wired){
                const auto event=MouseState::ParseInput(inputBytes.data(),count);
                Device::RecordInput(event,inputBytes[3],count);
                if(event==MouseState::Event::Offline){
                    batteryWait.Observe(event,GetTickCount64());onlineRefreshRequested=false;
                    mouseOnline=false;identityReady=false;snapshot.configurationValid=false;snapshot.buttonsValid=false;
                    snapshot.status.state=MouseState::Mode::Offline;snapshot.status.error=0;nextConfigurationRetry=MAXULONGLONG;
                    recorder.Record(snapshot.status);
                }else if(event==MouseState::Event::Online && !mouseOnline){
                    batteryWait.Observe(event,GetTickCount64());onlineRefreshRequested=true;queryBatteryAfterConfigurationRetry=false;
                    mouseOnline=true;identityReady=false;snapshot.configurationValid=false;snapshot.buttonsValid=false;
                    snapshot.status.state=MouseState::Mode::AwaitingBattery;snapshot.status.error=0;recorder.Record(snapshot.status);
                }else if(event==MouseState::Event::Battery || event==MouseState::Event::Power){
                    batteryWait.Observe(event,GetTickCount64());mouseOnline=true;
                    Device::SetBattery(snapshot.status,inputBytes[3]);recorder.Record(snapshot.status);
                }
            }
        }
        ResetEvent(inputEvent.value);
    };
    struct InputScope {decltype(cancelInput)& cancel;~InputScope() noexcept {try{cancel();}catch(...){}}} inputScope{cancelInput};
    auto beginInput=[&](){
        if(device.value==INVALID_HANDLE_VALUE || pending || WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0)return;
        inputBytes.fill(0);ResetEvent(inputEvent.value);input={};input.hEvent=inputEvent.value;DWORD count=0;
        const BOOL ok=ReadFile(device.value,inputBytes.data(),static_cast<DWORD>(inputBytes.size()),&count,&input);
        if(ok||GetLastError()==ERROR_IO_PENDING){pending=true;if(ok)SetEvent(inputEvent.value);}
        else{batteryWait.Reset();snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=GetLastError();Publish(snapshot,true);device.reset();}
    };
    auto readConfiguration=[&](){
        snapshot.buttonsValid=false;
        ParameterReport report;Settings settings=snapshot.settings;DWORD error=0;std::wstring message;
        if(ReadParameters(device.value,report,settings,error,message)){
            snapshot.settings=settings;snapshot.configurationValid=true;snapshot.parameterReadFailed=false;
            snapshot.error=0;snapshot.message.clear();
            snapshot.buttonsValid=ReadMappings(device.value,report,snapshot.settings.buttons,error,message);
            if(snapshot.buttonsValid)snapshot.macros=std::make_shared<const std::vector<MacroConfiguration>>(ReadMacroConfigurations(device.value,snapshot.settings.buttons,*snapshot.macros));
            else{snapshot.error=error;snapshot.message=message;}
        }else{snapshot.parameterReadFailed=true;snapshot.error=error;snapshot.message=message;}
    };
    auto identify=[&](){
        if(wired){batteryWait.Reset();snapshot.status.state=MouseState::Mode::Wired;return true;}
        bool online=false;DWORD error=0;std::wstring message;
        if(!CheckWirelessSession(identifierAttempted,online,error,
            [&](bool& connected,DWORD& code){return ReadDeviceIdentifier(device.value,connected,code,message);},
            [&](BYTE& state,DWORD& code){
                const bool read=QueryByte(device.value,{9,9,7,0,1,0,1,0},state,code);
                if(!read)message=Localization::Get(Localization::T307);return read;
            })){
            batteryWait.Reset();snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=error;snapshot.message=message;return false;
        }
        identityReady=online;mouseOnline=online;snapshot.status.error=0;
        if(!online){batteryWait.Reset();snapshot.status.state=MouseState::Mode::Offline;snapshot.configurationValid=false;snapshot.buttonsValid=false;return false;}
        batteryWait.ConfirmOnline();
        if(batteryWait.Waiting())snapshot.status.state=MouseState::Mode::AwaitingBattery;
        else if(snapshot.status.state!=MouseState::Mode::Charging)snapshot.status.state=MouseState::Mode::Wireless;
        return true;
    };
    auto queryBattery=[&](){
        if(wired || batteryWait.Waiting())return;
        BYTE battery=0;DWORD error=0;
        if(QueryByte(device.value,{9,12,9,1,1,0,1,0},battery,error))Device::SetBattery(snapshot.status,battery);
        else{snapshot.status.error=error;snapshot.message=Localization::Get(Localization::T308);}
    };
    auto refresh=[&](bool queryOnRefresh){
        // Pause vendor Input while this handle performs Feature transactions.
        cancelInput();FeatureInputGuard<decltype(beginInput)> resumeInput{beginInput};
        // A queued online notification can be drained by cancellation above.
        if(onlineRefreshRequested){queryOnRefresh=false;onlineRefreshRequested=false;}
        queryBatteryAfterConfigurationRetry=queryOnRefresh;
        ClearOperation(snapshot);snapshot.status.error=0;
        std::wstring receiver,wirePath;DWORD error=0;
        const bool changed=topologyChanged.exchange(false);
        if(changed){device.reset();batteryWait.Reset();identityReady=false;identifierAttempted=false;}
        const bool rescan=changed||device.value==INVALID_HANDLE_VALUE||snapshot.status.state==MouseState::Mode::Error;
        if(!rescan){if(wired)wirePath=activePath;else receiver=activePath;}
        if(rescan&&!Enumerate(receiver,wirePath,error)){
            snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=error;snapshot.configurationValid=false;snapshot.buttonsValid=false;
            snapshot.message=Localization::Get(Localization::T309);cancelInput();device.reset();batteryWait.Reset();identityReady=false;Publish(snapshot,true);return;
        }
        wired=!wirePath.empty();const std::wstring path=wired?wirePath:receiver;
        if(path!=activePath){device.reset();batteryWait.Reset();identityReady=false;identifierAttempted=false;mouseOnline=false;snapshot.configurationValid=false;snapshot.buttonsValid=false;snapshot.macros=EmptyMacros;activePath=path;}
        if(path.empty()){
            batteryWait.Reset();snapshot.status.state=MouseState::Mode::Missing;snapshot.configurationValid=false;snapshot.buttonsValid=false;Publish(snapshot,true);return;
        }
        if(device.value==INVALID_HANDLE_VALUE){
            identityReady=false;
            device.reset(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
        }
        if(device.value==INVALID_HANDLE_VALUE){
            snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=GetLastError();
            batteryWait.Reset();snapshot.message=Localization::Get(Localization::T310);Publish(snapshot,true);return;
        }
        if(wired){batteryWait.Reset();snapshot.status.state=MouseState::Mode::Wired;}
        else if(!identityReady){
            if(!identify()){Publish(snapshot,true);return;}
        }else{
            BYTE connection=0;
            if(!QueryByte(device.value,{9,9,7,0,1,0,1,0},connection,error)){
                batteryWait.Reset();snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=error;snapshot.message=Localization::Get(Localization::T307);
                Publish(snapshot,true);return;
            }
            if(connection==0){batteryWait.Reset();identityReady=false;mouseOnline=false;snapshot.status.state=MouseState::Mode::Offline;snapshot.configurationValid=false;snapshot.buttonsValid=false;Publish(snapshot,true);return;}
            if(connection!=1){
                batteryWait.Reset();snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=ERROR_INVALID_DATA;
                snapshot.message=Localization::Get(Localization::T311);Publish(snapshot,true);return;
            }
            batteryWait.ConfirmOnline();mouseOnline=true;
        }
        readConfiguration();
        // Enable editing as soon as the parameters arrive, before querying battery.
        if(!snapshot.parameterReadFailed){Publish(snapshot);if(queryOnRefresh)queryBattery();}
        Publish(snapshot,true);
    };
    auto retryConfiguration=[&](){
        cancelInput();FeatureInputGuard<decltype(beginInput)> resumeInput{beginInput};
        ClearOperation(snapshot);
        if(snapshot.status.state==MouseState::Mode::Offline && !mouseOnline){Publish(snapshot,true);return;}
        if(configurationRetryCount==2){
            // Rebuild the computer-side interface once after a failed direct parameter retry.
            cancelInput();device.reset();identityReady=false;
            device.reset(CreateFileW(activePath.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
            if(device.value==INVALID_HANDLE_VALUE){batteryWait.Reset();snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=GetLastError();Publish(snapshot,true);return;}
        }
        if(!wired && !identityReady && !identify()){Publish(snapshot,true);return;}
        readConfiguration();
        if(!snapshot.parameterReadFailed){Publish(snapshot);if(queryBatteryAfterConfigurationRetry)queryBattery();}
        Publish(snapshot,true);
    };
    auto queryMissingBatteryReport=[&](){
        cancelInput();FeatureInputGuard<decltype(beginInput)> resumeInput{beginInput};
        // Drain a completed battery/charging report before consuming the timeout.
        if(!batteryWait.TakeExpired(GetTickCount64())){Publish(snapshot,true);return;}
        if(wired || !mouseOnline || device.value==INVALID_HANDLE_VALUE)return;
        ProgramLog::Write("battery_report_wait_expired","timeout_ms=2000",0,"warn");
        BYTE connection=0;DWORD error=0;
        if(!QueryByte(device.value,{9,9,7,0,1,0,1,0},connection,error)){
            snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=error;snapshot.message=Localization::Get(Localization::T307);
        }else if(connection==0){
            batteryWait.Reset();mouseOnline=false;identityReady=false;snapshot.configurationValid=false;snapshot.buttonsValid=false;
            nextConfigurationRetry=MAXULONGLONG;snapshot.status.state=MouseState::Mode::Offline;snapshot.status.error=0;
        }else if(connection!=1){
            snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=ERROR_INVALID_DATA;
            snapshot.message=Localization::Get(Localization::T311);
        }else queryBattery();
        Publish(snapshot,true);
    };
    auto apply=[&](){
        cancelInput();FeatureInputGuard<decltype(beginInput)> resumeInput{beginInput};
        Settings draft,baseline;ApplyMode mode;
        {std::lock_guard<std::mutex> lock(requestMutex);draft=std::move(requestedDraft);baseline=std::move(requestedBaseline);requestedDraft={};requestedBaseline={};mode=requestedMode;}
        ClearOperation(snapshot);snapshot.operationFinished=true;
        if(mode==ApplyMode::PrepareClockCalibration||mode==ApplyMode::RestoreClockCalibration){
            snapshot.operationFinished=false;Snapshot result=snapshot;result.calibrationOperation=mode;
            if(device.value==INVALID_HANDLE_VALUE||snapshot.status.state==MouseState::Mode::Offline||snapshot.status.state==MouseState::Mode::Missing){result.error=ERROR_DEVICE_NOT_CONNECTED;}
            else result.calibrationSucceeded=RunClockCalibration(device.value,activePath,recoveryPath,mode,result.error,result.message);
            snapshot.calibrationRecoveryPending=GetFileAttributesW(recoveryPath.c_str())!=INVALID_FILE_ATTRIBUTES;result.calibrationRecoveryPending=snapshot.calibrationRecoveryPending;Publish(result);if(result.calibrationSucceeded&&mode==ApplyMode::RestoreClockCalibration)SetEvent(refreshEvent);return;
        }
        if(snapshot.calibrationRecoveryPending){snapshot.error=ERROR_BUSY;snapshot.message=Localization::Get(Localization::T312);Publish(snapshot);return;}
        if(!Validate(draft,snapshot.message)){snapshot.error=ERROR_INVALID_DATA;Publish(snapshot);return;}
        if(device.value==INVALID_HANDLE_VALUE||snapshot.status.state==MouseState::Mode::Offline||snapshot.status.state==MouseState::Mode::Missing){
            snapshot.error=ERROR_DEVICE_NOT_CONNECTED;snapshot.message=Localization::Get(Localization::T313);Publish(snapshot);return;
        }
        // Input resumes after this serial Feature transaction completes.
        ParameterReport fresh,write,reply,mappingFresh,mappingWrite;Settings freshSettings=snapshot.settings,expected,verified;
        ButtonMappings expectedButtons{};DWORD error=0;std::wstring message;
        if(!ReadParameters(device.value,fresh,freshSettings,error,message)){
            snapshot.error=error;snapshot.message=Localization::Format(Localization::T314,{{L"details",message}});Publish(snapshot);return;
        }
        const bool resettingButtons=mode==ApplyMode::ResetButtonMappings;
        if(!BuildParameterWrite(fresh,resettingButtons?freshSettings:draft,resettingButtons?freshSettings:baseline,write,expected,message)){
            snapshot.error=ERROR_INVALID_DATA;snapshot.message=message;Publish(snapshot);return;
        }
        const bool clockEdited=!resettingButtons&&(draft.clockCompensationEnabled!=baseline.clockCompensationEnabled||draft.clockCompensationFactor!=baseline.clockCompensationFactor);
        const bool macrosEdited=!resettingButtons&&(clockEdited||std::any_of(draft.macroAssignments.begin(),draft.macroAssignments.end(),[](const auto& assignment){return assignment.has_value();}));
        const bool buttonsEdited=resettingButtons||macrosEdited||draft.buttons!=baseline.buttons;
        snapshot.buttonsValid=ReadMappings(device.value,mappingFresh,freshSettings.buttons,error,message);
        snapshot.settings=freshSettings;snapshot.configurationValid=true;
        if(!snapshot.buttonsValid&&buttonsEdited){snapshot.error=error;snapshot.message=Localization::Format(Localization::T314,{{L"details",message}});Publish(snapshot);return;}
        auto resolvedButtons=draft.buttons;auto mappingBaseline=baseline.buttons;
        MacroStorage macroFresh{},macroWrite{};
        if(macrosEdited){
            for(std::uint8_t page=0;page<8;++page){ParameterReport query{};const std::array<BYTE,8> header={9,0,0x43,0,1,page,0,2};
                std::copy(header.begin(),header.end(),query.begin());DWORD length=0;
                if(!HidD_SetFeature(device.value,query.data(),static_cast<ULONG>(query.size()))){snapshot.error=GetLastError();snapshot.message=Localization::Get(Localization::T315);Publish(snapshot);return;}
                if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0)return;
                if(!ReadMatchingFeature(device.value,query,length,error,[&](const auto& response,DWORD count){return ValidateMacroPage(response,count,page);})){snapshot.error=error;snapshot.message=Localization::Get(Localization::T315);Publish(snapshot);return;}
                std::copy_n(query.begin()+8,512,macroFresh.begin()+page*512);
            }
            if(!BuildMacroStorage(macroFresh,mappingFresh,draft,macroWrite,resolvedButtons,message)){snapshot.error=ERROR_INVALID_DATA;snapshot.message=message;Publish(snapshot);return;}
            for(std::size_t index=0;index<6;++index)if(draft.macroAssignments[index])mappingBaseline[index]={0xff,0xff,0xff,0xff};
        }
        if(buttonsEdited&&!BuildMappingWrite(mappingFresh,resolvedButtons,mappingBaseline,mappingWrite,expectedButtons,message,mode)){
            snapshot.error=ERROR_INVALID_DATA;snapshot.message=message;Publish(snapshot);return;
        }
        if(macrosEdited&&macroWrite!=macroFresh){
            for(std::uint8_t page=0;page<8;++page){ParameterReport report{};report[0]=9;report[2]=3;report[4]=8;report[5]=page;report[7]=2;
                std::copy_n(macroWrite.begin()+page*512,512,report.begin()+8);report[1]=ParameterChecksum(report);
                if(!HidD_SetFeature(device.value,report.data(),static_cast<ULONG>(report.size()))){snapshot.error=GetLastError();snapshot.message=Localization::Get(Localization::T316);Publish(snapshot);return;}
                if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0)return;
            }
            for(std::uint8_t page=0;page<8;++page){ParameterReport query{};const std::array<BYTE,8> header={9,0,0x43,0,1,page,0,2};std::copy(header.begin(),header.end(),query.begin());DWORD length=0;
                if(!HidD_SetFeature(device.value,query.data(),static_cast<ULONG>(query.size()))){snapshot.error=GetLastError();snapshot.message=Localization::Get(Localization::T317);Publish(snapshot);return;}
                if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0)return;
                if(!ReadMatchingFeature(device.value,query,length,error,[&](const auto& response,DWORD count){return ValidateMacroPage(response,count,page);})||
                    !std::equal(macroWrite.begin()+page*512,macroWrite.begin()+(page+1)*512,query.begin()+8)){
                    snapshot.error=error?error:ERROR_INVALID_DATA;snapshot.message=Localization::Get(Localization::T318);
                    snapshot.macros=std::make_shared<const std::vector<MacroConfiguration>>(ReadMacroConfigurations(device.value,snapshot.settings.buttons,*snapshot.macros));Publish(snapshot);return;
                }
            }
        }
        if(!EqualParameterPayload(write,fresh)){
            const BOOL sent=HidD_SetFeature(device.value,write.data(),static_cast<ULONG>(write.size()));
            const DWORD sendError=sent?0:GetLastError();
            if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0)return;
            verified=snapshot.settings;
            if(!ReadParameters(device.value,reply,verified,error,message)){
                snapshot.configurationValid=false;snapshot.buttonsValid=false;snapshot.error=error;snapshot.message=Localization::Format(Localization::T319,{{L"details",message}});Publish(snapshot);return;
            }
            snapshot.settings=verified;
            if(!EqualParameterPayload(write,reply)){
                snapshot.error=sendError?sendError:ERROR_INVALID_DATA;
                snapshot.message=Localization::Get(Localization::T320);Publish(snapshot);return;
            }
        }
        if(buttonsEdited&&!EqualMappingPayload(mappingWrite,mappingFresh)){
            const BOOL sent=HidD_SetFeature(device.value,mappingWrite.data(),static_cast<ULONG>(mappingWrite.size()));
            const DWORD sendError=sent?0:GetLastError();
            if(WaitForSingleObject(stopEvent,100)==WAIT_OBJECT_0)return;
            snapshot.buttonsValid=ReadMappings(device.value,reply,snapshot.settings.buttons,error,message);
            if(!snapshot.buttonsValid){snapshot.error=error;snapshot.message=Localization::Format(Localization::T321,{{L"details",message}});Publish(snapshot);return;}
            if(!EqualMappingPayload(mappingWrite,reply)){
                snapshot.error=sendError?sendError:ERROR_INVALID_DATA;
                snapshot.message=Localization::Get(Localization::T322);Publish(snapshot);return;
            }
        }
        if(snapshot.buttonsValid)snapshot.macros=std::make_shared<const std::vector<MacroConfiguration>>(ReadMacroConfigurations(device.value,snapshot.settings.buttons,*snapshot.macros));
        if(!resettingButtons){snapshot.settings.clockCompensationEnabled=draft.clockCompensationEnabled;snapshot.settings.clockCompensationFactor=draft.clockCompensationFactor;
            if(!ClockCompensation::Save(settingsPath,{draft.clockCompensationEnabled,draft.clockCompensationFactor})){snapshot.error=GetLastError();snapshot.message=Localization::Get(Localization::T323);Publish(snapshot);return;}}
        snapshot.applied=true;snapshot.error=0;snapshot.message=Localization::Get(Localization::T324);
        Publish(snapshot);
    };
    SetEvent(refreshEvent);
    for(;;){
        HANDLE waits[]={stopEvent,refreshEvent,applyEvent,inputEvent.value};
        const ULONGLONG now=GetTickCount64(),next=std::min({nextPoll,nextConfigurationRetry,batteryWait.Deadline()});
        const DWORD timeout=now>=next?0:static_cast<DWORD>(next-now);
        const DWORD wait=WaitForMultipleObjects(pending?4:3,waits,FALSE,timeout);
        if(wait==WAIT_OBJECT_0||wait==WAIT_FAILED)break;
        if(wait==WAIT_TIMEOUT && batteryWait.Due(GetTickCount64())){queryMissingBatteryReport();continue;}
        if(wait==WAIT_TIMEOUT||wait==WAIT_OBJECT_0+1){
            if(wait==WAIT_OBJECT_0+1||GetTickCount64()>=nextPoll)configurationRetryCount=0;
            if(wait==WAIT_TIMEOUT && GetTickCount64()<nextPoll && device.value!=INVALID_HANDLE_VALUE)retryConfiguration();
            else{refresh(!onlineRefreshRequested);nextPoll=GetTickCount64()+PollInterval;}
            const bool retry=(!snapshot.configurationValid||!snapshot.buttonsValid)&&device.value!=INVALID_HANDLE_VALUE&&
                snapshot.status.state!=MouseState::Mode::Offline&&snapshot.status.state!=MouseState::Mode::Missing;
            nextConfigurationRetry=retry&&configurationRetryCount<2?GetTickCount64()+1000:MAXULONGLONG;
            if(retry)++configurationRetryCount;
        }
        else if(wait==WAIT_OBJECT_0+2)apply();
        else if(wait==WAIT_OBJECT_0+3){
            DWORD count=0;const BOOL ok=GetOverlappedResult(device.value,&input,&count,FALSE);pending=false;ClearOperation(snapshot);
            if(!ok){batteryWait.Reset();snapshot.status.state=MouseState::Mode::Error;snapshot.status.error=GetLastError();Publish(snapshot,true);device.reset();continue;}
            const auto event=MouseState::ParseInput(inputBytes.data(),count);
            Device::RecordInput(event,inputBytes[3],count);
            if(!wired&&event==MouseState::Event::Online){
                if(!mouseOnline){
                    batteryWait.Observe(event,GetTickCount64());onlineRefreshRequested=true;queryBatteryAfterConfigurationRetry=false;
                    mouseOnline=true;identityReady=false;snapshot.configurationValid=false;snapshot.buttonsValid=false;
                    snapshot.status.state=MouseState::Mode::AwaitingBattery;snapshot.status.error=0;Publish(snapshot,true);
                    SetEvent(refreshEvent);
                }
            }else if(!wired&&event==MouseState::Event::Offline){
                batteryWait.Observe(event,GetTickCount64());onlineRefreshRequested=false;
                mouseOnline=false;identityReady=false;nextConfigurationRetry=MAXULONGLONG;
                snapshot.configurationValid=false;snapshot.buttonsValid=false;
                snapshot.status.state=MouseState::Mode::Offline;snapshot.status.error=0;Publish(snapshot,true);
            }else if(!wired&&(event==MouseState::Event::Battery||event==MouseState::Event::Power)){
                batteryWait.Observe(event,GetTickCount64());mouseOnline=true;
                Device::SetBattery(snapshot.status,inputBytes[3]);Publish(snapshot,true);
            }else if(count==8&&inputBytes[0]==9&&inputBytes[1]==10&&inputBytes[2]==3&&inputBytes[3]>=1&&
                     inputBytes[3]<=snapshot.settings.stageCount&&inputBytes[4]==0&&inputBytes[5]==0&&inputBytes[6]==0&&inputBytes[7]==0){
                snapshot.settings.currentStage=inputBytes[3];Publish(snapshot);
            }
            beginInput();
        }
    }
    return 0;
}
inline DWORD WINAPI Run(void*) noexcept {
    try{return RunSession();}
    catch(...){
        try{Snapshot failed;failed.status.state=MouseState::Mode::Error;failed.status.error=ERROR_UNHANDLED_EXCEPTION;failed.error=ERROR_UNHANDLED_EXCEPTION;failed.operationFinished=true;failed.message=Localization::Get(Localization::T325);Publish(failed);}
        catch(...){}
        return ERROR_UNHANDLED_EXCEPTION;
    }
}
}

inline bool Start(HWND target,UINT resultMessage,const std::wstring& directory,const Device::Result& initial) {
    if(Detail::worker)return false;
    Detail::target=target;Detail::resultMessage=resultMessage;Detail::directory=directory;
    Detail::initial={};Detail::initial.status=initial;
    Detail::stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Detail::refreshEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Detail::applyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(Detail::stopEvent&&Detail::refreshEvent&&Detail::applyEvent)
        Detail::worker=CreateThread(nullptr,0,Detail::Run,nullptr,0,nullptr);
    if(Detail::worker)return true;
    const DWORD error=GetLastError();
    for(auto* handle:{&Detail::stopEvent,&Detail::refreshEvent,&Detail::applyEvent}){
        if(*handle)CloseHandle(*handle);*handle=nullptr;
    }
    SetLastError(error);return false;
}
inline void Refresh(bool topologyChanged=false){if(topologyChanged)Detail::topologyChanged=true;if(Detail::refreshEvent)SetEvent(Detail::refreshEvent);}
inline void Apply(const Settings& draft,const Settings& baseline,ApplyMode mode=ApplyMode::EditedSettings) {
    if(!Detail::applyEvent)return;
    {std::lock_guard<std::mutex> lock(Detail::requestMutex);Detail::requestedDraft=draft;Detail::requestedBaseline=baseline;Detail::requestedMode=mode;}
    SetEvent(Detail::applyEvent);
}
inline void Stop() {
    if(Detail::stopEvent)SetEvent(Detail::stopEvent);
    if(Detail::worker){CancelSynchronousIo(Detail::worker);WaitForSingleObject(Detail::worker,INFINITE);CloseHandle(Detail::worker);Detail::worker=nullptr;}
    for(auto* handle:{&Detail::stopEvent,&Detail::refreshEvent,&Detail::applyEvent}){
        if(*handle)CloseHandle(*handle);*handle=nullptr;
    }
}
}
