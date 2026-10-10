#pragma once
#include <atomic>
#include <new>
#include "alpha_state.h"
#include "history.h"
#include "program_log.h"
#include "native_handle.h"
#include <dbt.h>

namespace Device {
using MouseState::Mode;
struct Result {
    Mode state=Mode::Unknown;
    int rawPercent=-1;
    SYSTEMTIME lastRead{};
    ULONGLONG sampledAtUtc=0;
    DWORD error=0;
    bool recorded=true;
    unsigned generation=0;
};
using Handle=NativeHandle;
inline HANDLE stop=nullptr,refresh=nullptr;
inline HWND target=nullptr;
inline UINT message=0;
inline std::atomic<unsigned> generation{0};
inline std::wstring logDirectory;
inline Result initial;
class Recorder {
    BatteryHistory::Writer history;
    ULONGLONG lastSample=0;
    Mode previous=Mode::Unknown;
    int previousValue=-1;
    DWORD previousError=0;
    bool observed=false;
public:
    void Reset(const std::wstring& path,const Result& cached){history.Reset(path);lastSample=cached.sampledAtUtc;observed=false;}
    bool Record(const Result& result){
        const char* names[]={"unknown","wireless","offline","wired","missing","charging","error","awaiting_battery"};
        const char* state=names[static_cast<int>(result.state)];
        const int value=result.state==Mode::Wireless?result.rawPercent:-1;
        if(!observed||previous!=result.state||previousValue!=value||previousError!=result.error){
            if(ProgramLog::Enabled())ProgramLog::Write("device_status_changed",std::string("status=")+state+";value="+(value<0?"--":std::to_string(value)),result.error);
            previous=result.state;previousValue=value;previousError=result.error;observed=true;
        }
        // Initialization is internal; it does not describe an observed mouse state.
        if(result.state==Mode::Unknown)return true;
        // Cached display values never establish a history baseline.
        if(result.state==Mode::Wireless){
            if(result.error||!result.sampledAtUtc||result.sampledAtUtc==lastSample)return true;
            const bool ok=history.Append(state,value);if(ok)lastSample=result.sampledAtUtc;return ok;
        }
        return history.Append(state,-1);
    }
};
inline Recorder recorder;
inline void RecordInput(MouseState::Event event,BYTE value,DWORD count){
    const char* name=event==MouseState::Event::Online?"mouse_online":event==MouseState::Event::Offline?"mouse_offline":event==MouseState::Event::Battery?"battery_report_received":event==MouseState::Event::Power?"power_report_received":nullptr;
    if(name&&ProgramLog::Enabled())ProgramLog::Write(name,"bytes="+std::to_string(count)+";value="+std::to_string(value));
}

inline bool IsTargetDevicePath(std::wstring path){
    CharLowerBuffW(path.data(),static_cast<DWORD>(path.size()));
    return path.find(L"vid_258a&pid_0138")!=std::wstring::npos||path.find(L"vid_258a&pid_0136")!=std::wstring::npos;
}
inline bool IsTargetDeviceChange(WPARAM event,LPARAM data){
    if((event!=DBT_DEVICEARRIVAL&&event!=DBT_DEVICEREMOVECOMPLETE)||!data)return false;
    const auto header=reinterpret_cast<DEV_BROADCAST_HDR const*>(data);
    if(header->dbch_devicetype!=DBT_DEVTYP_DEVICEINTERFACE)return false;
    return IsTargetDevicePath(reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W const*>(data)->dbcc_name);
}
inline bool Enumerate(std::wstring& receiver,std::wstring& wired,DWORD& error){
    GUID guid;HidD_GetHidGuid(&guid);
    HDEVINFO list=SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(list==INVALID_HANDLE_VALUE){error=GetLastError();return false;}
    struct Guard{HDEVINFO list;~Guard(){SetupDiDestroyDeviceInfoList(list);}} guard{list};
    std::unique_ptr<BYTE[]> buffer;DWORD capacity=0;
    for(DWORD i=0;;++i){
        SP_DEVICE_INTERFACE_DATA item{sizeof(item)};
        if(!SetupDiEnumDeviceInterfaces(list,nullptr,&guid,i,&item)){
            error=GetLastError();if(error==ERROR_NO_MORE_ITEMS){error=0;return true;}return false;
        }
        DWORD size=0;SetupDiGetDeviceInterfaceDetailW(list,&item,nullptr,0,&size,nullptr);if(!size)continue;
        if(size>capacity){buffer=std::make_unique<BYTE[]>(size);capacity=size;}
        auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.get());detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailW(list,&item,detail,size,nullptr,nullptr))continue;
        if(!IsTargetDevicePath(detail->DevicePath))continue;
        Handle probe(CreateFileW(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
        if(probe.value==INVALID_HANDLE_VALUE){error=GetLastError();return false;}
        HIDD_ATTRIBUTES attributes{sizeof(attributes)};HIDP_CAPS caps{};PHIDP_PREPARSED_DATA data=nullptr;
        if(!HidD_GetAttributes(probe.value,&attributes)||!HidD_GetPreparsedData(probe.value,&data))continue;
        auto code=HidP_GetCaps(data,&caps);HidD_FreePreparsedData(data);
        if(code!=HIDP_STATUS_SUCCESS||attributes.VendorID!=0x258a||caps.UsagePage!=0xff02||caps.Usage!=2||caps.FeatureReportByteLength!=520||caps.InputReportByteLength!=8)continue;
        if(attributes.ProductID==0x0136&&wired.empty())wired=detail->DevicePath;
        if(attributes.ProductID==0x0138&&receiver.empty())receiver=detail->DevicePath;
    }
}
inline bool Query(HANDLE device,const std::array<BYTE,8>& header,BYTE& value,DWORD& error){
    const auto started=GetTickCount64();
    struct QueryLog {
        const std::array<BYTE,8>& header;DWORD& error;ULONGLONG started;
        ~QueryLog(){if(ProgramLog::Enabled())ProgramLog::Write(header[1]==12?"battery_query_completed":"connection_query_completed","duration_ms="+std::to_string(GetTickCount64()-started),error);}
    } queryLog{header,error,started};
    std::array<BYTE,520> bytes{};std::copy(header.begin(),header.end(),bytes.begin());
    if(!HidD_SetFeature(device,bytes.data(),static_cast<ULONG>(bytes.size()))){error=GetLastError();return false;}
    if(WaitForSingleObject(stop,10)==WAIT_OBJECT_0){error=ERROR_OPERATION_ABORTED;return false;}
    bytes.fill(0);bytes[0]=9;Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!event.value){error=GetLastError();return false;}
    OVERLAPPED overlapped{};overlapped.hEvent=event.value;DWORD received=0;
    BOOL ok=DeviceIoControl(device,0x000b0192,bytes.data(),static_cast<DWORD>(bytes.size()),bytes.data(),static_cast<DWORD>(bytes.size()),&received,&overlapped);
    if(!ok){error=GetLastError();if(error!=ERROR_IO_PENDING)return false;
        HANDLE waits[]={stop,event.value};DWORD wait=WaitForMultipleObjects(2,waits,FALSE,2000);
        if(wait!=WAIT_OBJECT_0+1){const DWORD waitError=wait==WAIT_FAILED?GetLastError():0;CancelIoEx(device,&overlapped);GetOverlappedResult(device,&overlapped,&received,TRUE);error=wait==WAIT_OBJECT_0?ERROR_OPERATION_ABORTED:wait==WAIT_FAILED?waitError:ERROR_TIMEOUT;return false;}
        if(!GetOverlappedResult(device,&overlapped,&received,FALSE)){error=GetLastError();return false;}
    }
    if(!MouseState::QueryReply(bytes.data(),received,header.data())){error=ERROR_INVALID_DATA;return false;}
    value=bytes[8];error=0;return true;
}
inline void Publish(Result result){
    if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0)return;
    result.recorded=recorder.Record(result);
    auto copy=new(std::nothrow) Result(result);
    if(copy&&!PostMessageW(target,message,0,reinterpret_cast<LPARAM>(copy)))delete copy;
}
inline void SetBattery(Result& result,BYTE value){
    if(value==255){result.state=Mode::Charging;result.error=0;}
    else if(value<=100){
        result.state=Mode::Wireless;result.rawPercent=value;GetLocalTime(&result.lastRead);result.error=0;
        FILETIME time;GetSystemTimeAsFileTime(&time);ULARGE_INTEGER ticks{};ticks.LowPart=time.dwLowDateTime;ticks.HighPart=time.dwHighDateTime;result.sampledAtUtc=ticks.QuadPart;
    }
    else{result.state=Mode::Error;result.error=ERROR_INVALID_DATA;}
}
inline DWORD RunSession(){
    recorder.Reset(logDirectory,initial);
    Result result=initial;Handle device,inputEvent(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!inputEvent.value){result.state=Mode::Error;result.error=GetLastError();Publish(result);return 1;}
    OVERLAPPED input{};input.hEvent=inputEvent.value;std::array<BYTE,8> bytes{};bool pending=false;
    MouseState::BatteryReportWait batteryWait;
    unsigned connectionGeneration=generation.load();
    auto acceptInput=[&](DWORD count){
        auto event=MouseState::ParseInput(bytes.data(),count);
        RecordInput(event,bytes[3],count);
        const bool newlyOnline=batteryWait.Observe(event,GetTickCount64());
        if(event==MouseState::Event::Online && newlyOnline){result.state=Mode::AwaitingBattery;result.error=0;Publish(result);}
        else if(event==MouseState::Event::Offline){result.state=Mode::Offline;result.error=0;Publish(result);}
        else if(event==MouseState::Event::Battery||event==MouseState::Event::Power){SetBattery(result,bytes[3]);Publish(result);}
    };
    auto cancel=[&](){
        if(pending){
            DWORD count=0;CancelIoEx(device.value,&input);
            const BOOL completed=GetOverlappedResult(device.value,&input,&count,TRUE);pending=false;
            if(completed)acceptInput(count);
        }
        ResetEvent(inputEvent.value);
    };
    struct InputScope {decltype(cancel)& cancelInput;~InputScope() noexcept {try{cancelInput();}catch(...){}}} inputScope{cancel};
    auto begin=[&](){
        if(device.value==INVALID_HANDLE_VALUE)return;
        bytes.fill(0);ResetEvent(inputEvent.value);input={};input.hEvent=inputEvent.value;DWORD count=0;
        BOOL ok=ReadFile(device.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,&input);
        if(ok||GetLastError()==ERROR_IO_PENDING){pending=true;if(ok)SetEvent(inputEvent.value);}
        else{batteryWait.Reset();result.state=Mode::Error;result.error=GetLastError();Publish(result);device.reset();}
    };
    for(;;){
        const auto now=GetTickCount64();
        const DWORD timeout=batteryWait.Waiting()?static_cast<DWORD>(now>=batteryWait.Deadline()?0:batteryWait.Deadline()-now):INFINITE;
        HANDLE waits[]={stop,refresh,inputEvent.value};DWORD wait=WaitForMultipleObjects(pending?3:2,waits,FALSE,timeout);
        if(wait==WAIT_OBJECT_0||wait==WAIT_FAILED)break;
        if(wait==WAIT_OBJECT_0+1 || wait==WAIT_TIMEOUT){
            cancel();
            if(wait==WAIT_TIMEOUT && !batteryWait.TakeExpired(GetTickCount64())){begin();continue;}
            if(wait==WAIT_TIMEOUT)ProgramLog::Write("battery_report_wait_expired","timeout_ms=2000",0,"warn");
            result.generation=generation.load();result.error=0;
            if(connectionGeneration!=result.generation){device.reset();connectionGeneration=result.generation;}
            std::wstring receiver,wired;
            const bool reuse=device.value!=INVALID_HANDLE_VALUE;
            if(!reuse&&!Enumerate(receiver,wired,result.error))result.state=Mode::Error;
            else if(!wired.empty())result.state=Mode::Wired;
            else if(!reuse&&receiver.empty())result.state=Mode::Missing;
            else{
                if(!reuse)device.reset(CreateFileW(receiver.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
                BYTE connection=0,battery=0;
                if(device.value==INVALID_HANDLE_VALUE){result.state=Mode::Error;result.error=GetLastError();}
                else if(!Query(device.value,{9,9,7,0,1,0,1,0},connection,result.error))result.state=Mode::Error;
                else if(connection==0){batteryWait.Reset();result.state=Mode::Offline;}
                else if(connection!=1){result.state=Mode::Error;result.error=ERROR_INVALID_DATA;}
                else{
                    batteryWait.ConfirmOnline();
                    if(!batteryWait.Waiting()){
                        if(!Query(device.value,{9,12,9,1,1,0,1,0},battery,result.error))result.state=Mode::Error;
                        else SetBattery(result,battery);
                    }
                }
            }
            if(result.state==Mode::Wired || result.state==Mode::Missing || result.state==Mode::Error){batteryWait.Reset();device.reset();}
            Publish(result);begin();
        }else if(wait==WAIT_OBJECT_0+2){
            DWORD count=0;BOOL ok=GetOverlappedResult(device.value,&input,&count,FALSE);pending=false;
            if(!ok){batteryWait.Reset();result.state=Mode::Error;result.error=GetLastError();Publish(result);device.reset();continue;}
            acceptInput(count);
            begin();
        }
    }
    return 0;
}
inline DWORD WINAPI Run(void*) noexcept {
    try{return RunSession();}
    catch(...){
        try{Result failed;failed.state=Mode::Error;failed.error=ERROR_UNHANDLED_EXCEPTION;failed.generation=generation.load();Publish(failed);}catch(...){}
        return ERROR_UNHANDLED_EXCEPTION;
    }
}
}
