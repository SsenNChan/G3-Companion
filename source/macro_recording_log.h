#pragma once
#include "program_log.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <thread>
#include <utility>
#include <functional>
#include <memory>
#include <system_error>

namespace MacroRecordingLog {
struct Event {
    FILETIME wallTime{};
    std::int64_t counter=0;
    const char* type="mouse";
    DWORD message=0,virtualKey=0,scanCode=0,flags=0,hookTime=0,mouseData=0,usage=0;
    ULONG_PTR extraInfo=0;
    LONG x=0,y=0;
    bool accepted=false;
    const char* reason="unsupported";
    std::size_t cursorBefore=0,cursorAfter=0;
    std::uint32_t intervalMs=0;
};
inline Event Timestamp(const char* type){
    Event event;event.type=type;GetSystemTimeAsFileTime(&event.wallTime);LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);event.counter=counter.QuadPart;return event;
}
class Recorder {
    std::wstring m_directory;
    std::string m_session,m_profileId,m_name;
    std::int64_t m_started=0,m_frequency=1;
    std::mutex m_lock;
    std::condition_variable m_changed;
    std::deque<Event> m_events;
    std::thread m_worker;
    bool m_enabled=false,m_stopping=false;
    std::atomic<DWORD> m_error{0};
    void Run() noexcept {
        try{
            std::uint64_t sequence=0;
            std::deque<Event> events;
            std::wstring path;std::string rows;
            const auto session=LogStorage::Csv(m_session),profile=LogStorage::Csv(m_profileId),name=LogStorage::Csv(m_name);
            for(;;){
                bool stopping=false;
                {std::unique_lock<std::mutex> guard(m_lock);m_changed.wait(guard,[this]{return m_stopping||!m_events.empty();});
                    m_changed.wait_for(guard,std::chrono::milliseconds(100),[this]{return m_stopping||m_events.size()>=1024;});events.swap(m_events);stopping=m_stopping;}
                const auto flush=[&](){if(rows.empty())return;SetLastError(ERROR_SUCCESS);if(!ProgramLog::writer.AppendDiagnostic(path,"time,session_id,profile_id,macro_name,sequence,qpc_counter,qpc_frequency,elapsed_us,event_type,message,vk_code,scan_code,flags,hook_time_ms,extra_info,x,y,mouse_data,usage,accepted,reason,cursor_before,cursor_after,interval_ms\r\n",rows)){const auto error=GetLastError();m_error=error?error:ERROR_WRITE_FAULT;}rows.clear();};
                for(auto const& event:events){
                    SYSTEMTIME time{};FILETIME local{};FileTimeToLocalFileTime(&event.wallTime,&local);FileTimeToSystemTime(&local,&time);
                    auto eventPath=LogStorage::DailyPath(m_directory,L"macro-recording-",time);if(path!=eventPath){flush();path=std::move(eventPath);}
                    char milliseconds[8]{};sprintf_s(milliseconds,".%03u",time.wMilliseconds);
                    rows+=LogStorage::Time(time)+milliseconds+","+session+","+profile+","+name+","+std::to_string(sequence++)+","+std::to_string(event.counter)+","+std::to_string(m_frequency)+","+std::to_string(static_cast<std::int64_t>((event.counter-m_started)*1000000.0/m_frequency))+","+event.type+","+std::to_string(event.message)+","+std::to_string(event.virtualKey)+","+std::to_string(event.scanCode)+","+std::to_string(event.flags)+","+std::to_string(event.hookTime)+","+std::to_string(event.extraInfo)+","+std::to_string(event.x)+","+std::to_string(event.y)+","+std::to_string(event.mouseData)+","+std::to_string(event.usage)+","+(event.accepted?"1":"0")+","+event.reason+","+std::to_string(event.cursorBefore)+","+std::to_string(event.cursorAfter)+","+std::to_string(event.intervalMs)+"\r\n";
                }
                flush();events.clear();if(stopping)break;
            }
        }catch(...){m_error=ERROR_WRITE_FAULT;}
    }
public:
    Recorder(std::wstring const& root,std::wstring const& settings,std::string session,std::string profileId,std::string name,std::size_t cursor):
        m_directory(LogStorage::Path(root,L"log")),m_session(std::move(session)),m_profileId(std::move(profileId)),m_name(std::move(name)),m_enabled(ProgramLog::Enabled(settings)){
        if(!m_enabled)return;
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);m_frequency=frequency.QuadPart;
        auto start=Timestamp("session_start");m_started=start.counter;start.reason="record_button";start.cursorBefore=start.cursorAfter=cursor;m_events.push_back(start);
        m_worker=std::thread([this]{Run();});
    }
    Recorder(Recorder const&)=delete;
    ~Recorder(){Stop();}
    void Push(Event event) noexcept {
        if(!m_enabled||m_error)return;
        try{
            bool notify=false;
            {std::lock_guard<std::mutex> guard(m_lock);if(m_stopping)return;
                // Cap diagnostic backlog when storage cannot keep up with input.
                if(m_events.size()>=8192){m_error=ERROR_BUFFER_OVERFLOW;return;}
                notify=m_events.empty();m_events.push_back(event);notify|=m_events.size()==1024;
            }
            if(notify)m_changed.notify_one();
        }catch(...){m_error=ERROR_NOT_ENOUGH_MEMORY;}
    }
    bool Enabled() const noexcept {return m_enabled&&!m_error.load();}
    bool HasWorker() const noexcept {return m_worker.joinable();}
    DWORD Stop(std::size_t cursor=0) noexcept {
        if(!m_worker.joinable())return m_error;
        auto stop=Timestamp("session_stop");stop.reason="recording_stopped";stop.cursorBefore=stop.cursorAfter=cursor;Push(stop);
        {std::lock_guard<std::mutex> guard(m_lock);m_stopping=true;}m_changed.notify_one();m_worker.join();return m_error;
    }
};
class Completions {
    struct Task {Completions* owner;std::shared_ptr<Recorder> recorder;std::size_t cursor;std::function<void(DWORD)> report;};
    std::mutex m_lock;
    std::condition_variable m_finished;
    std::size_t m_active=0;
    static void CALLBACK Run(PTP_CALLBACK_INSTANCE instance,void* context){
        CallbackMayRunLong(instance);
        std::unique_ptr<Task> task(static_cast<Task*>(context));auto owner=task->owner;
        const auto error=task->recorder->Stop(task->cursor);
        if(error)try{task->report(error);}catch(...){}
        task.reset();
        std::lock_guard guard(owner->m_lock);--owner->m_active;owner->m_finished.notify_all();
    }
public:
    ~Completions(){Drain();}
    void Finish(std::shared_ptr<Recorder> recorder,std::size_t cursor,std::function<void(DWORD)> report){
        if(!recorder->HasWorker())return;
        auto task=std::make_unique<Task>(Task{this,std::move(recorder),cursor,std::move(report)});
        std::lock_guard guard(m_lock);
        if(!TrySubmitThreadpoolCallback(Run,task.get(),nullptr))throw std::system_error(GetLastError(),std::system_category());
        ++m_active;task.release();
    }
    void Drain(){std::unique_lock guard(m_lock);m_finished.wait(guard,[this]{return m_active==0;});}
};
inline Completions completions;
inline void Finish(std::shared_ptr<Recorder> recorder,std::size_t cursor,std::function<void(DWORD)> report){completions.Finish(std::move(recorder),cursor,std::move(report));}
inline void Drain(){completions.Drain();}
}
