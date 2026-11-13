// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// CPU-only diagnostic. Original game bytecode/disassembly stays in private builds.
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
static int disassemble(const char *path,bool save){
 auto file=std::fopen(path,"rb");if(!file)return 3;
 std::fseek(file,0,SEEK_END);long bytes=std::ftell(file);std::rewind(file);
 if(bytes<32||bytes>1024*1024){std::fclose(file);return 4;}
 std::vector<unsigned char> data(bytes);
 bool read=std::fread(data.data(),1,data.size(),file)==data.size();std::fclose(file);if(!read)return 5;
 ID3DBlob *assembly=nullptr;
 HRESULT result=D3DDisassemble(data.data(),data.size(),D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING,nullptr,&assembly);
 if(FAILED(result)){fprintf(stderr,"D3DDisassemble failed: %08lx\n",static_cast<unsigned long>(result));return 6;}
 std::string output=std::string(path)+".asm";FILE *dst=save?std::fopen(output.c_str(),"wb"):stdout;
 if(!dst){assembly->Release();return 7;}
 bool written=std::fwrite(assembly->GetBufferPointer(),1,assembly->GetBufferSize(),dst)==assembly->GetBufferSize();
 if(save)written=(std::fclose(dst)==0)&&written;assembly->Release();return written?0:8;
}
int main(int argc,char **argv){
 if(argc==2)return disassemble(argv[1],false);
 if(argc!=3||strcmp(argv[1],"--directory"))return 2;
 std::string root=argv[2],glob=root+"\\*.dxbc";WIN32_FIND_DATAA entry={};HANDLE find=FindFirstFileA(glob.c_str(),&entry);if(find==INVALID_HANDLE_VALUE)return 9;
 unsigned count=0,failed=0;
 do {if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
  int result=disassemble((root+"\\"+entry.cFileName).c_str(),true);++count;failed+=result!=0;
  if(result)fprintf(stderr,"disassembly failed: %s status=%d\n",entry.cFileName,result);
 }while(FindNextFileA(find,&entry));FindClose(find);
 printf("DXBC_DISASSEMBLE files=%u failed=%u\n",count,failed);return failed?1:0;
}
