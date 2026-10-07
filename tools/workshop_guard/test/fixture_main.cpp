// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include <windows.h>
#include <cstdio>
#include <cstdint>
using Work=DWORD(*)(void*,DWORD*,uint64_t,DWORD,bool);
static Work work;
static volatile LONG completed=0;
static DWORD WINAPI thread(void* manager) {
 for(int i=0;i<8;i++){DWORD state=999;DWORD result=work(manager,&state,1,0,false);if(result!=4||state!=4)return 1;result=work(manager,&state,10,0,false);if(result!=0||state!=0)return 2;InterlockedIncrement(&completed);}return 0;
}
int main(int argc,char**) {
 setvbuf(stdout,nullptr,_IONBF,0);
 auto dll=LoadLibraryA("guard_fixture.dll");if(!dll)return 2;
 work=reinterpret_cast<Work>(GetProcAddress(dll,"WorkItem"));if(!work)return 2;
 puts("FIXTURE loaded; allowing debugger 5 seconds to attach");Sleep(5000);
 DWORD state=999;int manager=0;
 if(argc>1){DWORD result=work(&manager,&state,50,0,false);if(result!=4||state!=4)return 7;result=work(&manager,&state,1,0,false);if(result!=4||state!=4)return 8;puts("RESTORE_FIXTURE PASS");return 0;}
 DWORD result=work(&manager,&state,1,0,false);printf("SHARED_DAG result=%lu state=%lu\n",result,state);if(result!=4||state!=4)return 3;
 result=work(&manager,&state,10,0,false);printf("CYCLE result=%lu state=%lu\n",result,state);if(result!=0||state!=0)return 4;
 result=work(&manager,&state,100,0,false);printf("DEEP_GRAPH result=%lu state=%lu\n",result,state);if(result!=0||state!=0)return 5;
 HANDLE handles[4];for(int i=0;i<4;i++)handles[i]=CreateThread(nullptr,0,thread,&manager,0,nullptr);
 DWORD waited=WaitForMultipleObjects(4,handles,TRUE,30000);bool ok=waited==WAIT_OBJECT_0;for(auto h:handles){DWORD code=999;GetExitCodeThread(h,&code);printf("THREAD result=%lu\n",code);ok&=code==0;CloseHandle(h);}
 printf("COMPLETED %ld\n",completed);ok&=completed==32;puts(ok?"FIXTURE PASS":"FIXTURE FAIL");return ok?0:6;
}
