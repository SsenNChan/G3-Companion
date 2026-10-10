#pragma once
#include <windows.h>
#include <charconv>

namespace Format {
// The caller provides enough space; each function returns the end without a terminator.
template<class Char>
inline Char* Number(Char* out,unsigned long value,unsigned width=0) {
    char digits[10];
    const char* end=std::to_chars(digits,digits+sizeof(digits),value).ptr;
    for(unsigned count=static_cast<unsigned>(end-digits);count<width;++count)*out++=static_cast<Char>('0');
    for(const char* digit=digits;digit!=end;++digit)*out++=static_cast<Char>(*digit);
    return out;
}

template<class Char>
inline Char* Date(Char* out,const SYSTEMTIME& time) {
    out=Number(out,time.wYear,4);*out++=static_cast<Char>('-');
    out=Number(out,time.wMonth,2);*out++=static_cast<Char>('-');
    return Number(out,time.wDay,2);
}

template<class Char>
inline Char* Time(Char* out,const SYSTEMTIME& time,bool seconds=false) {
    out=Number(out,time.wHour,2);*out++=static_cast<Char>(':');
    out=Number(out,time.wMinute,2);
    if(seconds){*out++=static_cast<Char>(':');out=Number(out,time.wSecond,2);}
    return out;
}
}
