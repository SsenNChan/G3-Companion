#pragma once
#include "macro_model.h"

namespace MacroEditor {
struct LayoutState {
    std::size_t dirty=0,cursor=0,actions=0;
    std::uint64_t duration=0;
    double width=0;
    bool dragging=false;
    void Invalidate(std::size_t index=0){dirty=std::min(dirty,index);}
};

// Reflow starts at the first edit or changed cursor gap; measured prefixes stay reusable.
template<class Rect,class Measure>
double UpdateLayout(const std::vector<Step>& steps,std::size_t cursor,bool dragging,double width,
                    LayoutState& state,std::vector<Step>& measured,std::vector<double>& widths,
                    std::vector<Rect>& layout,Measure measure){
    const auto count=steps.size();
    if(measured.capacity()>std::max<std::size_t>(256,count*2)){
        std::vector<Step>().swap(measured);std::vector<double>().swap(widths);std::vector<Rect>().swap(layout);state={};
    }
    const auto previous=measured.size();
    auto first=std::min({state.dirty,previous,count});
    if(state.width!=width||layout.empty())first=0;
    if(state.cursor!=cursor||state.dragging!=dragging)first=std::min({first,state.cursor,cursor});
    const auto remove=[&](const Step& step){if(step.kind==StepKind::Delay)state.duration-=step.value;else --state.actions;};
    for(auto index=count;index<previous;++index)remove(measured[index]);
    measured.resize(count);widths.resize(count);layout.resize(count*2+1);
    double x=0,y=0;
    if(first){const auto& before=layout[first*2-1];x=before.X+before.Width;y=before.Y;}
    const auto place=[&](std::size_t key,double desired){
        const auto size=std::min(width,desired);if(x>0&&x+size>width){x=0;y+=38;}
        layout[key]={static_cast<float>(x),static_cast<float>(y),static_cast<float>(size),32};x+=size;
    };
    for(auto index=first;index<=count;++index){
        const bool left=index>0,right=index<count,active=index==cursor;
        double gap=active?(left&&right?28:left||right?20:14):14;if(dragging&&active)gap=72;place(index*2,gap);
        if(index==count)break;
        const auto& step=steps[index];
        if(index>=previous||!(step==measured[index])){
            if(index<previous)remove(measured[index]);
            if(step.kind==StepKind::Delay)state.duration+=step.value;else ++state.actions;
            widths[index]=measure(step);measured[index]=step;
        }
        place(index*2+1,widths[index]);
    }
    state.dirty=count;state.cursor=cursor;state.dragging=dragging;state.width=width;
    return layout.back().Y+32;
}
}
