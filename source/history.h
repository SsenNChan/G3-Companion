#pragma once
#include "log_storage.h"
#include <charconv>
#include <string_view>

namespace BatteryHistory {
inline constexpr unsigned RetentionDays=10;
struct Record { ULONGLONG day=0; std::string status; int value=-1; };
inline bool Parse(std::string_view line,Record& record) {
    if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
    const auto first=line.find(','),second=first==line.npos?line.npos:line.find(',',first+1);
    if(first==line.npos||second==line.npos||line.find(',',second+1)!=line.npos)return false;
    SYSTEMTIME time{};if(!LogStorage::ParseTime(line.substr(0,first),time))return false;
    auto status=line.substr(first+1,second-first-1),value=line.substr(second+1);
    int raw=-1;
    if(status=="wireless"){
        auto parsed=std::from_chars(value.data(),value.data()+value.size(),raw);
        if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||raw<0||raw>100)return false;
    }else if((status!="offline"&&status!="wired"&&status!="charging"&&status!="awaiting_battery"&&status!="missing"&&status!="error")||value!="--")return false;
    record={LogStorage::DayNumber(time),std::string(status),raw};return true;
}
inline bool Prune(const std::wstring& directory,ULONGLONG today) {
    WIN32_FIND_DATAW entry{};HANDLE search=FindFirstFileW(LogStorage::Path(directory,L"battery-????????.csv").c_str(),&entry);
    if(search==INVALID_HANDLE_VALUE){const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND;}
    bool ok=true;
    do {
        const std::wstring name=entry.cFileName;
        if((entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||name.size()!=20||name.substr(0,8)!=L"battery-"||name.substr(16)!=L".csv")continue;
        std::string date;bool digits=true;for(size_t i=8;i<16;++i){if(name[i]<L'0'||name[i]>L'9')digits=false;date.push_back(static_cast<char>(name[i]));}
        SYSTEMTIME time{};if(!digits||!LogStorage::ParseTime(date+"-000000",time))continue;
        const auto day=LogStorage::DayNumber(time);
        if((day>today||day+RetentionDays-1<today)&&!DeleteFileW(LogStorage::Path(directory,name.c_str()).c_str()))ok=false;
    }while(FindNextFileW(search,&entry));
    if(GetLastError()!=ERROR_NO_MORE_FILES)ok=false;FindClose(search);return ok;
}
class Writer {
    std::wstring directory;
    ULONGLONG loadedDay=0;
    Record latest{};
    bool loaded=false,retentionOk=false;
public:
    void Reset(const std::wstring& path){directory=path;loadedDay=0;latest={};loaded=false;retentionOk=false;}
    bool Append(const char* status,int value,SYSTEMTIME time) {
        if(directory.empty())return true;
        if(!LogStorage::EnsureDirectory(directory))return false;
        const auto day=LogStorage::DayNumber(time);if(!day)return false;
        const auto path=LogStorage::DailyPath(directory,L"battery-",time);
        if(day!=loadedDay){loadedDay=day;loaded=false;latest={};retentionOk=false;}
        if(!retentionOk)retentionOk=Prune(directory,day);
        if(!loaded){
            const DWORD error=LogStorage::ReadLines(path,[&](std::string_view line){Record row;if(Parse(line,row)&&row.day==day)latest=std::move(row);});
            if(error)return false;loaded=true;
        }
        if(latest.day==day&&latest.status==status&&latest.value==value)return retentionOk;
        const std::string row=LogStorage::Time(time)+","+status+","+(value<0?"--":std::to_string(value))+"\r\n";
        Record parsed;if(!Parse(row.substr(0,row.size()-2),parsed))return false;
        if(!LogStorage::Append(path,"time,status,value\r\n",row))return false;
        latest=std::move(parsed);return retentionOk;
    }
    bool Append(const char* status,int value){SYSTEMTIME time{};GetLocalTime(&time);return Append(status,value,time);}
};
}
