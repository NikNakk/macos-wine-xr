// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// x64 Windows debugger workaround for one verified SteamVR Home build.
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
struct Frame { DWORD64 sp,caller,self,item; };
static HANDLE process;
static DWORD pid;
static DWORD64 entry,gate;
static unsigned long long calls,cycles,returns;
static std::map<DWORD,HANDLE> threads;
static std::map<DWORD,std::vector<Frame>> paths;
static volatile LONG stop_requested;
static bool armed=false,failed=false;
static const BYTE prologue[]={0x44,0x89,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10};
#ifdef MWXR_GUARD_FIXTURE_HASH
static const char* target_exe="guard-fixture.exe";
static const char* target_dll="guard_fixture.dll";
static const DWORD64 target_offset=MWXR_GUARD_FIXTURE_RVA;
static const char* expected_hash=MWXR_GUARD_FIXTURE_HASH;
#else
static const char* target_exe="steamtours.exe";
static const char* target_dll="client.dll";
static const DWORD64 target_offset=0x4f14f0;
static const char* expected_hash="c331ca4fc37723dd496d06930049a5f847433aa169b70943a4511b080fa16f96";
#endif
static BOOL WINAPI stop_handler(DWORD event) { if(event==CTRL_C_EVENT||event==CTRL_BREAK_EVENT){InterlockedExchange(&stop_requested,1);return TRUE;}return FALSE; }
static bool read_memory(DWORD64 address,void* data,SIZE_T size) { SIZE_T done=0;return ReadProcessMemory(process,(void*)address,data,size,&done)&&done==size; }
static bool write_memory(DWORD64 address,const void* data,SIZE_T size,bool code=false) {
 DWORD old=0,unused=0;SIZE_T done=0;
 if(code&&!VirtualProtectEx(process,(void*)address,size,PAGE_EXECUTE_READWRITE,&old))return false;
 bool ok=WriteProcessMemory(process,(void*)address,data,size,&done)&&done==size;
 if(code){ok=VirtualProtectEx(process,(void*)address,size,old,&unused)&&ok;ok=FlushInstructionCache(process,(void*)address,size)&&ok;}return ok;
}
static std::string sha256(const char* filename) {
 HANDLE f=CreateFileA(filename,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
 if(f==INVALID_HANDLE_VALUE)return {};
 BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;BYTE digest[32];bool ok=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
 if(ok)ok=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0;
 BYTE buffer[65536];DWORD count=0;
 while(ok){if(!ReadFile(f,buffer,sizeof(buffer),&count,nullptr)){ok=false;break;}if(!count)break;ok=BCryptHashData(hash,buffer,count,0)>=0;}
 if(ok)ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
 if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);CloseHandle(f);
 std::string result;if(ok){char hex[3];for(BYTE b:digest){snprintf(hex,sizeof(hex),"%02x",b);result+=hex;}}return result;
}
static DWORD find_home() {
 HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32 p={};p.dwSize=sizeof(p);DWORD found=0;
 if(s!=INVALID_HANDLE_VALUE){if(Process32First(s,&p))do{if(!_stricmp(p.szExeFile,target_exe)){if(found){found=0;break;}found=p.th32ProcessID;}}while(Process32Next(s,&p));CloseHandle(s);}return found;
}
static bool arm() {
 if(armed)return true;
 HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);MODULEENTRY32 m={};m.dwSize=sizeof(m);
 if(s==INVALID_HANDLE_VALUE)return true;
 bool ok=true;
 if(Module32First(s,&m))do{if(_stricmp(m.szModule,target_dll))continue;
  std::string actual=sha256(m.szExePath);printf("HOME_DLL sha256=%s path=%s\n",actual.c_str(),m.szExePath);
  if(actual!=expected_hash){puts("REFUSED: unsupported Home DLL; no patch applied");ok=false;break;}
  DWORD64 candidate=(DWORD64)m.modBaseAddr+target_offset;BYTE original[sizeof(prologue)];
  if(!read_memory(candidate,original,sizeof(original))||memcmp(original,prologue,sizeof(original))){puts("REFUSED: unexpected function prologue");ok=false;break;}
  gate=(DWORD64)VirtualAllocEx(process,nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
  BYTE trap=0xcc;
  if(!gate||!write_memory(gate,&trap,1,true)){puts("GATE ALLOCATION FAILED");ok=false;break;}
  entry=candidate;armed=true;
  if(!write_memory(entry,&trap,1,true)){puts("ENTRY PATCH FAILED");ok=false;break;}printf("ARMED pid=%lu entry=%llx return_gate=%llx\n",pid,(unsigned long long)entry,(unsigned long long)gate);break;
 }while(Module32Next(s,&m));CloseHandle(s);return ok;
}
// Called while a debug event holds all target threads stopped. Revert every
// outstanding substituted return slot before detaching; keep the unused page
// allocated until the process exits to avoid racing a remote instruction fetch.
static bool restore() {
 bool ok=true;
 if(armed)ok=write_memory(entry,prologue,1,true);
 for(auto& thread:paths)for(auto&f:thread.second){DWORD64 current=0;if(read_memory(f.sp,&current,sizeof(current))&&current==gate)ok=write_memory(f.sp,&f.caller,sizeof(f.caller))&&ok;}
 if(ok){paths.clear();armed=false;puts("RESTORED entry and active return addresses");}return ok;
}
static bool handle_trap(DWORD tid,DWORD64 address) {
 auto it=threads.find(tid);if(it==threads.end())return false;
 CONTEXT c={};c.ContextFlags=CONTEXT_FULL;if(!GetThreadContext(it->second,&c)){failed=true;return false;}
 auto& path=paths[tid];
 if(address==gate){
  if(path.empty()||path.back().sp+8!=c.Rsp){puts("RETURN tracking mismatch; stopping guard");failed=true;return false;}
  c.Rip=path.back().caller;path.pop_back();++returns;
  if(!SetThreadContext(it->second,&c))failed=true;return true;
 }
 if(address!=entry)return false;
 ++calls;
 // Prune calls unwound by an exception or longjmp before reaching our gate.
 while(!path.empty()&&path.back().sp<=c.Rsp)path.pop_back();
 DWORD64 caller=0;if(!read_memory(c.Rsp,&caller,8)){failed=true;return false;}
 auto repeat=std::find_if(path.begin(),path.end(),[&](const Frame&f){return f.self==c.Rcx&&f.item==c.R8;});
 if(repeat!=path.end()||path.size()>=128){
  ++cycles;
  if(cycles<=20){printf("BLOCKED tid=%lu reason=%s path",tid,repeat!=path.end()?"cycle":"depth-limit");for(auto&f:path)printf(" -> %llu",(unsigned long long)f.item);printf(" -> %llu\n",(unsigned long long)c.R8);}
  DWORD unavailable=0;if(!write_memory(c.Rdx,&unavailable,4)){failed=true;return false;}
  c.Rax=0;c.Rip=caller;c.Rsp+=8; // Return unavailable; do not claim installed.
 }else{
  // Emulate the complete first instruction (mov [rsp+20h],r9d) so the
  // entry INT3 stays armed for other threads; no single-step race window.
  DWORD mode=(DWORD)c.R9;
  if(!write_memory(c.Rsp+0x20,&mode,4)||!write_memory(c.Rsp,&gate,8)){failed=true;return false;}
  path.push_back({c.Rsp,caller,c.Rcx,c.R8});c.Rip=entry+5;
 }
 if(!SetThreadContext(it->second,&c))failed=true;return true;
}
int main(int argc,char**argv) {
 setvbuf(stdout,nullptr,_IONBF,0);SetConsoleCtrlHandler(stop_handler,TRUE);
 DWORD seconds=0;if(argc>1)pid=strtoul(argv[1],nullptr,0);if(argc>2)seconds=strtoul(argv[2],nullptr,10);
 auto start=GetTickCount64();while(!pid&&GetTickCount64()-start<90000){pid=find_home();if(!pid)Sleep(100);}
 if(!pid||!DebugActiveProcess(pid)){printf("ATTACH failed pid=%lu error=%lu\n",pid,GetLastError());return 2;}
 DebugSetProcessKillOnExit(FALSE);printf("ATTACHED pid=%lu; keep guard running until Home exits or press Ctrl+C\n",pid);
 bool exited=false,break_requested=false,initial_break=true,probe_break_requested=false;auto attached=GetTickCount64(),report=attached,next_probe=attached;
 while(!exited){
  if(seconds&&GetTickCount64()-attached>=seconds*1000ULL)InterlockedExchange(&stop_requested,1);
  if(stop_requested&&!break_requested&&process){if(DebugBreakProcess(process))break_requested=true;else{printf("DEBUG_BREAK failed %lu\n",GetLastError());failed=true;}}
  if(!armed&&!break_requested&&!probe_break_requested&&process&&GetTickCount64()>=next_probe){
   if(DebugBreakProcess(process))probe_break_requested=true;next_probe=GetTickCount64()+1000;
  }
  DEBUG_EVENT e={};if(!WaitForDebugEvent(&e,200))continue;
  DWORD cont=DBG_CONTINUE;
  switch(e.dwDebugEventCode){
   case CREATE_PROCESS_DEBUG_EVENT: process=e.u.CreateProcessInfo.hProcess;threads[e.dwThreadId]=e.u.CreateProcessInfo.hThread;if(e.u.CreateProcessInfo.hFile)CloseHandle(e.u.CreateProcessInfo.hFile);if(!arm())failed=true;break;
   case CREATE_THREAD_DEBUG_EVENT:threads[e.dwThreadId]=e.u.CreateThread.hThread;break;
   case LOAD_DLL_DEBUG_EVENT:if(e.u.LoadDll.hFile)CloseHandle(e.u.LoadDll.hFile);if(!arm())failed=true;break;
   case EXIT_THREAD_DEBUG_EVENT:if(threads.count(e.dwThreadId)){CloseHandle(threads[e.dwThreadId]);threads.erase(e.dwThreadId);}paths.erase(e.dwThreadId);break;
   case EXIT_PROCESS_DEBUG_EVENT:printf("HOME_EXIT code=%lu\n",e.u.ExitProcess.dwExitCode);exited=true;break;
   case EXCEPTION_DEBUG_EVENT:{if(!armed&&!arm())failed=true;auto&x=e.u.Exception;DWORD code=x.ExceptionRecord.ExceptionCode;
    if(code==EXCEPTION_BREAKPOINT){
     DWORD64 address=(DWORD64)x.ExceptionRecord.ExceptionAddress;
     if(armed&&(address==entry||address==gate))handle_trap(e.dwThreadId,address);
     else if(initial_break)initial_break=false;
     else if(probe_break_requested)probe_break_requested=false;
     else if(!break_requested)cont=DBG_EXCEPTION_NOT_HANDLED;
    }
    else {cont=DBG_EXCEPTION_NOT_HANDLED;if(code==EXCEPTION_STACK_OVERFLOW||code==EXCEPTION_ACCESS_VIOLATION)printf("EXCEPTION tid=%lu code=%08lx first_chance=%lu\n",e.dwThreadId,code,x.dwFirstChance);}
    break;}
  }
  if(GetTickCount64()-report>=10000){printf("STATUS calls=%llu blocked=%llu returns=%llu threads=%zu\n",calls,cycles,returns,paths.size());report=GetTickCount64();}
  bool detach=!exited&&(failed||stop_requested);
  if(detach&&!restore()){puts("RESTORE failed; remaining attached for safe retry (stop Home to exit)");failed=true;detach=false;}
  if(!ContinueDebugEvent(e.dwProcessId,e.dwThreadId,cont)){printf("CONTINUE failed %lu\n",GetLastError());failed=true;break;}
  if(detach){BOOL detached=DebugActiveProcessStop(pid);printf("DETACHED success=%d\n",detached);if(!detached)failed=true;break;}
 }
 printf("SUMMARY calls=%llu blocked=%llu returns=%llu failed=%d\n",calls,cycles,returns,failed);
 for(auto&t:threads)CloseHandle(t.second);if(process)CloseHandle(process);return failed?2:0;
}
