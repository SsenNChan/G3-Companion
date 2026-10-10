#pragma once
#include "localization.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace MacroEditor {
inline constexpr std::uint32_t MaximumDelay=0xfffff;
enum class StepKind { Keyboard, Mouse, Wheel, Delay };
struct Step {
    StepKind kind=StepKind::Keyboard;
    std::uint32_t value=4;
    bool pressed=true;
    bool operator==(const Step& other)const{return kind==other.kind&&value==other.value&&pressed==other.pressed;}
};
struct Profile {
    std::wstring id,name=Localization::Get(Localization::T158);
    std::uint64_t created=0;
    std::vector<Step> steps;
    bool stepsLoaded=true;
    std::uint32_t completionDelayMs=0;
    int executionMode=1,repeatCount=1;
};
inline bool SameContent(const Profile& a,const Profile& b){return a.steps==b.steps&&a.completionDelayMs==b.completionDelayMs&&a.executionMode==b.executionMode&&a.repeatCount==b.repeatCount;}
struct Validation { std::wstring message;std::vector<std::size_t> invalid; };
inline bool Validate(const Profile& profile,Validation& result,bool requireActions=false){
    result={};std::uint64_t delay=0;bool previousAction=false,hasDelay=false;std::size_t previousIndex=0;
    if((profile.executionMode!=1&&profile.executionMode!=2&&profile.executionMode!=4)||profile.repeatCount<1||profile.repeatCount>255)
        result.message=Localization::Get(Localization::T159);
    for(std::size_t i=0;i<profile.steps.size();++i){const auto& step=profile.steps[i];
        if(step.kind==StepKind::Delay){
            delay+=step.value;hasDelay=true;
            if(!step.value||step.value>MaximumDelay||delay>MaximumDelay){result.invalid.push_back(i);result.message=Localization::Get(Localization::T160);}
        }else{
            if(previousAction&&!hasDelay){result.invalid.push_back(previousIndex);result.invalid.push_back(i);result.message=Localization::Get(Localization::T161);}
            bool valid=step.kind==StepKind::Keyboard ? ((step.value>=4&&step.value<=0xa4)||(step.value>=0xe0&&step.value<=0xe7)) :
                step.kind==StepKind::Mouse ? (step.value==1||step.value==2||step.value==4||step.value==8||step.value==16) : (step.value==0x0a||step.value==0xff);
            if(!valid){result.invalid.push_back(i);result.message=Localization::Get(Localization::T162);}
            previousAction=true;previousIndex=i;delay=0;hasDelay=false;
        }
    }
    if(!profile.steps.empty()&&profile.steps.back().kind==StepKind::Delay){result.invalid.push_back(profile.steps.size()-1);result.message=Localization::Get(Localization::T163);}
    if(!profile.steps.empty()&&profile.steps.front().kind==StepKind::Delay){
        for(std::size_t i=0;i<profile.steps.size()&&profile.steps[i].kind==StepKind::Delay;++i)result.invalid.push_back(i);
        result.message=Localization::Get(Localization::T164);
    }
    if(profile.completionDelayMs>MaximumDelay||(!previousAction&&profile.completionDelayMs))result.message=Localization::Get(Localization::T165);
    if(requireActions&&!previousAction)result.message=Localization::Get(Localization::T166);
    std::sort(result.invalid.begin(),result.invalid.end());result.invalid.erase(std::unique(result.invalid.begin(),result.invalid.end()),result.invalid.end());
    return result.message.empty();
}
inline std::size_t ActionCount(const Profile& profile){return std::count_if(profile.steps.begin(),profile.steps.end(),[](const auto& step){return step.kind!=StepKind::Delay;});}
inline std::uint64_t Duration(const Profile& profile){std::uint64_t total=profile.completionDelayMs;for(const auto& step:profile.steps)if(step.kind==StepKind::Delay)total+=step.value;return total;}
inline bool Encode(const Profile& profile,const std::string& utf8Name,std::vector<std::uint8_t>& data,std::wstring& message,bool requireActions=true,bool limitStorage=true){
    Validation validation;if(!Validate(profile,validation,requireActions)){message=validation.message;return false;}
    if(utf8Name.empty()||utf8Name.size()>255){message=Localization::Get(Localization::T167);return false;}
    data.clear();data.push_back(static_cast<std::uint8_t>(utf8Name.size()));data.insert(data.end(),utf8Name.begin(),utf8Name.end());
    std::size_t previousEvent=0;bool hasEvent=false;
    for(const auto& step:profile.steps){
        if(step.kind==StepKind::Delay){
            // Firmware waits after the preceding operation, including key-down holds.
            auto delay=((data[previousEvent]&15u)<<16)|(data[previousEvent+1]<<8)|data[previousEvent+2];delay+=step.value;
            data[previousEvent]=static_cast<std::uint8_t>((data[previousEvent]&0xf0)|(delay>>16));data[previousEvent+1]=static_cast<std::uint8_t>(delay>>8);data[previousEvent+2]=static_cast<std::uint8_t>(delay);continue;
        }
        unsigned control=step.kind==StepKind::Wheel?5:step.kind==StepKind::Mouse?(step.pressed?2:10):step.value>=0xe0?(step.pressed?1:9):(step.pressed?0:8);
        previousEvent=data.size();hasEvent=true;
        data.push_back(static_cast<std::uint8_t>(control<<4));data.push_back(0);data.push_back(0);data.push_back(static_cast<std::uint8_t>(step.value));
    }
    if(hasEvent){data[previousEvent]|=static_cast<std::uint8_t>(profile.completionDelayMs>>16);data[previousEvent+1]=static_cast<std::uint8_t>(profile.completionDelayMs>>8);data[previousEvent+2]=static_cast<std::uint8_t>(profile.completionDelayMs);}
    if(limitStorage&&data.size()+4>4096){message=Localization::Get(Localization::T168);return false;}
    message.clear();return true;
}
inline void Insert(std::vector<Step>& steps,std::size_t& cursor,const Step& step,bool overwrite){
    cursor=std::min(cursor,steps.size());
    if(overwrite&&cursor<steps.size())steps[cursor]=step;else steps.insert(steps.begin()+cursor,step);
    ++cursor;
}
inline void Move(std::vector<Step>& steps,std::size_t source,std::size_t destination){
    if(source>=steps.size())return;destination=std::min(destination,steps.size());
    auto step=steps[source];steps.erase(steps.begin()+source);if(destination>source)--destination;steps.insert(steps.begin()+destination,step);
}
// A pending assignment carries an immutable saved profile into the device transaction.
struct Assignment {
    std::wstring profileId,name;
    std::vector<std::uint8_t> data;
    // Native macros without a local profile retain their readback bytes as the timing source.
    std::vector<std::uint8_t> clockSourceData;
    double clockSourceFactor=1;
    std::uint8_t executionMode=1,repeatCount=1;
    bool operator==(const Assignment& other)const{return profileId==other.profileId&&name==other.name&&data==other.data&&clockSourceData==other.clockSourceData&&clockSourceFactor==other.clockSourceFactor&&executionMode==other.executionMode&&repeatCount==other.repeatCount;}
};
}
