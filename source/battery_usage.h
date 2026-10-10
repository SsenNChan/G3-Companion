#pragma once
#include "history.h"
#include "alpha_state.h"
#include <array>
#include <vector>

namespace BatteryUsage {
inline constexpr size_t DisplayDays=BatteryHistory::RetentionDays;
struct Day { SYSTEMTIME date{}; bool hasReading=false; double consumed=0; };
struct Report {std::array<Day,DisplayDays> days{};double average=0;unsigned averageDays=0;DWORD error=0;};
using Record=BatteryHistory::Record;
inline SYSTEMTIME DateFromDay(ULONGLONG day){return LogStorage::DateFromDay(day);}
inline Report Empty(SYSTEMTIME today){Report result;const auto last=LogStorage::DayNumber(today);for(size_t i=0;i<DisplayDays;++i)result.days[i].date=DateFromDay(last-(DisplayDays-1-i));return result;}
class Reader {
    struct CachedDay {
        ULONGLONG number=0,offset=0,size=0;
        DWORD volume=0,indexHigh=0,indexLow=0;
        FILETIME written{};
        Day value{};
        int lowest=-1;
        bool loaded=false;
    };
    std::array<CachedDay,DisplayDays> m_days{};
public:
    std::atomic<bool> canceled{false};
    Report Read(const std::wstring& directory,SYSTEMTIME today){
        Report result=Empty(today);const auto last=LogStorage::DayNumber(today);
        for(size_t i=0;i<DisplayDays;++i){
            if(canceled.load()){result.error=ERROR_OPERATION_ABORTED;return result;}
            const auto number=last-(DisplayDays-1-i);auto& cache=m_days[number%DisplayDays];
            if(cache.number!=number){cache={};cache.number=number;}
            const auto date=DateFromDay(number);
            NativeHandle file(CreateFileW(LogStorage::DailyPath(directory,L"battery-",date).c_str(),GENERIC_READ,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
            DWORD error=0;
            if(file.value==INVALID_HANDLE_VALUE){
                error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND){cache={};cache.number=number;error=0;}
            }else{
                BY_HANDLE_FILE_INFORMATION info{};
                if(!GetFileInformationByHandle(file.value,&info))error=GetLastError();
                else{
                    const ULONGLONG size=(static_cast<ULONGLONG>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
                    const bool sameFile=cache.loaded&&cache.volume==info.dwVolumeSerialNumber&&cache.indexHigh==info.nFileIndexHigh&&cache.indexLow==info.nFileIndexLow;
                    const bool sameTime=CompareFileTime(&cache.written,&info.ftLastWriteTime)==0;
                    if(!sameFile||size<cache.size||(size==cache.size&&!sameTime)){
                        cache.offset=0;cache.lowest=-1;cache.value={};cache.loaded=false;
                    }
                    if(!cache.loaded||size!=cache.size||!sameTime){
                        error=LogStorage::ReadLinesFrom(file.value,cache.offset,[&](std::string_view line){
                            Record row;if(!BatteryHistory::Parse(line,row)||row.day!=number)return;
                            if(row.status=="wired"||row.status=="charging"){cache.lowest=-1;return;}
                            if(row.status!="wireless")return;
                            cache.value.hasReading=true;
                            if(cache.lowest<0)cache.lowest=row.value;
                            else if(row.value<cache.lowest){cache.value.consumed+=MouseState::RelativePercent(cache.lowest)-MouseState::RelativePercent(row.value);cache.lowest=row.value;}
                        },&canceled);
                        cache.loaded=error==0;cache.size=size;cache.written=info.ftLastWriteTime;
                        cache.volume=info.dwVolumeSerialNumber;cache.indexHigh=info.nFileIndexHigh;cache.indexLow=info.nFileIndexLow;
                    }
                }
            }
            result.days[i]=cache.value;result.days[i].date=date;
            if(error&&!result.error)result.error=error;
            if(i+1<DisplayDays&&result.days[i].hasReading){result.average+=result.days[i].consumed;++result.averageDays;}
        }
        if(result.averageDays)result.average/=result.averageDays;return result;
    }
    Report Read(const std::wstring& directory){SYSTEMTIME today{};GetLocalTime(&today);return Read(directory,today);}
};
}
