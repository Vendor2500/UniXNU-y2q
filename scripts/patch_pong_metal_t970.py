#!/usr/bin/env python3
"""SM-T970 changes to the PongMetal plugin, applied to the checked-out submodule at build time. Idempotent.

1. Accept the Adreno 650 as well as the A730: the A650 backend (drivers/T970Adreno/PongAdrenoCpT970.cpp) reports chip
   0x06050002 with 0x120000 bytes of GMEM; the plugin otherwise refuses every chip but 0x07030002/0x200000.
2. Turnip's debug options from /private/var/pong/tu_debug when that file exists (first line), else the plugin's own
   "sysmem,nocb": the A650 tuning (the same tablet's UniXNU-gts7xlwifi used "noubwc,nocb") without a rebuild.
4. Telemetry on demand (every selected client): each fsput of /private/var/pong/metal_report (new mtime or size) makes
   the process report the telemetry rows whose first word starts with one of the file's words ("@name" limits it to
   processes whose name starts with name; no words: S RW SR P0 P1 M). M rows are the selectors the bridge lacks. The
   kernel's gpudiag keeps 8 lines, which backboardd's per-commit K rows always filled, so no WebKit line was ever read.
5. GPU readers of an IOSurface wait for other processes' GPU writers (only while /private/var/pong/surf_read_wait
   exists, and only as long as the kernel's z97 pong_surf_order_ms allows): see READ_WAIT below.
"""
import sys
from pathlib import Path

