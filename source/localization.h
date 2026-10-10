#pragma once
#include "program_mode.h"
#include "localization_strings.h"
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace Localization {
enum class Override { System, Chinese, English };

inline Override LanguageOverride() {
    static const auto value=ProgramMode::HasArgument(L"--lang=en-US")?Override::English:
        ProgramMode::HasArgument(L"--lang=zh-CN")?Override::Chinese:Override::System;
    return value;
}
inline bool IsEnglish() {
    static const bool value=LanguageOverride()==Override::English ||
        (LanguageOverride()==Override::System && PRIMARYLANGID(GetUserDefaultUILanguage())!=LANG_CHINESE);
    return value;
}
inline const wchar_t* LanguageTag(){return IsEnglish()?L"en-US":L"zh-CN";}
inline const wchar_t* LaunchArgument() {
    switch(LanguageOverride()){
    case Override::English:return L" --lang=en-US";
    case Override::Chinese:return L" --lang=zh-CN";
    default:return L"";
    }
}
inline void ConfigureNativeLanguage() {
    const wchar_t* languages=IsEnglish()?L"en-US\0":L"zh-CN\0";
    ULONG count=0;
    SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME,languages,&count);
    SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME,languages,&count);
    SetThreadUILanguage(MAKELANGID(IsEnglish()?LANG_ENGLISH:LANG_CHINESE,
        IsEnglish()?SUBLANG_ENGLISH_US:SUBLANG_CHINESE_SIMPLIFIED));
}
inline const wchar_t* Get(TextId id) {
    const auto& text=Strings[static_cast<std::size_t>(id)];
    return IsEnglish()?text.english:text.chinese;
}
inline const wchar_t* Key(TextId id){return Strings[static_cast<std::size_t>(id)].key;}

inline std::wstring Format(TextId id,
    std::initializer_list<std::pair<std::wstring_view,std::wstring_view>> arguments) {
    std::wstring result=Get(id);
    for(const auto& [name,value]:arguments){
        std::wstring marker=L"{";marker+=name;marker+=L"}";
        std::size_t position=0;
        while((position=result.find(marker,position))!=std::wstring::npos){
            result.replace(position,marker.size(),value);
            position+=value.size();
        }
    }
    return result;
}
}
