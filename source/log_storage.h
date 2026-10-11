#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <string_view>
#include <cstdio>
#include <atomic>
#include "native_handle.h"

namespace LogStorage {
inline constexpr ULONGLONG TicksPerDay=864000000000ULL;
inline ULONGLONG DayNumber(SYSTEMTIME date) {
    date.wHour=date.wMinute=date.wSecond=date.wMilliseconds=0;
    FILETIME time{};if(!SystemTimeToFileTime(&date,&time))return 0;
    ULARGE_INTEGER value{};value.LowPart=time.dwLowDateTime;value.HighPart=time.dwHighDateTime;return value.QuadPart/TicksPerDay;
}
inline SYSTEMTIME DateFromDay(ULONGLONG day){ULARGE_INTEGER value{};value.QuadPart=day*TicksPerDay;FILETIME time{value.LowPart,value.HighPart};SYSTEMTIME date{};FileTimeToSystemTime(&time,&date);return date;}
inline std::wstring Path(const std::wstring& directory,const wchar_t* name){return directory+(directory.empty()||directory.back()==L'\\'||directory.back()==L'/'?L"":L"\\")+name;}
inline bool EnsureDirectory(const std::wstring& directory){return CreateDirectoryW(directory.c_str(),nullptr)!=FALSE||GetLastError()==ERROR_ALREADY_EXISTS;}
inline std::string Time(const SYSTEMTIME& time){char text[32]{};sprintf_s(text,"%04u%02u%02u-%02u%02u%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);return text;}
inline std::wstring DailyPath(const std::wstring& directory,const wchar_t* prefix,const SYSTEMTIME& time){wchar_t name[64]{};swprintf_s(name,L"%s%04u%02u%02u.csv",prefix,time.wYear,time.wMonth,time.wDay);return Path(directory,name);}
inline bool ParseTime(std::string_view text,SYSTEMTIME& time){
    if(text.size()!=15||text[8]!='-')return false;
    for(size_t i=0;i<text.size();++i)if(i!=8&&(text[i]<'0'||text[i]>'9'))return false;
    auto number=[&](size_t offset,size_t count){WORD value=0;for(size_t i=0;i<count;++i)value=static_cast<WORD>(value*10+text[offset+i]-'0');return value;};
    time={};time.wYear=number(0,4);time.wMonth=number(4,2);time.wDay=number(6,2);time.wHour=number(9,2);time.wMinute=number(11,2);time.wSecond=number(13,2);
    FILETIME file{};SYSTEMTIME normalized{};
    return SystemTimeToFileTime(&time,&file)&&FileTimeToSystemTime(&file,&normalized)&&time.wYear==normalized.wYear&&time.wMonth==normalized.wMonth&&time.wDay==normalized.wDay&&time.wHour==normalized.wHour&&time.wMinute==normalized.wMinute&&time.wSecond==normalized.wSecond;
}
template<class Accept> inline DWORD ReadLinesFrom(HANDLE file,ULONGLONG& offset,Accept accept,const std::atomic<bool>* canceled=nullptr){
    LARGE_INTEGER start{};start.QuadPart=offset;
    if(!SetFilePointerEx(file,start,nullptr,FILE_BEGIN))return GetLastError();
    char buffer[4096];std::string line;bool oversized=false;ULONGLONG position=offset;
    for(;;){
        if(canceled&&canceled->load())return ERROR_OPERATION_ABORTED;
        DWORD count=0;if(!ReadFile(file,buffer,sizeof(buffer),&count,nullptr))return GetLastError();if(!count)return 0;
        const std::string_view chunk(buffer,count);
        std::size_t begin=0;
        while(begin<chunk.size()){
            const auto end=chunk.find('\n',begin);
            const auto part=chunk.substr(begin,(end==chunk.npos?chunk.size():end)-begin);
            if(!oversized){
                if(part.size()>4096-line.size()){line.clear();oversized=true;}
                else if(end!=chunk.npos&&line.empty())accept(part);
                else{line.append(part);if(end!=chunk.npos)accept(std::string_view(line));}
            }
            if(end==chunk.npos)break;
            offset=position+end+1;line.clear();oversized=false;begin=end+1;
        }
        position+=count;
    }
}
template<class Accept> inline DWORD ReadLines(const std::wstring& path,Accept accept){
    NativeHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(file.value==INVALID_HANDLE_VALUE){const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?0:error;}
    ULONGLONG offset=0;return ReadLinesFrom(file.value,offset,accept);
}
inline bool Append(const std::wstring& path,const char* header,const std::string& row){
    NativeHandle owned(CreateFileW(path.c_str(),GENERIC_READ|FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
    HANDLE file=owned.value;
    if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};bool ok=GetFileSizeEx(file,&size)!=FALSE;std::string text;
    if(ok&&size.QuadPart==0)text=header;
    else if(ok){LARGE_INTEGER last{};last.QuadPart=size.QuadPart-1;char end=0;DWORD count=0;
        ok=SetFilePointerEx(file,last,nullptr,FILE_BEGIN)&&ReadFile(file,&end,1,&count,nullptr)&&count==1;
        // Keep a damaged final row separate from the next complete record.
        if(ok&&end!='\n')text="\r\n";
    }
    if(ok){text+=row;DWORD written=0;ok=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size();}
    return ok;
}
// Diagnostic files keep ten days, with one bounded rollover per day.
inline constexpr ULONGLONG DiagnosticFileBytes=8*1024*1024;
inline bool PruneDiagnostics(const std::wstring& directory,const wchar_t* prefix,ULONGLONG today){
    WIN32_FIND_DATAW entry{};const std::wstring stem=prefix;
    HANDLE search=FindFirstFileW(Path(directory,(stem+L"*.csv").c_str()).c_str(),&entry);
    if(search==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    bool ok=true;
    do{
        if(entry.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))continue;
        const std::wstring name=entry.cFileName;
        if(name.size()!=stem.size()+12&&name.size()!=stem.size()+14)continue;
        const auto suffix=name.substr(stem.size()+8);
        if(suffix!=L".csv"&&suffix!=L".1.csv")continue;
        std::string date;for(size_t i=stem.size();i<stem.size()+8;++i)date+=static_cast<char>(name[i]);
        SYSTEMTIME parsed{};if(!ParseTime(date+"-000000",parsed))continue;
        const auto day=DayNumber(parsed);
        if(day<today&&today-day>=10&&!DeleteFileW(Path(directory,name.c_str()).c_str()))ok=false;
    }while(FindNextFileW(search,&entry));
    const auto error=GetLastError();FindClose(search);return ok&&error==ERROR_NO_MORE_FILES;
}
inline bool AppendDiagnostic(const std::wstring& path,const char* header,const std::string& rows){
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)){
        const ULONGLONG size=(static_cast<ULONGLONG>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
        if(size&&size+rows.size()>DiagnosticFileBytes){
            const auto previous=path.substr(0,path.size()-4)+L".1.csv";
            if(!MoveFileExW(path.c_str(),previous.c_str(),MOVEFILE_REPLACE_EXISTING))return false;
        }
    }else if(GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
    return Append(path,header,rows);
}
inline std::string Csv(std::string_view value){
    if(value.empty())return "--";
    if(value.find_first_of(",\"\r\n")==value.npos)return std::string(value);
    std::string quoted="\"";for(char c:value){if(c=='\"')quoted+='\"';quoted+=c;}quoted+='\"';return quoted;
}
}
