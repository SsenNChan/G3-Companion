#pragma once
#include "../localization.h"

namespace Localization {
inline void AddXamlResources(winrt::Microsoft::UI::Xaml::ResourceDictionary const& resources) {
    for(auto id:XamlStrings){
        resources.Insert(winrt::box_value(winrt::hstring(Key(id))),winrt::box_value(winrt::hstring(Get(id))));
    }
    resources.Insert(winrt::box_value(L"T020_10"),
        winrt::box_value(winrt::hstring(Format(T020,{{L"minutes",L"10"}}))));

}
}
