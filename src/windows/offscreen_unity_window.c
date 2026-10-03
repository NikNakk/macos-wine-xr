// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
static DWORD game_pid;
static HANDLE game_process;
static BOOL WINAPI stop(DWORD type) {
 if (type==CTRL_C_EVENT || type==CTRL_BREAK_EVENT || type==CTRL_CLOSE_EVENT) { if(game_process) TerminateProcess(game_process,130); return TRUE; }
 return FALSE;
}
static BOOL CALLBACK hide(HWND window, LPARAM unused) {
 DWORD pid; wchar_t cls[128]; GetWindowThreadProcessId(window,&pid);
 if(pid!=game_pid || !IsWindowVisible(window)) return TRUE;
 GetClassNameW(window,cls,128);
 if(wcscmp(cls,L"UnityWndClass")==0) {
  RECT rect;GetWindowRect(window,&rect);
  if(rect.left > -16000) {
   SetWindowPos(window,NULL,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
   SetForegroundWindow(window);
   fprintf(stderr,"mwxr: moved Unity desktop window off-screen hwnd=%p pid=%lu\n",window,pid);
  }
 }
 return TRUE;
}
// Quote each argument using CommandLineToArgvW/CRT backslash rules.
static wchar_t *quote(wchar_t *out,const wchar_t *arg) {
 *out++=L'"';
 while(*arg) {
  unsigned n=0;while(*arg==L'\\'){n++;arg++;}
  if(*arg==L'"'){for(unsigned i=0;i<2*n+1;i++)*out++=L'\\';*out++=*arg++;}
  else if(!*arg){for(unsigned i=0;i<2*n;i++)*out++=L'\\';break;}
  else{for(unsigned i=0;i<n;i++)*out++=L'\\';*out++=*arg++;}
 }
 *out++=L'"';return out;
}
int wmain(int argc,wchar_t **argv) {
 if(argc<2){fprintf(stderr,"usage: offscreen_unity_window.exe game.exe [arguments]\n");return 2;}
 size_t needed=1;for(int i=1;i<argc;i++)needed+=2*wcslen(argv[i])+4;
 if(needed>32767){fprintf(stderr,"command line too long\n");return 2;}
 wchar_t *cmd=HeapAlloc(GetProcessHeap(),0,needed*sizeof(wchar_t));if(!cmd)return 2;
 wchar_t *end=cmd;for(int i=1;i<argc;i++){if(i>1)*end++=L' ';end=quote(end,argv[i]);}*end=0;
 HANDLE job=CreateJobObjectW(NULL,NULL);
 JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
 limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
 if(!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) {
  fprintf(stderr,"Unable to configure child lifetime: %lu\n",GetLastError());
  if(job)CloseHandle(job);HeapFree(GetProcessHeap(),0,cmd);return 3;
 }
 STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION pi={0};
 if(!CreateProcessW(argv[1],cmd,NULL,NULL,TRUE,CREATE_SUSPENDED,NULL,NULL,&si,&pi)){fprintf(stderr,"CreateProcess failed: %lu\n",GetLastError());HeapFree(GetProcessHeap(),0,cmd);CloseHandle(job);return 3;}
 HeapFree(GetProcessHeap(),0,cmd);
 if(!AssignProcessToJobObject(job,pi.hProcess) || ResumeThread(pi.hThread)==(DWORD)-1) {
  fprintf(stderr,"Unable to start managed game: %lu\n",GetLastError());
  TerminateProcess(pi.hProcess,3);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(job);return 3;
 }
 CloseHandle(pi.hThread);game_pid=pi.dwProcessId;game_process=pi.hProcess;SetConsoleCtrlHandler(stop,TRUE);
 fprintf(stderr,"mwxr: off-screen desktop mirror enabled for game pid=%lu; Ctrl+C stops the game\n",game_pid);
 while(WaitForSingleObject(game_process,100)==WAIT_TIMEOUT)EnumWindows(hide,0);
 DWORD code=1;GetExitCodeProcess(game_process,&code);CloseHandle(game_process);CloseHandle(job);return (int)code;
}
