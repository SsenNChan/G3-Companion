#pragma once
#include "localization.h"
#include "manager_model.h"
#include <vector>
#include <cwchar>

namespace ManagerBackend {
inline constexpr std::size_t MappingReplyLength=80;
inline constexpr std::array<std::uint8_t,8> MappingQuery={9,0,0x41,0,1,0,0x48,0};
inline bool ValidateMappingReply(const ParameterReport& report,std::size_t received) {
    return received==MappingReplyLength&&report[0]==9&&
        std::equal(MappingQuery.begin()+2,MappingQuery.end(),report.begin()+2)&&report[1]==ParameterChecksum(report);
}
inline bool ParseMappings(const ParameterReport& report,std::size_t received,ButtonMappings& buttons,std::wstring& message) {
    if(!ValidateMappingReply(report,received)){message=Localization::Get(Localization::T041);return false;}
    for(std::size_t i=0;i<buttons.size();++i)std::copy_n(report.begin()+8+4*i,4,buttons[i].begin());
    message.clear();return true;
}
inline bool BuildMappingWrite(const ParameterReport& fresh,const ButtonMappings& draft,const ButtonMappings& baseline,
                              ParameterReport& write,ButtonMappings& expected,std::wstring& message,ApplyMode mode=ApplyMode::EditedSettings) {
    if(!ParseMappings(fresh,MappingReplyLength,expected,message))return false;
    write=fresh;std::fill(write.begin()+MappingReplyLength,write.end(),std::uint8_t{0});
    for(std::size_t i=0;i<draft.size();++i)if(mode==ApplyMode::ResetButtonMappings||draft[i]!=baseline[i]){
        expected[i]=mode==ApplyMode::ResetButtonMappings?DefaultButtonMappings[i]:draft[i];
        std::copy(expected[i].begin(),expected[i].end(),write.begin()+8+4*i);
    }
    write[2]=1;write[1]=ParameterChecksum(write);return true;
}
inline bool EqualMappingPayload(const ParameterReport& write,const ParameterReport& reply) {
    return std::equal(write.begin()+8,write.begin()+MappingReplyLength,reply.begin()+8);
}
struct MacroConfiguration {
    std::wstring name;
    ButtonCode binding{}; // Execution mode and count belong to the defined configuration.
    std::vector<std::uint8_t> data;
};
using MacroStorage=std::array<std::uint8_t,4096>;
inline bool ValidateMacroPage(const ParameterReport& report,std::size_t received,std::uint8_t page) {
    return received==520&&report[0]==9&&report[2]==0x43&&report[3]==0&&report[4]==1&&report[5]==page&&
        report[6]==0&&report[7]==2&&report[1]==ParameterChecksum(report);
}
inline bool MacroNameBytes(const MacroStorage& storage,std::size_t index,std::string& name) {
    const auto word=[&](std::size_t at){return static_cast<std::size_t>(storage[at])+(static_cast<std::size_t>(storage[at+1])<<8);};
    const auto directoryLength=word(0);
    if(directoryLength<4||directoryLength>1024||directoryLength%4||index>=directoryLength/4)return false;
    const auto start=word(4*index),length=word(4*index+2);
    if(start<directoryLength||!length||start>storage.size()||length>storage.size()-start)return false;
    const auto nameLength=static_cast<std::size_t>(storage[start]);
    if(nameLength+1>length||(length-nameLength-1)%4)return false;
    name.assign(reinterpret_cast<const char*>(storage.data()+start+1),nameLength);return true;
}
inline std::wstring UnknownButtonName(const ButtonCode& code) {
    wchar_t text[64]{};
    if(code[0]==3)return Localization::Format(Localization::T042,{{L"index",std::to_wstring(static_cast<unsigned>(code[3])+1)}});
    std::swprintf(text,64,L"%02X %02X %02X %02X",code[0],code[1],code[2],code[3]);return Localization::Format(Localization::T043,{{L"code",text}});
}
}
