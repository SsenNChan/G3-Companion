#pragma once
#include "localization.h"
#include <windows.h>
#include <cmath>
#include <cwchar>
#include "macro_model.h"

namespace ClockCompensation {
struct State { bool enabled=false;double factor=0; };
inline bool Valid(double factor){return std::isfinite(factor)&&factor>0;}
inline double Effective(State state){return state.enabled&&Valid(state.factor)?state.factor:1;}
inline State Read(std::wstring const& path){
    wchar_t text[64]{};GetPrivateProfileStringW(L"Macros",L"ClockCompensationFactor",L"0",text,64,path.c_str());wchar_t* end=nullptr;
    State state;state.factor=wcstod(text,&end);if(end==text||*end||!Valid(state.factor))state.factor=0;
    state.enabled=state.factor>0&&GetPrivateProfileIntW(L"Macros",L"ClockCompensationEnabled",0,path.c_str())!=0;return state;
}
inline bool Save(std::wstring const& path,State state){
    wchar_t text[64]{};swprintf_s(text,L"%.17g",state.factor);
    return WritePrivateProfileStringW(L"Macros",L"ClockCompensationFactor",text,path.c_str())&&WritePrivateProfileStringW(L"Macros",L"ClockCompensationEnabled",state.enabled?L"1":L"0",path.c_str());
}
inline bool ScaleData(std::vector<std::uint8_t>& data,double factor,std::wstring& error){
    if(!Valid(factor)||data.empty()||data.size()<1u+data[0]||(data.size()-1u-data[0])%4){error=Localization::Get(Localization::T295);return false;}
    auto scaled=data;
    for(std::size_t i=1u+data[0];i<data.size();i+=4){const auto delay=((data[i]&15u)<<16)|(data[i+1]<<8)|data[i+2];
        const auto value=delay?std::max(1.0,std::round(delay*factor)):0.0;
        if(!std::isfinite(value)||value>MacroEditor::MaximumDelay){error=Localization::Get(Localization::T296);return false;}
        const auto time=static_cast<std::uint32_t>(value);scaled[i]=static_cast<std::uint8_t>((data[i]&0xf0)|(time>>16));scaled[i+1]=static_cast<std::uint8_t>(time>>8);scaled[i+2]=static_cast<std::uint8_t>(time);
    }
    data=std::move(scaled);error.clear();return true;
}
}
