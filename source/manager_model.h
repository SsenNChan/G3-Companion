#pragma once
#include "localization.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <optional>
#include <cmath>
#include "macro_model.h"

namespace ManagerBackend {
enum class ApplyMode { EditedSettings, ResetButtonMappings, PrepareClockCalibration, RestoreClockCalibration };
using ButtonCode=std::array<std::uint8_t,4>;
using ButtonMappings=std::array<ButtonCode,6>;
inline constexpr ButtonMappings DefaultButtonMappings={{{1,1,1,0},{1,2,1,0},{1,3,1,0},{1,4,1,0},{1,5,1,0},{5,4,0,0}}};
struct Stage {
    int dpiX=800;
    int dpiY=800;
    // The byte order matches Windows COLORREF: 0x00BBGGRR.
    std::uint32_t color=0x000000ff;
};
struct Settings {
    int sleepMinutes=10;
    int debounceMs=4;
    int reportRate=1000;
    int stageCount=6;
    int currentStage=1;
    bool angleSnap=false;
    bool ripple=false;
    bool clockCompensationEnabled=false;
    double clockCompensationFactor=0;
    std::array<Stage,6> stages{};
    ButtonMappings buttons=DefaultButtonMappings;
    std::array<std::optional<MacroEditor::Assignment>,6> macroAssignments{};
};

inline bool Equals(const Settings& left,const Settings& right) {
    if(left.sleepMinutes!=right.sleepMinutes||left.debounceMs!=right.debounceMs||
       left.reportRate!=right.reportRate||left.stageCount!=right.stageCount||
       left.currentStage!=right.currentStage||left.angleSnap!=right.angleSnap||
       left.ripple!=right.ripple||left.clockCompensationEnabled!=right.clockCompensationEnabled||left.clockCompensationFactor!=right.clockCompensationFactor||left.buttons!=right.buttons||left.macroAssignments!=right.macroAssignments)return false;
    for(std::size_t i=0;i<left.stages.size();++i)
        if(left.stages[i].dpiX!=right.stages[i].dpiX||left.stages[i].dpiY!=right.stages[i].dpiY||
           left.stages[i].color!=right.stages[i].color)return false;
    return true;
}

inline bool Validate(const Settings& settings,std::wstring& message) {
    if(!std::isfinite(settings.clockCompensationFactor)||settings.clockCompensationFactor<0||(settings.clockCompensationEnabled&&settings.clockCompensationFactor<=0)){message=Localization::Get(Localization::T326);return false;}
    if(settings.sleepMinutes<1||settings.sleepMinutes>30){message=Localization::Get(Localization::T327);return false;}
    if(settings.debounceMs<1||settings.debounceMs>14){message=Localization::Get(Localization::T328);return false;}
    if(settings.reportRate!=125&&settings.reportRate!=250&&settings.reportRate!=500&&settings.reportRate!=1000){
        message=Localization::Get(Localization::T329);return false;
    }
    if(settings.stageCount<1||settings.stageCount>6){message=Localization::Get(Localization::T330);return false;}
    if(settings.currentStage<1||settings.currentStage>settings.stageCount){message=Localization::Get(Localization::T331);return false;}
    // Inactive slots can contain firmware values outside the supported editor range.
    // They are retained exactly and are validated when the user enables them.
    for(int i=0;i<settings.stageCount;++i){
        const auto& stage=settings.stages[static_cast<std::size_t>(i)];
        if(stage.dpiX<100||stage.dpiX>12000||stage.dpiX%100||stage.dpiY<100||stage.dpiY>12000||stage.dpiY%100){
            message=Localization::Format(Localization::T332,{{L"stage",std::to_wstring(i+1)}});return false;
        }
        if(stage.color&0xff000000){message=Localization::Get(Localization::T333);return false;}
    }
    message.clear();return true;
}

using ParameterReport=std::array<std::uint8_t,520>;
constexpr std::size_t ParameterReplyLength=408;
inline constexpr std::array<std::uint8_t,8> IdentifierQuery={9,0x0d,5,1,1,0,6,0};
inline bool ValidateIdentifierReply(const ParameterReport& report,std::size_t received) {
    // The identifier command's response checksum byte is not documented.
    return received==14 && report[0]==9 && std::equal(IdentifierQuery.begin()+2,IdentifierQuery.end(),report.begin()+2);
}
inline bool HasDeviceIdentifier(const ParameterReport& report) {
    return std::any_of(report.begin()+8,report.begin()+14,[](std::uint8_t value){return value!=0;});
}
inline std::uint8_t ParameterChecksum(const ParameterReport& report) {
    unsigned total=0;for(std::size_t i=2;i<report.size();++i)total+=report[i];
    return static_cast<std::uint8_t>(total);
}
inline bool ValidateParameterReply(const ParameterReport& report,std::size_t received) {
    return received==ParameterReplyLength&&report[0]==9&&report[2]==0x44&&report[3]==0&&
        report[4]==1&&report[5]==0&&report[6]==0x90&&report[7]==1&&
        report[1]==ParameterChecksum(report);
}
inline int DecodeDpi(const ParameterReport& report,std::size_t offset) {
    return (static_cast<int>(report[offset])+(static_cast<int>(report[offset+1])<<8)+1)*100;
}
inline bool ParseParameters(const ParameterReport& report,std::size_t received,Settings& settings,std::wstring& message) {
    if(!ValidateParameterReply(report,received)){message=Localization::Get(Localization::T334);return false;}
    Settings decoded;
    decoded.buttons=settings.buttons;
    decoded.macroAssignments=settings.macroAssignments;
    decoded.clockCompensationEnabled=settings.clockCompensationEnabled;decoded.clockCompensationFactor=settings.clockCompensationFactor;
    switch(report[10]){case 1:decoded.reportRate=125;break;case 2:decoded.reportRate=250;break;
        case 3:decoded.reportRate=500;break;case 4:decoded.reportRate=1000;break;
        default:message=Localization::Get(Localization::T335);return false;}
    decoded.stageCount=report[11]&15;decoded.currentStage=report[11]>>4;
    decoded.debounceMs=report[13]&15;
    if((report[14]&15)>1||(report[14]>>4)>1){message=Localization::Get(Localization::T336);return false;}
    decoded.angleSnap=(report[14]&15)!=0;decoded.ripple=(report[14]>>4)!=0;
    decoded.sleepMinutes=report[82];
    for(std::size_t i=0;i<decoded.stages.size();++i){
        decoded.stages[i].dpiX=DecodeDpi(report,15+2*i);decoded.stages[i].dpiY=DecodeDpi(report,35+2*i);
        const std::size_t offset=55+3*i;
        decoded.stages[i].color=static_cast<std::uint32_t>(report[offset])|
            (static_cast<std::uint32_t>(report[offset+1])<<8)|(static_cast<std::uint32_t>(report[offset+2])<<16);
    }
    if(!Validate(decoded,message))return false;
    settings=decoded;return true;
}
inline void EncodeDpi(ParameterReport& report,std::size_t offset,int dpi) {
    const unsigned value=static_cast<unsigned>(dpi/100-1);
    report[offset]=static_cast<std::uint8_t>(value);report[offset+1]=static_cast<std::uint8_t>(value>>8);
}

// The fresh report supplies all bytes that the user did not edit.
inline bool BuildParameterWrite(const ParameterReport& fresh,const Settings& draft,const Settings& baseline,
                               ParameterReport& write,Settings& expected,std::wstring& message) {
    if(!Validate(draft,message))return false;
    if(!ParseParameters(fresh,ParameterReplyLength,expected,message))return false;
    write=fresh;
    std::fill(write.begin()+ParameterReplyLength,write.end(),std::uint8_t{0});
    if(draft.sleepMinutes!=baseline.sleepMinutes)write[82]=static_cast<std::uint8_t>(draft.sleepMinutes);
    if(draft.debounceMs!=baseline.debounceMs)write[13]=static_cast<std::uint8_t>((write[13]&0xf0)|draft.debounceMs);
    if(draft.reportRate!=baseline.reportRate)
        write[10]=draft.reportRate==125?1:draft.reportRate==250?2:draft.reportRate==500?3:4;
    if(draft.angleSnap!=baseline.angleSnap)write[14]=static_cast<std::uint8_t>((write[14]&0xf0)|(draft.angleSnap?1:0));
    if(draft.ripple!=baseline.ripple)write[14]=static_cast<std::uint8_t>((write[14]&15)|(draft.ripple?0x10:0));
    const int count=draft.stageCount!=baseline.stageCount?draft.stageCount:expected.stageCount;
    // The editor does not expose current-stage selection. A draft stage adjustment
    // only keeps its local shape valid; the latest physical selection remains authoritative.
    const int current=std::clamp(expected.currentStage,1,count);
    write[11]=static_cast<std::uint8_t>((current<<4)|count);
    // Hidden slots retain fresh device bytes, including previously edited slots hidden by a count reduction.
    for(int i=0;i<draft.stageCount;++i){
        const auto index=static_cast<std::size_t>(i);
        if(draft.stages[index].dpiX!=baseline.stages[index].dpiX)EncodeDpi(write,15+2*index,draft.stages[index].dpiX);
        if(draft.stages[index].dpiY!=baseline.stages[index].dpiY)EncodeDpi(write,35+2*index,draft.stages[index].dpiY);
        if(draft.stages[index].color!=baseline.stages[index].color){
            const auto color=draft.stages[index].color;const auto offset=55+3*index;
            write[offset]=static_cast<std::uint8_t>(color);write[offset+1]=static_cast<std::uint8_t>(color>>8);
            write[offset+2]=static_cast<std::uint8_t>(color>>16);
        }
    }
    // Decode the intended result before changing the opcode. This catches newly enabled invalid slots.
    write[1]=ParameterChecksum(write);
    if(!ParseParameters(write,ParameterReplyLength,expected,message))return false;
    write[2]=4;write[1]=ParameterChecksum(write);return true;
}
inline bool EqualParameterPayload(const ParameterReport& write,const ParameterReport& reply) {
    return std::equal(write.begin()+8,write.begin()+ParameterReplyLength,reply.begin()+8);
}
}
