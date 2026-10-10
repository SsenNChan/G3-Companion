#pragma once
#include "../localization.h"
#include "../macro_model.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cwchar>
#include <atomic>
#include "../native_handle.h"

namespace MacroLibrary {
using namespace winrt::Windows::Data::Json;
inline std::wstring NewId(){GUID id{};winrt::check_hresult(CoCreateGuid(&id));wchar_t text[40]{};StringFromGUID2(id,text,40);return std::wstring(text+1,36);}
inline std::uint64_t Now(){FILETIME time{};GetSystemTimeAsFileTime(&time);return (static_cast<std::uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime;}
inline bool SameName(std::wstring const& a,std::wstring const& b){return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;}
inline bool ValidName(std::wstring const& name){
    if(name.empty()||winrt::to_string(name).size()>255||name.back()==L'.'||name.back()==L' '||name.find_first_of(L"<>:\"/\\|?*")!=std::wstring::npos)return false;
    for(auto c:name)if(c<32)return false;
    auto base=name.substr(0,name.find(L'.'));
    for(auto reserved:{L"CON",L"PRN",L"AUX",L"NUL",L"CONIN$",L"CONOUT$"})if(SameName(base,reserved))return false;
    if(base.size()==4&&(SameName(base.substr(0,3),L"COM")||SameName(base.substr(0,3),L"LPT"))&&((base[3]>=L'1'&&base[3]<=L'9')||base[3]==L'\x00b9'||base[3]==L'\x00b2'||base[3]==L'\x00b3'))return false;
    return true;
}
inline std::filesystem::path File(const std::wstring& directory,const std::wstring& name){
    if(!ValidName(name))throw winrt::hresult_invalid_argument(Localization::Get(Localization::T173));
    return std::filesystem::path(directory)/(name+L".bin");
}
inline std::wstring TimeText(std::uint64_t value){
    FILETIME utc{static_cast<DWORD>(value),static_cast<DWORD>(value>>32)},local{};SYSTEMTIME time{};
    winrt::check_bool(FileTimeToLocalFileTime(&utc,&local));winrt::check_bool(FileTimeToSystemTime(&local,&time));
    wchar_t text[32]{};swprintf_s(text,L"%04u-%02u-%02u  %02u-%02u-%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);return text;
}
inline std::uint64_t ParseTime(std::wstring const& text){
    SYSTEMTIME time{};if(text.size()!=20||swscanf_s(text.c_str(),L"%hu-%hu-%hu  %hu-%hu-%hu",&time.wYear,&time.wMonth,&time.wDay,&time.wHour,&time.wMinute,&time.wSecond)!=6)throw winrt::hresult_invalid_argument(Localization::Get(Localization::T174));
    FILETIME local{},utc{};winrt::check_bool(SystemTimeToFileTime(&time,&local));winrt::check_bool(LocalFileTimeToFileTime(&local,&utc));
    auto value=(static_cast<std::uint64_t>(utc.dwHighDateTime)<<32)|utc.dwLowDateTime;if(TimeText(value)!=text)throw winrt::hresult_invalid_argument(Localization::Get(Localization::T174));return value;
}
inline JsonObject Serialize(const MacroEditor::Profile& profile){
    File(L"",profile.name);std::vector<std::uint8_t> data;std::wstring error;
    if(!MacroEditor::Encode(profile,winrt::to_string(profile.name),data,error,false,false))throw winrt::hresult_invalid_argument(error);
    const wchar_t* hex=L"0123456789ABCDEF";std::wstring text;text.reserve(data.size()*4);
    for(auto byte:data){text+=L"0x";text+=hex[byte>>4];text+=hex[byte&15];}
    JsonObject fields;fields.SetNamedValue(L"RunOne",JsonValue::CreateNumberValue(profile.repeatCount));fields.SetNamedValue(L"RunWay",JsonValue::CreateNumberValue(profile.executionMode));
    fields.SetNamedValue(L"Time",JsonValue::CreateStringValue(TimeText(profile.created?profile.created:Now())));fields.SetNamedValue(L"buffsize",JsonValue::CreateNumberValue(static_cast<double>(data.size())));
    fields.SetNamedValue(L"mdfdata",JsonValue::CreateStringValue(text));JsonObject object;object.SetNamedValue(profile.name,fields);return object;
}
inline MacroEditor::Profile Parse(const JsonObject& object){
    if(object.Size()!=1)throw winrt::hresult_invalid_argument(Localization::Get(Localization::T175));auto outer=object.First().Current();MacroEditor::Profile profile;profile.id=NewId();profile.name=outer.Key();File(L"",profile.name);
    const auto fields=outer.Value().GetObject();const auto mode=fields.GetNamedNumber(L"RunWay"),count=fields.GetNamedNumber(L"RunOne"),size=fields.GetNamedNumber(L"buffsize");
    if((mode!=1&&mode!=2&&mode!=4)||!std::isfinite(count)||count<1||count>255||std::floor(count)!=count||!std::isfinite(size)||size<1||size>UINT32_MAX||std::floor(size)!=size)throw winrt::hresult_invalid_argument(Localization::Get(Localization::T176));
    profile.executionMode=static_cast<int>(mode);profile.repeatCount=static_cast<int>(count);profile.created=ParseTime(std::wstring(fields.GetNamedString(L"Time")));
    auto encodedText=fields.GetNamedString(L"mdfdata");std::wstring_view text(encodedText);if(text.size()!=static_cast<std::size_t>(size)*4)throw winrt::hresult_invalid_argument(Localization::Get(Localization::T177));
    const auto digit=[](wchar_t c)->int{if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'A'&&c<=L'F')return c-L'A'+10;if(c>=L'a'&&c<=L'f')return c-L'a'+10;return -1;};
    std::vector<std::uint8_t> data;data.reserve(static_cast<std::size_t>(size));
    for(std::size_t i=0;i<text.size();i+=4){auto high=digit(text[i+2]),low=digit(text[i+3]);if(text[i]!=L'0'||(text[i+1]!=L'x'&&text[i+1]!=L'X')||high<0||low<0)throw winrt::hresult_invalid_argument(Localization::Get(Localization::T178));data.push_back(static_cast<std::uint8_t>((high<<4)|low));}
    auto name=winrt::to_string(profile.name);if(data[0]!=name.size()||data.size()<name.size()+1||(data.size()-name.size()-1)%4||!std::equal(name.begin(),name.end(),data.begin()+1,[](char a,std::uint8_t b){return static_cast<std::uint8_t>(a)==b;}))throw winrt::hresult_invalid_argument(Localization::Get(Localization::T179));
    profile.steps.reserve((data.size()-name.size()-1)/2);
    for(std::size_t i=name.size()+1;i<data.size();i+=4){const auto control=data[i]>>4;const std::uint32_t delay=((data[i]&15)<<16)|(data[i+1]<<8)|data[i+2];MacroEditor::Step step;step.value=data[i+3];
        switch(control){case 0:case 8:step.kind=MacroEditor::StepKind::Keyboard;step.pressed=control==0;if(step.value<4||step.value>0xa4)throw winrt::hresult_invalid_argument(Localization::Get(Localization::T180));break;
        case 1:case 9:step.kind=MacroEditor::StepKind::Keyboard;step.pressed=control==1;if(step.value<0xe0||step.value>0xe7)throw winrt::hresult_invalid_argument(Localization::Get(Localization::T181));break;
        case 2:case 10:step.kind=MacroEditor::StepKind::Mouse;step.pressed=control==2;break;case 5:step.kind=MacroEditor::StepKind::Wheel;break;
        default:throw winrt::hresult_invalid_argument(Localization::Get(Localization::T182));}
        profile.steps.push_back(step);
        if(i+4<data.size()){if(delay)profile.steps.push_back({MacroEditor::StepKind::Delay,delay,true});}
        else profile.completionDelayMs=delay;
    }
    MacroEditor::Validation validation;if(!MacroEditor::Validate(profile,validation))throw winrt::hresult_invalid_argument(validation.message);return profile;
}
inline void Save(const std::wstring& directory,const MacroEditor::Profile& profile){
    std::filesystem::create_directories(directory);const auto path=File(directory,profile.name);
    const auto temporary=path.wstring()+L"."+NewId()+L".tmp";
    auto saved=profile;saved.created=Now();const auto bytes=winrt::to_string(Serialize(saved).Stringify());
    NativeHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)winrt::throw_last_error();
    TemporaryFile cleanup(temporary);DWORD written=0;
    const bool complete=WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file.value);
    const auto error=GetLastError();file.reset();
    if(!complete)throw winrt::hresult_error(HRESULT_FROM_WIN32(error?error:ERROR_WRITE_FAULT),Localization::Get(Localization::T183));
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))winrt::throw_last_error();
}
inline void Rename(const std::wstring& directory,const MacroEditor::Profile& profile,std::wstring const& previousName){
    const auto previous=File(directory,previousName),next=File(directory,profile.name);if(!SameName(previousName,profile.name)&&std::filesystem::exists(next))throw winrt::hresult_invalid_argument(Localization::Get(Localization::T184));
    Save(directory,profile);if(!SameName(previousName,profile.name)&&std::filesystem::exists(previous))std::filesystem::remove(previous);
}
inline constexpr LONGLONG MaximumFileBytes=16*1024*1024;
inline JsonObject Read(const std::filesystem::path& path){
    winrt::file_handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(file.get()==INVALID_HANDLE_VALUE)winrt::throw_last_error();
    LARGE_INTEGER size{};winrt::check_bool(GetFileSizeEx(file.get(),&size));
    if(size.QuadPart<=0||size.QuadPart>MaximumFileBytes)throw winrt::hresult_invalid_argument(Localization::Get(Localization::T185));
    std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0');DWORD read=0;
    winrt::check_bool(ReadFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr));
    if(read!=bytes.size())throw winrt::hresult_error(E_FAIL,Localization::Get(Localization::T186));
    return JsonObject::Parse(winrt::to_hstring(bytes));
}
inline std::vector<MacroEditor::Profile> Load(const std::wstring& directory,std::wstring& errors,const std::atomic<bool>* canceled=nullptr){
    std::vector<MacroEditor::Profile> profiles;std::filesystem::create_directories(directory);std::vector<std::filesystem::path> files;
    for(const auto& entry:std::filesystem::directory_iterator(directory))if(SameName(entry.path().extension().wstring(),L".bin")&&!entry.is_symlink()&&entry.is_regular_file())files.push_back(entry.path());std::sort(files.begin(),files.end());
    const auto report=[&](auto const& file){if(!errors.empty())errors+=Localization::IsEnglish()?L"; ":L"；";errors+=Localization::Format(Localization::T187,{{L"filename",file.filename().wstring()}});};
    for(const auto& file:files){if(canceled&&canceled->load())break;try{
        auto profile=Parse(Read(file));if(!SameName(profile.name,file.stem().wstring()))throw winrt::hresult_invalid_argument();std::vector<MacroEditor::Step>().swap(profile.steps);profile.stepsLoaded=false;profiles.push_back(std::move(profile));
    }catch(winrt::hresult_error const&){report(file);}}
    std::stable_sort(profiles.begin(),profiles.end(),[](auto const& a,auto const& b){return a.created<b.created;});return profiles;
}
}
