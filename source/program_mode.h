#pragma once
#include <windows.h>
#include <shellapi.h>
#include <cwchar>

namespace ProgramMode {
inline bool HasArgument(const wchar_t* argument) {
    int count=0;
    wchar_t** arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!arguments)return false;
    bool found=false;
    for(int index=1;index<count;++index)if(wcscmp(arguments[index],argument)==0){found=true;break;}
    LocalFree(arguments);
    return found;
}
}
