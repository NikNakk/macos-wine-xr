#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "winemetal_fixed_psvr2_profile.h"
static double coord(id<MTLRasterizationRateMap> m,double v,int axis) {
 MTLCoordinate2D p=[m mapScreenToPhysicalCoordinates:MTLCoordinate2DMake(axis?0:v,axis?v:0) forLayer:0];
 return axis?p.y:p.x;
}
static double rate(id<MTLRasterizationRateMap> m,double v,int axis) {return (coord(m,v+1,axis)-coord(m,v-1,axis))/2;}
int main(){@autoreleasepool{
 id<MTLDevice> d=MTLCreateSystemDefaultDevice();if(!d)return 2;
 int failures=0;unsigned sizes[][2]={{5600,2856},{5120,1440},{1792,1008}};
 float rates[]={.3,.5,.75,1};
 for(unsigned s=0;s<3;s++)for(unsigned r=0;r<4;r++){
 unsigned w=sizes[s][0],h=sizes[s][1];float outer=rates[r];
 MTLRasterizationRateLayerDescriptor *l=[[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(64,32,1)];
 for(unsigned i=0;i<64;i++)l.horizontal[i]=@(wmt_fixed_psvr2_horizontal(i,outer));
 for(unsigned i=0;i<32;i++)l.vertical[i]=@(wmt_fixed_psvr2_vertical(i,outer));
 id<MTLRasterizationRateMap> m=[d newRasterizationRateMapWithDescriptor:[MTLRasterizationRateMapDescriptor rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(w,h,1) layer:l]];
 if(!m){failures++;continue;}MTLSize p=[m physicalSizeForLayer:0];
 double minCore=2,maxIdentityError=0,maxEdge=0;const double rad=M_PI/180.;
 for(int eye=0;eye<2;eye++)for(int a=-15;a<=15;a++){
 double left=tan((eye?-43.5:-61.5)*rad),right=tan((eye?61.5:43.5)*rad);
 double u=(tan(a*rad)-left)/(right-left),x=(eye+u)*w/2;
 minCore=fmin(minCore,rate(m,x,0));
 double y=(tan(a*rad)/tan(53*rad)+1)*h/2;
 minCore=fmin(minCore,rate(m,y,1));
 }
 for(int axis=0;axis<2;axis++){
 double n=axis?h:w,previous=-1;
 for(unsigned i=0;i<=1024;i++){
 double x=n*i/1024.,actual=coord(m,x,axis);
 if(actual<previous-.001)failures++;previous=actual;
 if(r==3)maxIdentityError=fmax(maxIdentityError,fabs(actual-x));
 }
 }
 for(int eye=0;eye<2;eye++)for(int side=0;side<2;side++) {
 double left=tan((eye?-43.5:-61.5)*rad),right=tan((eye?61.5:43.5)*rad);
 double angle=(side?40:-40)*rad,u=(tan(angle)-left)/(right-left);
 maxEdge=fmax(maxEdge,rate(m,(eye+u)*w/2,0));
 }
 for(int side=0;side<2;side++)maxEdge=fmax(maxEdge,rate(m,(tan((side?40:-40)*rad)/tan(53*rad)+1)*h/2,1));
 // Metal quantizes density to its physical tiles; smaller maps round upward.
 if(maxEdge>outer+.15 || (outer<1 && maxEdge>=.99))failures++;
 if(minCore<.99 || (r==3 && maxIdentityError>.02))failures++;
 printf("%ux%u outer=%.2f physical=%lux%lu protected_15deg_min_rate=%.5f edge_max_rate=%.5f identity_error=%.5f\n",w,h,outer,p.width,p.height,minCore,maxEdge,maxIdentityError);
 [m release];[l release];
 }
 printf("failures=%d\n",failures);return failures?1:0;
}}