EDITS = (
    ('info.chip_id!=0x07030002 || info.gmem_bytes!=0x200000 ||',
     '!((info.chip_id==0x07030002 && info.gmem_bytes==0x200000) ||\n'
     '         (info.chip_id==0x06050002 && info.gmem_bytes==0x120000)) || /* T970 A650 */'),
    ('    setenv("TU_DEBUG","sysmem,nocb",1);\n',
     '    {char tu[64]="sysmem,nocb";FILE *f=fopen("/private/var/pong/tu_debug","r");   /* T970: tuning without a rebuild */\n'
     '     if(f){if(fgets(tu,sizeof tu,f))tu[strcspn(tu,"\\r\\n")]=0;fclose(f);}\n'
     '     setenv("TU_DEBUG",tu,1);}\n'),
    # 4. telemetry on demand
    ('#include <sys/stat.h>\n',
     '#include <sys/stat.h>\n#include <errno.h>\n#include <os/lock.h>\n'),
    ('''static void missing(const char *type,const char *selector)
{
    static unsigned count;
    if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)>=12)return;
    char text[192];snprintf(text,sizeof(text),"missing %.70s %.90s",type,selector);
    report(text,-1);
}
''',
     r'''/* T970: every distinct missing selector with its count, for the on-demand report's M rows. */
static os_unfair_lock missingLock=OS_UNFAIR_LOCK_INIT;
static char missingSeen[24][100];
static unsigned missingTimes[24],missingKinds;
static void missing(const char *type,const char *selector)
{
    char text[192];snprintf(text,sizeof(text),"missing %.70s %.90s",type,selector);
    os_unfair_lock_lock(&missingLock);
    unsigned i=0;
    while(i<missingKinds && strncmp(missingSeen[i],text+8,sizeof missingSeen[0]-1))i++;
    if(i==missingKinds && missingKinds<24)strlcpy(missingSeen[missingKinds++],text+8,sizeof missingSeen[0]);
    if(i<missingKinds)missingTimes[i]++;
    os_unfair_lock_unlock(&missingLock);
    static unsigned count;
    if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)>=12)return;
    report(text,-1);
}
'''),
    ('static void startPresentation(id candidate,unsigned port)\n',
     r'''/* T970: the telemetry rows a request names (see patch_pong_metal_t970.py, 4). */
static bool rowWanted(const char *row,char words[][24],unsigned n)
{
    for(unsigned i=0;i<n;i++)if(!strncmp(row,words[i],strlen(words[i])))return true;
    return false;
}
static void reportRows(const char *filter)
{
    char words[12][24]={};unsigned n=0;char copy[160];strlcpy(copy,filter,sizeof copy);
    for(char *save=nullptr,*w=strtok_r(copy," \t\r\n",&save);w;w=strtok_r(nullptr," \t\r\n",&save)) {
        if(w[0]=='@'){if(strncmp(getprogname(),w+1,strlen(w+1)))return;continue;}
        if(n<12)strlcpy(words[n++],w,sizeof words[0]);
    }
    if(!n)for(const char *w:{"S","RW","SR","P0","P1","M"})strlcpy(words[n++],w,sizeof words[0]);
    char *rows=(char *)calloc(1,8192);if(!rows)return;
    native_bridge_metal_telemetry(rows,8192);
    for(char *line=rows;*line;) {
        char *end=strchr(line,'\n');if(end)*end=0;
        if(rowWanted(line,words,n))report(line,0);
        if(!end)break;line=end+1;
    }
    free(rows);
    if(!rowWanted("M",words,n))return;
    char seen[24][100];unsigned times[24];
    os_unfair_lock_lock(&missingLock);
    const unsigned kinds=missingKinds;memcpy(seen,missingSeen,sizeof seen);memcpy(times,missingTimes,sizeof times);
    os_unfair_lock_unlock(&missingLock);
    if(!kinds)report("M none",0);
    for(unsigned i=0;i<kinds;i++){char text[160];snprintf(text,sizeof text,"M n%u %.120s",times[i],seen[i]);report(text,0);}
}
static void watchReportRequests()
{
    static dispatch_source_t timer;if(timer)return;
    static const char *path="/private/var/pong/metal_report";
    struct stat first={};
    // A sandbox that hides /private/var/pong (WebKit's GPU process may) gets the default rows every minute instead.
    const bool hidden=stat(path,&first) && errno!=ENOENT;
    __block struct timespec seenTime=first.st_mtimespec;__block off_t seenSize=first.st_size;
    timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_global_queue(QOS_CLASS_UTILITY,0));
    if(!timer)return;
    const uint64_t period=(hidden?60:5)*NSEC_PER_SEC;
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,period),period,NSEC_PER_SEC);
    dispatch_source_set_event_handler(timer,^{
        if(hidden){reportRows("");return;}
        struct stat st={};if(stat(path,&st))return;
        if(st.st_mtimespec.tv_sec==seenTime.tv_sec && st.st_mtimespec.tv_nsec==seenTime.tv_nsec && st.st_size==seenSize)return;
        seenTime=st.st_mtimespec;seenSize=st.st_size;
        char filter[160]={};
        if(FILE *f=fopen(path,"r")){if(!fgets(filter,sizeof filter,f))filter[0]=0;fclose(f);}
        reportRows(filter);
    });
    dispatch_resume(timer);
}
static void startPresentation(id candidate,unsigned port)
'''),
    ('            if(device){report("real device returned to matching stock discovery",0);startPresentation(device,port);}\n',
     '            if(device){report("real device returned to matching stock discovery",0);startPresentation(device,port);\n'
     '                watchReportRequests();}   /* T970 */\n'),
)


# 3. The bridge does not answer the two alignment selectors (UniXNU-gts7xlwifi adreno-metal 0001, same tablet: with Turnip's
#    4-byte constant / 64-byte linear texture alignments answered, CoreAnimation's frames came out row-smeared on the A650).
BRIDGE_EDITS = (
    ('    ("minConstantBufferAlignmentBytes", 0),\n    ("deviceLinearReadOnlyTextureAlignmentBytes", 0),\n',
     '    // T970: minConstantBufferAlignmentBytes, deviceLinearReadOnlyTextureAlignmentBytes left unanswered (A650)\n'),
)


