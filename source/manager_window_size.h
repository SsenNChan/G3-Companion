#pragma once
#include <windows.h>
#include <algorithm>
#include <limits>
#include <string>

namespace ManagerWindowSize {
struct State {
    std::wstring settingsPath;
    int widthDip=960, heightDip=740;
    bool enabled=false, changed=false;
};

inline void Load(State& state) {
    const UINT width=GetPrivateProfileIntW(L"ManagerWindow",L"WidthDip",0,state.settingsPath.c_str());
    const UINT height=GetPrivateProfileIntW(L"ManagerWindow",L"HeightDip",0,state.settingsPath.c_str());
    if(width>=640 && height>=480 && width<=static_cast<UINT>((std::numeric_limits<int>::max)()) &&
       height<=static_cast<UINT>((std::numeric_limits<int>::max)())) {
        state.widthDip=static_cast<int>(width);state.heightDip=static_cast<int>(height);
    }
}

inline SIZE FitWorkArea(State const& state,RECT const& workArea,UINT dpi) {
    const int margin=MulDiv(32,static_cast<int>(dpi),96);
    const int availableWidth=(std::max)(1,static_cast<int>(workArea.right-workArea.left)-margin);
    const int availableHeight=(std::max)(1,static_cast<int>(workArea.bottom-workArea.top)-margin);
    const int widthDip=(std::min)((std::max)(640,state.widthDip),MulDiv(availableWidth,96,static_cast<int>(dpi)));
    const int heightDip=(std::min)((std::max)(480,state.heightDip),MulDiv(availableHeight,96,static_cast<int>(dpi)));
    return {(std::max)(1,MulDiv(widthDip,static_cast<int>(dpi),96)),
            (std::max)(1,MulDiv(heightDip,static_cast<int>(dpi),96))};
}

inline void Remember(State& state,SIZE pixels,UINT dpi) {
    if(!state.enabled || !dpi)return;
    const int width=MulDiv(pixels.cx,96,static_cast<int>(dpi));
    const int height=MulDiv(pixels.cy,96,static_cast<int>(dpi));
    if(width<640 || height<480 || (width==state.widthDip && height==state.heightDip))return;
    state.widthDip=width;state.heightDip=height;state.changed=true;
}

inline bool Save(State& state) {
    if(!state.enabled || !state.changed)return true;
    if(!WritePrivateProfileStringW(L"ManagerWindow",L"WidthDip",std::to_wstring(state.widthDip).c_str(),state.settingsPath.c_str()) ||
       !WritePrivateProfileStringW(L"ManagerWindow",L"HeightDip",std::to_wstring(state.heightDip).c_str(),state.settingsPath.c_str()))return false;
    state.changed=false;return true;
}
}
