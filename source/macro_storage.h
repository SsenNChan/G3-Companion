#pragma once
#include "localization.h"
#include "button_mapping.h"

namespace ManagerBackend {
inline bool BuildMacroStorage(const MacroStorage& fresh,const ParameterReport& mappings,const Settings& draft,
                             MacroStorage& output,ButtonMappings& resolved,std::wstring& message){
    const auto word=[&](std::size_t offset){return static_cast<std::size_t>(fresh[offset])+(static_cast<std::size_t>(fresh[offset+1])<<8);};
    std::vector<std::vector<std::uint8_t>> entries;
    const auto directoryLength=word(0);
    if(directoryLength){
        if(directoryLength>1024||directoryLength%4){message=Localization::Get(Localization::T169);return false;}
        for(std::size_t index=0;index<directoryLength/4;++index){
            std::string name;if(!MacroNameBytes(fresh,index,name)){message=Localization::Get(Localization::T169);return false;}
            const auto start=word(index*4),length=word(index*4+2);entries.emplace_back(fresh.begin()+start,fresh.begin()+start+length);
        }
    }else if(std::any_of(fresh.begin(),fresh.begin()+4,[](auto byte){return byte!=0;})){
        message=Localization::Get(Localization::T169);return false;
    }
    resolved=draft.buttons;
    for(std::size_t button=0;button<6;++button)if(draft.macroAssignments[button]){
        const auto& assignment=*draft.macroAssignments[button];
        if(assignment.data.empty()||assignment.data[0]+1>assignment.data.size()||(assignment.data.size()-assignment.data[0]-1)%4||
            (assignment.executionMode!=1&&assignment.executionMode!=2&&assignment.executionMode!=4)||!assignment.repeatCount){message=Localization::Get(Localization::T170);return false;}
        auto found=std::find(entries.begin(),entries.end(),assignment.data);std::size_t index=found-entries.begin();
        if(found==entries.end()){
            const auto original=mappings[8+4*button+3];std::size_t references=0;
            for(std::size_t slot=8;slot+3<MappingReplyLength;slot+=4)if(mappings[slot]==3&&mappings[slot+3]==original)++references;
            bool requestedElsewhere=false;
            for(std::size_t other=0;other<6;++other)if(other!=button&&resolved[other][0]==3&&resolved[other][3]==original&&(!draft.macroAssignments[other]||other<button))requestedElsewhere=true;
            if(mappings[8+4*button]==3&&original<entries.size()&&references==1&&!requestedElsewhere){index=original;entries[index]=assignment.data;}
            else{index=entries.size();entries.push_back(assignment.data);}
        }
        if(index>255){message=Localization::Get(Localization::T171);return false;}
        resolved[button]={3,assignment.executionMode,assignment.repeatCount,static_cast<std::uint8_t>(index)};
    }
    std::size_t size=entries.size()*4;for(const auto& entry:entries)size+=entry.size();
    if(size>4096){message=Localization::Get(Localization::T172);return false;}
    output=fresh;std::size_t cursor=entries.size()*4;
    for(std::size_t index=0;index<entries.size();++index){const auto length=entries[index].size();
        output[index*4]=static_cast<std::uint8_t>(cursor);output[index*4+1]=static_cast<std::uint8_t>(cursor>>8);
        output[index*4+2]=static_cast<std::uint8_t>(length);output[index*4+3]=static_cast<std::uint8_t>(length>>8);
        std::copy(entries[index].begin(),entries[index].end(),output.begin()+cursor);cursor+=length;
    }
    message.clear();return true;
}
}
