/* CPU-only regression for VMM physical-block release and rearm. */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <stdarg.h>
static ssize_t fixture_pwrite(int, const void *, size_t, off_t);
static ssize_t fixture_pread(int, void *, size_t, off_t);
static int fixture_fsync(int);
static int fixture_prctl(int, ...);
int main(int, char **) __asm__("unused_ublk_target_main");
#define pwrite fixture_pwrite
#define pread fixture_pread
#define fsync fixture_fsync
#define prctl fixture_prctl
#include "ublk-lzo-target.c"
#undef pwrite
#undef pread
#undef fsync
#undef prctl

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); _Exit(90); } } while (0)
int ublksrv_queue_send_event(const struct ublksrv_queue *q) { (void)q; CHECK(0); return -1; }
enum { BACK_FD=123456, PAGES=128, BLOCK=2*1024*1024, VRAM=2*BLOCK,
       MARGIN=64*1024*1024, BACKING=2*1024*1024 };
static unsigned char backing[BACKING], expected[PAGES][COMP_PAGE], replacement[COMP_PAGE];
static unsigned char *vram;
static CUdeviceptr fake_base;
static size_t mem_free=MARGIN, mem_total=8*MARGIN;
static unsigned commits[2], releases[2];
static pthread_mutex_t meminfo_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t meminfo_cond=PTHREAD_COND_INITIALIZER;
static int meminfo_barrier, meminfo_entered, meminfo_release;
static struct timespec deadline(void)
{ struct timespec t; CHECK(clock_gettime(CLOCK_REALTIME,&t)==0); t.tv_sec+=10; return t; }
static void wait_state(int state)
{
    struct timespec until=deadline(); pthread_mutex_lock(&workq.lock);
    while ((int)workq.drain_state!=state) CHECK(pthread_cond_timedwait(&workq.cond,&workq.lock,&until)==0);
    pthread_mutex_unlock(&workq.lock);
}
static void wait_flag(int *flag)
{
    struct timespec until=deadline(); pthread_mutex_lock(&workq.lock);
    while (!*flag) CHECK(pthread_cond_timedwait(&workq.cond,&workq.lock,&until)==0);
    pthread_mutex_unlock(&workq.lock);
}
static CUresult upload(CUdeviceptr d,const void*s,size_t n,CUstream st)
{ (void)st; CHECK(d>=fake_base&&d-fake_base<=VRAM&&n<=VRAM-(d-fake_base)); memcpy((void*)(uintptr_t)d,s,n); return 0; }
static CUresult download(void*d,CUdeviceptr s,size_t n,CUstream st)
{ (void)st; CHECK(s>=fake_base&&s-fake_base<=VRAM&&n<=VRAM-(s-fake_base)); memcpy(d,(void*)(uintptr_t)s,n); return 0; }
static CUresult sync_stream(CUstream s) { (void)s; return 0; }
static CUresult forbidden_free(CUdeviceptr p) { (void)p; CHECK(0); return 1; }
static CUresult ctxbind(CUcontext c) { CHECK(c==g_cu_ctx); return 0; }
static CUresult meminfo(size_t*a,size_t*t)
{
    pthread_mutex_lock(&meminfo_lock);
    if(meminfo_barrier){meminfo_entered=1;pthread_cond_broadcast(&meminfo_cond);while(!meminfo_release)pthread_cond_wait(&meminfo_cond,&meminfo_lock);}
    *a=mem_free; *t=mem_total;
    pthread_mutex_unlock(&meminfo_lock); return 0;
}
static CUresult gran(size_t*v,const void*p,int f)
{ (void)p; CHECK(f==0); *v=65536; return 0; }
static CUresult reserve(CUdeviceptr*p,size_t n,size_t a,CUdeviceptr at,unsigned long long fl)
{ CHECK(n==VRAM&&a==65536&&!at&&!fl); *p=fake_base=(CUdeviceptr)(uintptr_t)vram; return 0; }
static CUresult create(uint64_t*h,size_t n,const void*p,unsigned long long fl)
{ (void)n;(void)p; CHECK(!fl); *h=1; return 0; }
static CUresult mapmem(CUdeviceptr p,size_t n,size_t o,uint64_t h,unsigned long long f)
{ (void)n;(void)o;(void)h;(void)f; CHECK(p==fake_base||p==fake_base+BLOCK); ++commits[(p-fake_base)/BLOCK]; return 0; }
static CUresult accessmem(CUdeviceptr p,size_t n,const void*a,size_t c)
{ (void)p;(void)n;(void)a; CHECK(c==1); return 0; }
static CUresult unmapmem(CUdeviceptr p,size_t n) { (void)n; ++releases[(p-fake_base)/BLOCK]; return 0; }
static CUresult releasemem(uint64_t h) { CHECK(h==1); return 0; }
static CUresult addressfree(CUdeviceptr p,size_t n) { CHECK(p==fake_base&&n==VRAM); return 0; }
static CUresult ctxsync(void) { return 0; }
void *dlsym(void *h,const char*s)
{
    if (h!=(void*)0x12345) { static void *libc; static void *(*real_dlsym)(void *,const char *); if (!libc) libc=dlopen("libc.so.6",RTLD_NOW); if (!real_dlsym) *(void **)(&real_dlsym)=dlvsym(libc,"dlsym","GLIBC_2.34"); return real_dlsym(h,s); }
#define SYM(n,f) if(!strcmp(s,n)) return (void *)(f)
    SYM("cuMemGetAllocationGranularity",gran); SYM("cuMemAddressReserve",reserve);
    SYM("cuMemCreate",create); SYM("cuMemMap",mapmem); SYM("cuMemSetAccess",accessmem);
    SYM("cuMemAddressFree",addressfree); SYM("cuMemUnmap",unmapmem);
    SYM("cuMemRelease",releasemem); SYM("cuCtxSynchronize",ctxsync);
#undef SYM
    return NULL;
}
static void *retired_vram;
static int fixture_prctl(int option,...)
{ CHECK(option==PR_SET_IO_FLUSHER); return 0; }
static void pattern(unsigned char*p,unsigned seed,int compressible)
{ for(size_t i=0;i<COMP_PAGE;i++){ seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;p[i]=compressible?(unsigned char)((i/64+seed%5)%5):(unsigned char)seed; } }
static void check_io(int fd,size_t n,off_t off)
{ CHECK(fd==BACK_FD&&off>=0&&(size_t)off<=sizeof(backing)-n); }
static ssize_t fixture_pwrite(int fd,const void*p,size_t n,off_t off)
{ check_io(fd,n,off); memcpy(backing+off,p,n); return n; }
static ssize_t fixture_pread(int fd,void*p,size_t n,off_t off)
{ check_io(fd,n,off); memcpy(p,backing+off,n); return n; }
static int fixture_fsync(int fd) { CHECK(fd==BACK_FD); return 0; }
static void seed_pages(void)
{
    for(unsigned pg=0;pg<PAGES;pg++){ pattern(expected[pg],0x12345+pg*23,pg%3==0);
        struct cpend op={.pg=pg,.plain=(const char*)expected[pg]}; prepare_plain(pg,op.plain,0,&op); CHECK(write_pending(&op,1,NULL)==0); }
    CHECK(g_pages_raw+g_pages_compressed==PAGES);
}
static void verify_pages(void)
{ for(unsigned p=0;p<PAGES;p++){ unsigned char got[COMP_PAGE]; CHECK(comp_read((uint64_t)p*COMP_PAGE,(char*)got,COMP_PAGE,NULL)==0); CHECK(!memcmp(got,expected[p],COMP_PAGE)); } }
static void init(void)
{
    alarm(10); g_compress=COMP_LZO;g_dedup=0;g_nbd_threads=1;g_export_size=(uint64_t)PAGES*COMP_PAGE;g_vram_size=VRAM;
    vram=calloc(1,VRAM);CHECK(vram); g_vram_ptr=(CUdeviceptr)(uintptr_t)vram; fake_base=g_vram_ptr;
    _cuMemcpyHtoDAsync=upload;_cuMemcpyDtoHAsync=download;_cuStreamSynchronize=sync_stream;_cuMemFree=forbidden_free;_cuMemGetInfo=meminfo;_cuCtxSetCurrent=ctxbind;g_cu_ctx=(CUcontext)(uintptr_t)0x123;
    CHECK(compress_init()==0&&g_npages==PAGES);g_back_fd=BACK_FD;
    uint64_t base=0; CHECK(cuda_vmm_pool_reserve(&workq.vmm_pool,(void*)0x12345,0,VRAM,&base)==0&&base==g_vram_ptr);
    workq.vmm_enabled=1;workq.backend=QUEUE_BACKEND_CUDA;workq.vmm_count=2;workq.vmm_block_bytes=BLOCK;
    workq.vmm_refs=calloc(2,sizeof(*workq.vmm_refs));workq.vmm_released=calloc(2,1);CHECK(workq.vmm_refs&&workq.vmm_released);
    g_prepare_slot=vmm_prepare_slot;
    CHECK(!cuda_vmm_pool_commit_block(workq.vmm_pool,0)&&!cuda_vmm_pool_commit_block(workq.vmm_pool,1));
    workq.stage=calloc(1,compression_stage_size());CHECK(workq.stage);CHECK(posix_memalign((void**)&workq.drain_page,4096,COMP_PAGE)==0);
    t_cstage=workq.stage;t_lzo_work=g_lzo_work[0]; seed_pages();
    workq.drain_state=DRAIN_RUNNING;workq.drain_jobs_left=0;
    /* Direct test drain: reaching DRAIN_OK retains VA and empty slab metadata. */
    int done=0; while(!done) { uint32_t e=drain_mock_pool(&workq,&done); if(e) fprintf(stderr,"drain errno=%u cursor=%llu refs=%llu,%llu released=%u,%u flags=%d,%d\n",e,(unsigned long long)workq.drain_cursor,(unsigned long long)workq.vmm_refs[0],(unsigned long long)workq.vmm_refs[1],workq.vmm_released[0],workq.vmm_released[1],commits[0]-releases[0],commits[1]-releases[1]); CHECK(!e); }
    workq.drain_state=DRAIN_OK; retired_vram=vram;
    CHECK(g_vram_ptr==(CUdeviceptr)(uintptr_t)vram&&g_slabs&&g_pool_retired&&releases[0]&&releases[1]);
    verify_pages();
}
static void start_worker(void) { CHECK(pthread_create(&workq.worker,NULL,worker,&workq)==0); wait_flag(&workq.started); CHECK(workq.started==1); }
static void reset_jobs(void)
{ for(int i=0;i<QDEPTH;i++) workq.jobs[i].state=JOB_FREE;workq.next_seq=1;workq.drain_seq=0;workq.drain_jobs_left=QDEPTH; }
int rearm_test_main(void) __asm__("main");
int rearm_test_main(void)
{
    init(); reset_jobs();start_worker();
    /* poll_free_vram exact threshold boundaries while the pool is retired. */
    size_t threshold=vram_reserve(mem_total);
    mem_free=threshold-1;poll_free_vram();CHECK(!workq.rearm_pending&&workq.drain_state==DRAIN_OK);
    mem_free=threshold;poll_free_vram();CHECK(!workq.rearm_pending&&workq.drain_state==DRAIN_OK);
    mem_free=threshold+MARGIN-1;poll_free_vram();CHECK(!workq.rearm_pending&&workq.drain_state==DRAIN_OK);
    uint32_t saved_free=g_free_chunk; uint32_t saved_head=g_class_head[0];
    mem_free=threshold+MARGIN-1;
    CHECK(vmm_rearm_pool(&workq)==EAGAIN&&g_pool_retired&&g_free_chunk==saved_free&&g_class_head[0]==saved_head);
    workq.reserve_state=DISK_RESERVE_ATTEMPTED;
    CHECK(vmm_rearm_pool(&workq)==ENOSPC&&g_pool_retired&&g_free_chunk==saved_free);
    workq.reserve_state=DISK_RESERVE_OFF;
    mem_free=threshold+MARGIN;
    ++workq.vmm_refs[0]; CHECK(vmm_rearm_pool(&workq)==EIO&&g_pool_retired&&g_free_chunk==saved_free); --workq.vmm_refs[0];
    g_slabs[0].bm[0]=1; CHECK(vmm_rearm_pool(&workq)==EIO&&g_pool_retired&&g_free_chunk==saved_free); g_slabs[0].bm[0]=0;
    /* Low-free polling queues the actual worker drain; later free memory rearms. */
    mem_free=threshold-1;poll_free_vram();wait_state(DRAIN_OK);
    CHECK(g_pool_retired&&workq.vmm_refs_ready==1&&workq.drain_cursor==g_npages&&workq.vmm_released[0]&&workq.vmm_released[1]);
    verify_pages();
    mem_free=threshold+MARGIN;poll_free_vram();wait_state(DRAIN_IDLE);CHECK(!g_pool_retired&&workq.vmm_refs_ready==0);
    CHECK(commits[0]==1&&commits[1]==1); verify_pages();
    pattern(replacement,0xfed123,0); unsigned commits_before=commits[0]+commits[1];
    CHECK(comp_write(0,(char*)replacement,COMP_PAGE,NULL)==0);memcpy(expected[0],replacement,COMP_PAGE);
    CHECK(commits[0]+commits[1]>commits_before);verify_pages();
    /* A second real worker drain after new writes releases newly committed blocks. */
    unsigned releases_before=releases[0]+releases[1];
    pthread_mutex_lock(&workq.lock);workq.drain_cursor=0;workq.drain_seq=workq.next_seq;workq.drain_jobs_left=0;workq.drain_state=DRAIN_PENDING;pthread_cond_broadcast(&workq.cond);pthread_mutex_unlock(&workq.lock);
    wait_state(DRAIN_OK);CHECK(releases[0]+releases[1]>releases_before);verify_pages();
    /* Hold the worker inside meminfo after it dropped workq.lock in rearm. */
    pthread_mutex_lock(&meminfo_lock);meminfo_barrier=1;meminfo_entered=0;meminfo_release=0;pthread_mutex_unlock(&meminfo_lock);
    pthread_mutex_lock(&workq.lock);workq.drain_state=DRAIN_OK;workq.rearm_pending=1;pthread_cond_broadcast(&workq.cond);pthread_mutex_unlock(&workq.lock);
    pthread_mutex_lock(&meminfo_lock);while(!meminfo_entered)pthread_cond_wait(&meminfo_cond,&meminfo_lock);pthread_mutex_unlock(&meminfo_lock);
    pthread_mutex_lock(&workq.lock);CHECK(workq.drain_state==DRAIN_REARMING);workq.shutdown_requested=1;workq.stopping=1;pthread_mutex_unlock(&workq.lock);
    pthread_mutex_lock(&meminfo_lock);meminfo_release=1;pthread_cond_broadcast(&meminfo_cond);pthread_mutex_unlock(&meminfo_lock);
    CHECK(pthread_join(workq.worker,NULL)==0);CHECK(workq.drain_state==DRAIN_OK&&g_pool_retired);
    CHECK(cuda_vmm_pool_destroy(&workq.vmm_pool)==0&&!workq.vmm_pool);
    puts("VMM rearm CPU fixture: PASS (GPU=0 device=0)");return 0;
}
