// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include <windows.h>
#include <cstdint>
using ID=uint64_t;
extern "C" __declspec(dllexport) DWORD WorkItem(void*,DWORD*,ID,DWORD,bool);
extern "C" DWORD Body(void* manager,DWORD* state,ID id,DWORD mode,bool high) {
 DWORD result=4;DWORD child=4;
 if(id==50)Sleep(5000);
 if(id==1){WorkItem(manager,&child,2,mode,high);result&=child;WorkItem(manager,&child,3,mode,high);result&=child;}
 else if(id==2||id==3){WorkItem(manager,&child,4,mode,high);result&=child;}
 else if(id==10||id==11){WorkItem(manager,&child,id==10?11:10,mode,high);result&=child;}
 else if(id>=100&&id<240){WorkItem(manager,&child,id+1,mode,high);result&=child;}
 *state=result;return result;
}
// The verified Home entry instruction sequence, followed by our own fixture
// body. This tests actual entry/return interception rather than graph mocks.
extern "C" __declspec(dllexport) __attribute__((naked)) DWORD WorkItem(void*,DWORD*,ID,DWORD,bool) {
 __asm__ volatile("mov %r9d,0x20(%rsp)\nmov %r8,0x18(%rsp)\nmov %rdx,0x10(%rsp)\njmp Body\n");
}
