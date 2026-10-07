// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Windows x64, dynamically loaded flat Steam API; no VR runtime or Home code.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <algorithm>
using U64=uint64_t; using U32=uint32_t;
// Windows pack-8 layouts from Valve's public isteamugc.h / isteamremotestorage.h.
#pragma pack(push,8)
struct Details {
 U64 id; int result,type; U32 creator,consumer; char title[129],description[8000];
 U64 owner; U32 created,updated,added; int visibility; bool banned,accepted,truncated;
 char tags[1025]; U64 file,preview; char filename[260]; int file_size,preview_size;
 char url[256]; U32 votes_up,votes_down; float score; U32 children;
};
struct Completed { U64 handle; int result; U32 returned,total; bool cached; char cursor[256]; };
#pragma pack(pop)
static_assert(sizeof(Details)==9776,"Steam UGC details ABI");
static_assert(sizeof(Completed)==280,"Steam UGC callback ABI");
HMODULE api;
template<typename T> T sym(const char* name) {
 auto p=GetProcAddress(api,name); if(!p){printf("missing export %s\n",name);exit(2);}return reinterpret_cast<T>(p);
}
#define FN(name,ret,...) auto name=sym<ret(*)(__VA_ARGS__)>("SteamAPI_" #name)
std::map<U64,std::vector<U64>> graph;
std::map<U64,int> colour;
std::vector<U64> path;
bool cycle=false;
void walk(U64 id) {
 if(colour[id]==1){cycle=true;printf("CYCLE");auto start=std::find(path.begin(),path.end(),id);for(auto i=start;i!=path.end();++i)printf(" -> %llu",(unsigned long long)*i);printf(" -> %llu\n",(unsigned long long)id);return;}
 if(colour[id]==2)return;
 colour[id]=1;path.push_back(id);for(auto child:graph[id])walk(child);path.pop_back();colour[id]=2;
}
int main(int argc,char**argv) {
 setvbuf(stdout,nullptr,_IONBF,0);
 if(argc<2){puts("usage: workshop-probe.exe <absolute steam_api64.dll> [cache-age-seconds] [item IDs ...]");return 2;}
 U32 cache=argc>2?strtoul(argv[2],nullptr,10):0;
 std::vector<U64> pending;for(int i=3;i<argc;i++)pending.push_back(strtoull(argv[i],nullptr,10));
 if(pending.empty())pending={3149046643ULL,2289310332ULL};
 api=LoadLibraryA(argv[1]);if(!api){printf("LoadLibrary error=%lu\n",GetLastError());return 2;}
 FN(Init,bool);FN(Shutdown,void);FN(GetHSteamUser,int);FN(GetHSteamPipe,int);FN(RunCallbacks,void);
 FN(ISteamClient_GetISteamUtils,void*,void*,int,const char*);
 auto client=sym<void*(*)()>("SteamClient");
 FN(ISteamUtils_GetAppID,U32,void*);
 FN(ISteamUtils_IsAPICallCompleted,bool,void*,U64,bool*);
 FN(ISteamUtils_GetAPICallResult,bool,void*,U64,void*,int,int,bool*);
 FN(ISteamUtils_GetAPICallFailureReason,int,void*,U64);
 FN(ISteamUGC_CreateQueryUGCDetailsRequest,U64,void*,U64*,U32);
 FN(ISteamUGC_SetReturnChildren,bool,void*,U64,bool);
 FN(ISteamUGC_SetAllowCachedResponse,bool,void*,U64,U32);
 FN(ISteamUGC_SendQueryUGCRequest,U64,void*,U64);
 FN(ISteamUGC_GetQueryUGCResult,bool,void*,U64,U32,Details*);
 FN(ISteamUGC_GetQueryUGCChildren,bool,void*,U64,U32,U64*,U32);
 FN(ISteamUGC_ReleaseQueryUGCRequest,bool,void*,U64);
 FN(ISteamUGC_GetItemState,U32,void*,U64);
 FN(ISteamUGC_GetNumSubscribedItems,U32,void*);
 if(!Init()){puts("SteamAPI_Init failed: start Steam in this Wine prefix; set SteamAppId=250820");return 2;}
 auto find=sym<void*(*)(int,const char*)>("SteamInternal_FindOrCreateUserInterface");
 void* c=client();void*ugc=find(GetHSteamUser(),"STEAMUGC_INTERFACE_VERSION013");
 printf("client=%p user=%d pipe=%d ugc=%p\n",c,GetHSteamUser(),GetHSteamPipe(),ugc);
 void*utils=ISteamClient_GetISteamUtils(c,GetHSteamPipe(),"SteamUtils009");
 printf("utils=%p\n",utils);
 if(!ugc||!utils){puts("Home-matching interface unavailable");Shutdown();return 2;}
 printf("app=%u UGC=013 Utils=009 cache_age=%u subscribed=%u details_size=%zu callback_size=%zu\n",ISteamUtils_GetAppID(utils),cache,ISteamUGC_GetNumSubscribedItems(ugc),sizeof(Details),sizeof(Completed));
 bool error=false;
 while(!pending.empty()&&!error){
  U64 id=pending.back();pending.pop_back();if(graph.count(id))continue;
  if(graph.size()>=128){puts("graph limit exceeded");error=true;break;}
  auto start=GetTickCount64();U32 state=ISteamUGC_GetItemState(ugc,id);
  printf("STATE id=%llu flags=0x%x duration_ms=%llu\n",(unsigned long long)id,state,(unsigned long long)(GetTickCount64()-start));
  U64 q=ISteamUGC_CreateQueryUGCDetailsRequest(ugc,&id,1);
  bool children=ISteamUGC_SetReturnChildren(ugc,q,true),allow=ISteamUGC_SetAllowCachedResponse(ugc,q,cache);
  U64 call=ISteamUGC_SendQueryUGCRequest(ugc,q);
  printf("QUERY id=%llu handle=%llu call=%llu children_option=%d cache_option=%d\n",(unsigned long long)id,(unsigned long long)q,(unsigned long long)call,children,allow);
  bool failed=false,done=false;
  while(GetTickCount64()-start<30000){RunCallbacks();if(ISteamUtils_IsAPICallCompleted(utils,call,&failed)){done=true;break;}Sleep(10);}
  Completed result={};bool got=done&&ISteamUtils_GetAPICallResult(utils,call,&result,sizeof(result),3401,&failed);
  printf("COMPLETE done=%d got=%d io_failed=%d failure_reason=%d result=%d returned=%u cached=%d duration_ms=%llu\n",done,got,failed,failed?ISteamUtils_GetAPICallFailureReason(utils,call):-1,result.result,result.returned,result.cached,(unsigned long long)(GetTickCount64()-start));
  if(!got||failed||result.result!=1||result.returned!=1){error=true;ISteamUGC_ReleaseQueryUGCRequest(ugc,q);break;}
  Details d={};bool detail=ISteamUGC_GetQueryUGCResult(ugc,q,0,&d);d.title[128]=0;
  printf("DETAIL got=%d id=%llu result=%d title=\"%s\" creator=%u consumer=%u children=%u updated=%u\n",detail,(unsigned long long)d.id,d.result,d.title,d.creator,d.consumer,d.children,d.updated);
  if(!detail||d.id!=id||d.result!=1||d.children>4096){error=true;ISteamUGC_ReleaseQueryUGCRequest(ugc,q);break;}
  std::vector<U64> deps(d.children);bool deps_ok=deps.empty()||ISteamUGC_GetQueryUGCChildren(ugc,q,0,deps.data(),d.children);
  printf("CHILDREN got=%d",deps_ok);for(auto dep:deps)printf(" %llu",(unsigned long long)dep);puts("");
  if(!deps_ok)error=true;else{graph[id]=deps;for(auto dep:deps)pending.push_back(dep);}
  ISteamUGC_ReleaseQueryUGCRequest(ugc,q);
 }
 if(!error){for(auto&node:graph)walk(node.first);printf("SUMMARY nodes=%zu cycle=%d (Steam API completed without recursive calls)\n",graph.size(),cycle);}
 Shutdown();return error?2:0;
}
