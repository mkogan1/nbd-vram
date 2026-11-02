/* No block devices or swap; run only with an explicitly supplied bridge .so. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test-cuda-bridge-protocol.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define API(name,args) int (*name) args = dlsym(lib,#name); CHECK(name)
int main(int argc,char **argv)
{
 CHECK(argc==2);
 void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); CHECK(lib);
 API(cuInit,(unsigned)); API(cuDeviceGet,(int *,int));
 API(cuCtxCreate_v2,(void **,unsigned,int)); API(cuCtxSetCurrent,(void *));
 API(cuCtxDestroy_v2,(void *)); API(cuMemAlloc_v2,(uint64_t *,size_t));
 API(cuMemFree_v2,(uint64_t)); API(cuMemAllocHost_v2,(void **,size_t));
 API(cuMemFreeHost,(void *)); API(cuStreamCreate,(void **,unsigned));
 API(cuStreamSynchronize,(void *)); API(cuStreamDestroy_v2,(void *));
 API(cuMemcpyHtoDAsync_v2,(uint64_t,const void *,size_t,void *));
 API(cuMemcpyDtoHAsync_v2,(void *,uint64_t,size_t,void *));
 API(cuMemGetInfo_v2,(size_t *,size_t *));
 int device=-1; void *ctx=NULL,*stream=NULL,*host=NULL;
 uint64_t gpu=0,extra=0; size_t available=0,total=0;
 CHECK(!cuInit(0)); CHECK(!cuDeviceGet(&device,0));
 CHECK(!cuCtxCreate_v2(&ctx,4,device)); CHECK(!cuCtxSetCurrent(ctx));
 CHECK(!cuMemGetInfo_v2(&available,&total)); CHECK(available<=total&&total);
 CHECK(!cuMemAlloc_v2(&gpu,CB_GPU_LIMIT)); CHECK(gpu==CB_DEVICE_BASE);
 CHECK(cuMemAlloc_v2(&extra,1)!=0&&extra==0);
 CHECK(!cuMemAllocHost_v2(&host,CB_MAX_DATA)); CHECK(!cuStreamCreate(&stream,1));
 unsigned char *bytes=host;
 for(size_t offset=0;offset<CB_GPU_LIMIT;offset+=CB_MAX_DATA) {
  for(size_t i=0;i<CB_MAX_DATA;++i) bytes[i]=(unsigned char)((offset/4096+i*17+i/251)%256);
  CHECK(!cuMemcpyHtoDAsync_v2(gpu+offset,host,CB_MAX_DATA,stream));
 }
 CHECK(!cuStreamSynchronize(stream));
 for(size_t offset=0;offset<CB_GPU_LIMIT;offset+=CB_MAX_DATA) {
  memset(host,0,CB_MAX_DATA);
  CHECK(!cuMemcpyDtoHAsync_v2(host,gpu+offset,CB_MAX_DATA,stream));
  for(size_t i=0;i<CB_MAX_DATA;++i) CHECK(bytes[i]==(unsigned char)((offset/4096+i*17+i/251)%256));
 }
 CHECK(cuMemcpyHtoDAsync_v2(gpu+CB_GPU_LIMIT-1,host,2,stream)!=0);
 CHECK(cuMemFree_v2(gpu+1)!=0);
 CHECK(!cuStreamSynchronize(stream)); CHECK(!cuStreamDestroy_v2(stream));
 CHECK(!cuMemFreeHost(host)); CHECK(!cuMemFree_v2(gpu));
 CHECK(!cuCtxDestroy_v2(ctx)); CHECK(!dlclose(lib));
 puts("CUDA_BRIDGE_ROUNDTRIP_PASS payload=2097152 verified_bytes=2097152 blocks=0 swap=0");
 return 0;
}
