#pragma once
#include <cstddef>
#include <cstdint>

namespace MouseState {
enum class Mode { Unknown, Wireless, Offline, Wired, Missing, Charging, Error, AwaitingBattery };
constexpr int RelativePercent(int raw) {
    if(raw<0||raw>100)return -1;
    const int relative=(raw*100+47)/94;
    return relative>100?100:relative;
}
constexpr bool ShowPercent(Mode mode) { return mode==Mode::Wireless||mode==Mode::Offline||mode==Mode::Error; }
inline bool QueryReply(const uint8_t* bytes,size_t length,const uint8_t* header) {
    if(length!=9)return false;
    for(size_t i=0;i<8;++i)if(bytes[i]!=header[i])return false;
    return true;
}
enum class Event { None, Online, Offline, Battery, Power };
class BatteryReportWait {
    bool online=false;
    uint64_t deadline=0;
public:
    static constexpr uint64_t TimeoutMilliseconds=2000;
    bool Observe(Event event,uint64_t now) {
        if(event==Event::Online){
            const bool newlyOnline=!online;
            if(newlyOnline)deadline=now+TimeoutMilliseconds;
            online=true;return newlyOnline;
        }
        if(event==Event::Offline)Reset();
        else if(event==Event::Battery || event==Event::Power){online=true;deadline=0;}
        return false;
    }
    void ConfirmOnline(){online=true;}
    void Reset(){online=false;deadline=0;}
    bool Waiting() const{return deadline!=0;}
    uint64_t Deadline() const{return deadline?deadline:UINT64_MAX;}
    bool Due(uint64_t now) const{return deadline && now>=deadline;}
    bool TakeExpired(uint64_t now){if(!Due(now))return false;deadline=0;return true;}
};
inline Event ParseInput(const uint8_t* bytes,size_t length) {
    if(length!=8||bytes[0]!=9||bytes[1]!=10||bytes[5]||bytes[6]||bytes[7])return Event::None;
    if(bytes[2]==2&&bytes[4]==0){if(bytes[3]==1)return Event::Online;if(bytes[3]==0)return Event::Offline;}
    if(bytes[2]==5){if(bytes[3]<=100)return Event::Battery;if(bytes[3]==255)return Event::Power;}
    return Event::None;
}
}
