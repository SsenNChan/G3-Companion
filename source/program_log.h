#pragma once
#include "log_storage.h"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cwctype>

namespace ProgramLog {
class Writer {
    struct Row {SYSTEMTIME time{};std::string text;};
    std::wstring m_directory,m_settings;
    std::string m_source;
    NativeHandle m_fileMutex,m_mapping;
    volatile LONG* m_enabled=nullptr;
    std::mutex m_lock;
    std::condition_variable m_changed;
    std::deque<Row> m_rows;
    std::thread m_worker;
    bool m_stopping=false;
    std::atomic<bool> m_failed{false};
    ULONGLONG m_prunedDay=0;

    bool Append(std::wstring const& path,std::string const& rows){
        return AppendDiagnostic(path,"time,source,level,event,details,error_code\r\n",rows);
    }
public:
    bool AppendDiagnostic(std::wstring const& path,const char* header,std::string const& rows){
        const auto wait=WaitForSingleObject(m_fileMutex.value,5000);
        if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)return false;
        struct Unlock {HANDLE mutex;~Unlock(){ReleaseMutex(mutex);}} unlock{m_fileMutex.value};
        if(!LogStorage::EnsureDirectory(m_directory))return false;
        SYSTEMTIME now{};GetLocalTime(&now);const auto day=LogStorage::DayNumber(now);
        if(day!=m_prunedDay){
            if(!LogStorage::PruneDiagnostics(m_directory,L"program-",day)||!LogStorage::PruneDiagnostics(m_directory,L"macro-recording-",day))return false;
            m_prunedDay=day;
        }
        return LogStorage::AppendDiagnostic(path,header,rows);
    }
private:
    void Run() noexcept {
        try{
            for(;;){
                std::deque<Row> batch;bool stopping=false;
                {
                    std::unique_lock guard(m_lock);
                    m_changed.wait(guard,[this]{return m_stopping||!m_rows.empty();});
                    m_changed.wait_for(guard,std::chrono::milliseconds(100),[this]{return m_stopping||m_rows.size()>=128;});
                    batch.swap(m_rows);stopping=m_stopping;
                }
                std::wstring path;std::string rows;
                for(auto& row:batch){
                    auto next=LogStorage::DailyPath(m_directory,L"program-",row.time);
                    if(path!=next){if(!rows.empty()&&!Append(path,rows))m_failed=true;rows.clear();path=std::move(next);}
                    rows+=row.text;
                }
                if(!rows.empty()&&!Append(path,rows))m_failed=true;
                if(stopping)return;
            }
        }catch(...){m_failed=true;}
    }
public:
    ~Writer(){Stop();if(m_enabled)UnmapViewOfFile(const_cast<LONG*>(m_enabled));}
    void Configure(std::wstring const& root,std::wstring const& settings,const char* source){
        m_directory=LogStorage::Path(root,L"log");m_settings=settings;m_source=source;
        ULONGLONG hash=14695981039346656037ULL;
        for(wchar_t value:root){hash^=static_cast<ULONGLONG>(towlower(value));hash*=1099511628211ULL;}
        const auto name=L"Local\\MouseBatteryProgramLog-"+std::to_wstring(hash);
        m_fileMutex.reset(CreateMutexW(nullptr,FALSE,name.c_str()));
        m_mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(LONG),(name+L"-Enabled").c_str()));
        if(m_mapping.value){
            m_enabled=static_cast<LONG*>(MapViewOfFile(m_mapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(LONG)));
            if(m_enabled)InterlockedExchange(m_enabled,GetPrivateProfileIntW(L"Logging",L"ProgramLogEnabled",0,settings.c_str())!=0);
        }
    }
    bool Enabled(std::wstring const& settings) const noexcept {
        if(m_enabled&&settings==m_settings)return InterlockedCompareExchange(m_enabled,0,0)!=0;
        return GetPrivateProfileIntW(L"Logging",L"ProgramLogEnabled",0,settings.c_str())!=0;
    }
    bool Enabled() const noexcept {return !m_settings.empty()&&Enabled(m_settings);}
    bool SetEnabled(std::wstring const& settings,bool enabled){
        if(!WritePrivateProfileStringW(L"Logging",L"ProgramLogEnabled",enabled?L"1":L"0",settings.c_str()))return false;
        if(m_enabled&&settings==m_settings)InterlockedExchange(m_enabled,enabled?1:0);
        return true;
    }
    bool Write(const char* event,std::string_view details,DWORD error,const char* level) noexcept {
        try{
            if(m_settings.empty()||!Enabled(m_settings))return true;
            if(!m_fileMutex.value||m_failed.load())return false;
            Row row;GetLocalTime(&row.time);
            row.text=LogStorage::Time(row.time)+","+m_source+","+(level?level:error?"error":"info")+","+LogStorage::Csv(event)+","+LogStorage::Csv(details)+","+std::to_string(error)+"\r\n";
            std::lock_guard guard(m_lock);
            if(m_stopping||m_rows.size()>=1024)return false;
            if(!m_worker.joinable())m_worker=std::thread([this]{Run();});
            m_rows.push_back(std::move(row));m_changed.notify_one();return true;
        }catch(...){return false;}
    }
    void Stop() noexcept {
        {std::lock_guard guard(m_lock);m_stopping=true;}
        m_changed.notify_one();if(m_worker.joinable())m_worker.join();
    }
};
inline Writer writer;
inline void Configure(std::wstring const& root,std::wstring const& settings,const char* source){writer.Configure(root,settings,source);}
inline bool Enabled(){return writer.Enabled();}
inline bool Enabled(std::wstring const& settings){return writer.Enabled(settings);}
inline bool SetEnabled(std::wstring const& settings,bool enabled){return writer.SetEnabled(settings,enabled);}
inline bool Write(const char* event,std::string_view details={},DWORD error=0,const char* level=nullptr) noexcept {return writer.Write(event,details,error,level);}
}
