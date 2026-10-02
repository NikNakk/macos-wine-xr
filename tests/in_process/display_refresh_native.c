// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Standalone macOS diagnostic: compare display-link timing outside Wine.
#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <unistd.h>
#include <stdatomic.h>
struct observations { atomic_uint count; atomic_ullong first,last; };
static CVReturn callback(CVDisplayLinkRef link,const CVTimeStamp *now,const CVTimeStamp *out,CVOptionFlags in,CVOptionFlags *flags,void *context)
{
 struct observations *p=context; unsigned n=atomic_fetch_add(&p->count,1);
 unsigned long long t=CVGetCurrentHostTime(); if (!n)atomic_store(&p->first,t);atomic_store(&p->last,t);
 return kCVReturnSuccess;
}
int main(void)
{
 CGDirectDisplayID ids[16];uint32_t count=0;CGGetOnlineDisplayList(16,ids,&count);
 for (unsigned i=0;i<count;++i) {
  CGDisplayModeRef mode=CGDisplayCopyDisplayMode(ids[i]);double hz=mode?CGDisplayModeGetRefreshRate(mode):0;if(mode)CGDisplayModeRelease(mode);
  printf("display=%u size=%zux%zu mode_hz=%.3f\n",ids[i],CGDisplayPixelsWide(ids[i]),CGDisplayPixelsHigh(ids[i]),hz);
  if (CGDisplayPixelsWide(ids[i])!=4000)continue;
  CVDisplayLinkRef link=NULL;CVReturn r=CVDisplayLinkCreateWithCGDisplay(ids[i],&link);if(r||!link){printf("create_error=%d\n",r);continue;}
  CVTime nominal=CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
  printf("nominal=%lld/%d flags=%d hz=%.3f\n",(long long)nominal.timeValue,nominal.timeScale,nominal.flags,(double)nominal.timeScale/nominal.timeValue);
  struct observations p={0};CVDisplayLinkSetOutputCallback(link,callback,&p);CVDisplayLinkStart(link);usleep(1200000);CVDisplayLinkStop(link);
  unsigned n=atomic_load(&p.count);printf("callbacks=%u measured_hz=%.3f actual_period_s=%.9f\n",n,n>1?(n-1)*CVGetHostClockFrequency()/(atomic_load(&p.last)-atomic_load(&p.first)):0,CVDisplayLinkGetActualOutputVideoRefreshPeriod(link));CVDisplayLinkRelease(link);
 }
 return 0;
}