# 5. A command buffer brackets the IOSurfaces it renders to with the kernel's GPU handoff (PongSurfaceHandoff.h), and the CPU
#    display waits for those writers (MiniIOSurfaceDrawn), but a command that only samples a surface waited for nobody: with
#    both WebKit's GPU process and backboardd on Metal, backboardd's frame could sample a WebKit tile before WebKit's job
#    (often still held in its runtime) had written it, and Core Animation redraws only damaged regions, so the tile stayed
#    black. Stock drivers order such cross-process reads in the kernel. Here a reader takes and drops a handoff for each
#    sampled surface it does not write itself; with z97's pong_surf_order_ms the take waits for the other writers.
READ_WAIT = r'''#include "PongSurfaceHandoff.h"
// T970 (patch_pong_metal_t970.py, 5): wait for other processes' GPU writers of the IOSurfaces this command samples.
static std::atomic<uint64_t> gReadWaitChecks,gReadWaitWaited,gReadWaitUs,gReadWaitMax,gReadWaitRefused;
static int gReadWaitOn=-1;
static void waitForSurfaceWriters(NSSet *owners,NSSet *writers) {
 if(gReadWaitOn<0)gReadWaitOn=access("/private/var/pong/surf_read_wait",F_OK)==0;
 if(!gReadWaitOn || !owners.count)return;
 std::vector<uint32_t> written;
 for(NBTexture *t in writers)if(t.surface)written.push_back(IOSurfaceGetID(t.surface));
 std::vector<std::pair<uint32_t,IOSurfaceRef>> read;
 for(id o in owners) {
  if(![o isKindOfClass:[NBTexture class]])continue;
  NBTexture *t=o;
  for(unsigned depth=0;t && !t.surface && depth<64;++depth)t=[(id)t.parent isKindOfClass:[NBTexture class]]?(NBTexture *)t.parent:nil;
  if(!t || !t.surface)continue;
  const uint32_t sid=IOSurfaceGetID(t.surface);
  if(std::find(written.begin(),written.end(),sid)==written.end())read.emplace_back(sid,t.surface);
 }
 std::sort(read.begin(),read.end(),[](const auto &a,const auto &b){return a.first<b.first;});
 read.erase(std::unique(read.begin(),read.end(),[](const auto &a,const auto &b){return a.first==b.first;}),read.end());
 for(const auto &entry:read) {
  mach_port_t right=IOSurfaceCreateMachPort(entry.second);if(!right)continue;
  const uint64_t began=nowUs();
  const kern_return_t result=pongsurface::handoff(right,false,false);   // waits while another process writes it (z97)
  const uint64_t took=nowUs()-began;
  if(!result)(void)pongsurface::handoff(right,true,false);else gReadWaitRefused++;
  mach_port_deallocate(mach_task_self(),right);
  gReadWaitChecks++;
  if(took>=500) {
   gReadWaitWaited++;gReadWaitUs+=took;
   uint64_t seen=gReadWaitMax.load();while(took>seen && !gReadWaitMax.compare_exchange_weak(seen,took)){}
  }
 }
}
'''

FRONTEND_EDITS = (
    ('#include "PongSurfaceHandoff.h"\n', READ_WAIT),
    (' if(!self.encodeFailure && !pongsurface::begin(_surfaces,displayHandoffs))\n'
     '  [self refuse:@"IOSurface CPU/GPU ownership handoff unavailable"];\n',
     ' if(!self.encodeFailure && !pongsurface::begin(_surfaces,displayHandoffs))\n'
     '  [self refuse:@"IOSurface CPU/GPU ownership handoff unavailable"];\n'
     ' if(!self.encodeFailure)waitForSurfaceWriters(_resourceOwners,_surfaces);   // T970\n'),
    (' row("PR functions resumed=%u",native_prime_functions_resumed);\n',
     ' row("RW on%d checks=%llu waited=%llu us=%llu max=%llu refused=%llu",gReadWaitOn,(unsigned long long)gReadWaitChecks.load(),\n'
     '  (unsigned long long)gReadWaitWaited.load(),(unsigned long long)gReadWaitUs.load(),(unsigned long long)gReadWaitMax.load(),\n'
     '  (unsigned long long)gReadWaitRefused.load());   // T970\n'
     ' row("PR functions resumed=%u",native_prime_functions_resumed);\n'),
)


def apply(path, edits):
    text = path.read_text()
    for old, new in edits:
        if new in text:
            continue
        if text.count(old) != 1:
            raise SystemExit('expected exactly one %r in %s' % (old[:40], path))
        text = text.replace(old, new)
    path.write_text(text)
    print('patched:', path)


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    apply(root / 'drivers/PongMetal/PongMetalPlugin.mm', EDITS)
    apply(root / 'drivers/PongMetal/bridge/NativeMetalObjects.rs', BRIDGE_EDITS)
    apply(root / 'drivers/PongMetal/NativeBridgeMetalFrontend.mm', FRONTEND_EDITS)


if __name__ == '__main__':
    main()
