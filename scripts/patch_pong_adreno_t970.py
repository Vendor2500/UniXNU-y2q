#!/usr/bin/env python3
"""Patch PongAdreno for SM-T970 (SM8250 / Adreno 650 hardware constants and ZAP metadata size)."""
from pathlib import Path
import sys


ZAP_H_REPLACEMENTS = (
    ('PongZapReservedPhysical=0x8b91a000,', 'PongZapReservedPhysical=0x9031a000,'),
)

ZAP_C_REPLACEMENTS = (
    ('if (loads!=1 || signatures!=1 || header>4096 || signature>4096-header ||',
     'if (loads!=1 || signatures!=1 || header>4096 || signature>8192-header ||'),
    ('!reserved->cpu || meta->bytes!=4096 || !meta->physical ||',
     '!reserved->cpu || (meta->bytes!=4096 && meta->bytes!=8192) || meta->bytes<plan->metadata_bytes || !meta->physical ||'),
)

CP_CPP_REPLACEMENTS = (
    ('extern "C" int get_preemption_level(void);\n',
     'extern "C" int get_preemption_level(void);\n'
     'static IOLock *gPongScmCallLock;\n'
     '__attribute__((weak)) extern "C" bool pong_scm_call_lock(void)\n'
     '{\n'
     '    if(!__atomic_load_n(&gPongScmCallLock,__ATOMIC_ACQUIRE)) {\n'
     '        IOLock *lock=IOLockAlloc(),*empty=nullptr;\n'
     '        if(lock && !__atomic_compare_exchange_n(&gPongScmCallLock,&empty,lock,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))\n'
     '            IOLockFree(lock);\n'
     '    }\n'
     '    if(!gPongScmCallLock)return false;\n'
     '    IOLockLock(gPongScmCallLock);\n'
     '    return true;\n'
     '}\n'
     '__attribute__((weak)) extern "C" void pong_scm_call_unlock(void)\n'
     '{\n'
     '    if(gPongScmCallLock)IOLockUnlock(gPongScmCallLock);\n'
     '}\n'),
    ('if(gpu->metadata.allocate(4096,gpu->output_bits,true,4096) || gpu->metadata.makeWriteCombined())return false;',
     'if(gpu->metadata.allocate(8192,gpu->output_bits,true,4096) || gpu->metadata.makeWriteCombined())return false;'),
    ('PongZapConfig c={PongAdrenoZapMdt,PongAdrenoZapMdtBytes,PongAdrenoZapSignature,PongAdrenoZapSignatureBytes,2,\n'
     '        PongAdrenoZapCode,PongAdrenoZapCodeBytes,1,\n'
     '        {static_cast<volatile uint8_t *>(gpu->metadata.mapping()),gpu->metadata.spans()[0].physical,4096},',
     'PongZapConfig c={PongAdrenoZapMdt,PongAdrenoZapMdtBytes,PongAdrenoZapSignature,PongAdrenoZapSignatureBytes,1,\n'
     '        PongAdrenoZapCode,PongAdrenoZapCodeBytes,2,\n'
     '        {static_cast<volatile uint8_t *>(gpu->metadata.mapping()),gpu->metadata.spans()[0].physical,8192},'),
)

GMU_H_REPLACEMENTS = (
    ('PongAdrenoChipId=0x07030002,', 'PongAdrenoChipId=0x06050002,'),
    ('PongAdrenoGmemBytes=0x200000,', 'PongAdrenoGmemBytes=0x100000,'),
    ('PongAdrenoGpuIrq=332,PongAdrenoHfiIrq=336,PongAdrenoGmuIrq=337,',
     'PongAdrenoGpuIrq=300,PongAdrenoHfiIrq=304,PongAdrenoGmuIrq=305,'),
    ('PongAdrenoCxGdscr=0x9108,', 'PongAdrenoCxGdscr=0x106c,'),
    ('PongAdrenoCxHwControl=0x953c,', 'PongAdrenoCxHwControl=0x1540,'),
    ('PongAdrenoGxGdscr=0x905c,', 'PongAdrenoGxGdscr=0x100c,'),
    ('PongAdrenoSmmuClockVote=0x7000};', 'PongAdrenoSmmuClockVote=0x5000};'),
)


# D0a (11-15): the A6xx (generation 6) ring and CP_ME_INIT forms for the A650, as UniXNU-gts7xlwifi drivers/Adreno650
# (MIT, same NativeAdreno lineage): NativeAdrenoRing.cpp emitA6xxPrologue / emitFence / program() NO_UPDATE / submitWords,
# NativeAdrenoCpInit.cpp a6xx branch. The default stays generation 7, so the A730 path is unchanged.
RING_H_REPLACEMENTS = (
    ('    bool attached_=false,poisoned_=false,reset_pending_=false,stamps_=false;\n',
     '    bool attached_=false,poisoned_=false,reset_pending_=false,stamps_=false;\n'
     '    /* 7 (A7xx, the default) or 6 (A6xx: one CP thread, CACHE_FLUSH_TS fences, NO_UPDATE), set before attach() */\n'
     '    unsigned generation_=7;\n'),
    ('    void requestContextReset() {reset_pending_=true;}\n',
     '    void requestContextReset() {reset_pending_=true;}\n'
     '    IOReturn setGeneration(unsigned generation)\n'
     '    {\n'
     '        if(attached_)return kIOReturnBusy;\n'
     '        if(generation!=6 && generation!=7)return kIOReturnBadArgument;\n'
     '        generation_=generation;return kIOReturnSuccess;\n'
     '    }\n'
     '    unsigned generation() const {return generation_;}\n'),
)

RING_CPP_REPLACEMENTS = (
    ('    RingBvFenceOffset = 12    /* msm_rbmemptrs.bv_fence; +8 is bv_rptr, the BV RPTR shadow hw init programs */\n};\n',
     '    RingBvFenceOffset = 12,   /* msm_rbmemptrs.bv_fence; +8 is bv_rptr, the BV RPTR shadow hw init programs */\n'
     '    /* A6xx forms (a6xx_submit, a6xx_hw_init): CACHE_FLUSH_TS (0x04) fences with the IRQ bit, the UCHE and two CCU\n'
     '     * invalidates before the IBs, NO_UPDATE in CP_RB_CNTL (without APRIV the CP may not write the RPTR shadow). */\n'
     '    RingA6xxCacheFlushTs = 0x04,\n'
     '    RingA6xxPrologueWords = 6,\n'
     '    RingNoUpdate = 1u << 27\n'
     '};\n'),
    ('IOReturn NativeAdrenoRing::emitPrologue(uint32_t at,bool reset,uint32_t *end)\n{\n'
     '    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);\n',
     'IOReturn NativeAdrenoRing::emitPrologue(uint32_t at,bool reset,uint32_t *end)\n{\n'
     '    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);\n'
     '    if(generation_==6) {\n'
     '        /* a6xx_submit: one CP thread and one page table, so no thread selection and no context entry; the UCHE\n'
     '         * invalidate a6xx_set_pagetable ends with, then PC_CCU_INVALIDATE_DEPTH and _COLOR. Six words. */\n'
     '        uint32_t six[RingA6xxPrologueWords]={0};\n'
     '        const uint32_t uche=RingCacheInvalidate,depth=RingCcuInvalidateDepth,color=RingCcuInvalidateColor;\n'
     '        size_t k=packet(six,0,RingFenceOpcode,&uche,1);\n'
     '        if(k)k=packet(six,k,RingFenceOpcode,&depth,1);\n'
     '        if(k)k=packet(six,k,RingFenceOpcode,&color,1);\n'
     '        if(k!=RingA6xxPrologueWords)return poison();\n'
     '        (void)reset;\n'
     '        for(uint32_t i=0;i<RingA6xxPrologueWords;++i)ring_[(at+i)&mask]=six[i];\n'
     '        *end=(at+RingA6xxPrologueWords)&mask;return kIOReturnSuccess;\n'
     '    }\n'),
    ('    words[1]=seqno;\n    words[3]=RingFenceEvent|RingFenceEnable|RingFenceIrq;\n',
     '    words[1]=seqno;\n    words[3]=generation_==6?RingA6xxCacheFlushTs|RingFenceIrq:RingFenceEvent|RingFenceEnable|RingFenceIrq;\n'),
    ('IOReturn NativeAdrenoRing::emitFramedFence(uint32_t at,uint32_t seqno,uint32_t *end)\n{\n',
     'IOReturn NativeAdrenoRing::emitFramedFence(uint32_t at,uint32_t seqno,uint32_t *end)\n{\n'
     '    /* a6xx_submit has no BV thread: CP_SCRATCH(2) and one CACHE_FLUSH_TS, i.e. the plain fence */\n'
     '    if(generation_==6)return emitFence(at,seqno,end);\n'),
    ('    const uint32_t control=(uint32_t)(ilog2(ring_bytes/8)&0x3f)|(ilog2(RingBlockBytes/8)<<8);\n',
     '    const uint32_t control=(uint32_t)(ilog2(ring_bytes/8)&0x3f)|(ilog2(RingBlockBytes/8)<<8)|(generation_==6?RingNoUpdate:0u);\n'),
    ('    const uint32_t extra=framed?RingPrologueWords+(reset?RingResetWords:0)+RingFramedFenceWords+(stamps_?2*RingStampWords:0):RingFenceWords;\n',
     '    const uint32_t extra=generation_==6?(framed?RingA6xxPrologueWords:0)+RingFenceWords:\n'
     '        framed?RingPrologueWords+(reset?RingResetWords:0)+RingFramedFenceWords+(stamps_?2*RingStampWords:0):RingFenceWords;\n'),
)

CPINIT_H_REPLACEMENTS = (
    ('    /* Pong\'s gen7 firmware requires BIT11 when DMS is not negotiated. */\n    uint32_t dms_enabled;\n};\n',
     '    /* Pong\'s gen7 firmware requires BIT11 when DMS is not negotiated. */\n    uint32_t dms_enabled;\n'
     '    /* 6: a6xx_cp_init (CP_ME_INIT with eight words, no thread control, no register list); 0 or 7: the A7XX sequence */\n'
     '    uint32_t generation;\n};\n'),
)

CPINIT_CPP_REPLACEMENTS = (
    ('    if(!config || config->dms_enabled>1 || !config->sqe_iova || (config->sqe_iova&4095) ||\n'
     '        config->sqe_iova>=UINT64_C(1)<<48 || !config->pwrup_reglist_iova ||\n',
     '    const bool a6xx=config && config->generation==6;\n'
     '    if(!config || config->dms_enabled>1 || !config->sqe_iova || (config->sqe_iova&4095) ||\n'
     '        (config->generation && config->generation!=6 && config->generation!=7) ||\n'
     '        config->sqe_iova>=UINT64_C(1)<<48 || (!a6xx && !config->pwrup_reglist_iova) ||\n'),
    ('    uint32_t words[CpInitWords]={0};\n    if(AdrenoPM4Type7(CpThreadControlOpcode,1,&words[0])!=AdrenoPM4Success ||\n',
     '    uint32_t words[CpInitWords]={0};\n'
     '    if(a6xx) {\n'
     '        /* a6xx_cp_init: CP_ME_INIT with Linux\'s mask 0x2f, three hardware contexts, 0x20000000 (no header dump),\n'
     '         * zeros for the workarounds and padding; 1 + 8 words, through the bootstrap path like the A7XX one. */\n'
     '        if(AdrenoPM4Type7(CpMeInitOpcode,8,&words[0])!=AdrenoPM4Success)return poison();\n'
     '        words[1]=0x2f;words[2]=0x00000003;words[3]=0x20000000;\n'
     '        uint32_t at=0;\n'
     '        if(ring->submitBootstrap(words,9,&at))return poison();\n'
     '        done_=true;*writePointer=at;\n'
     '        return kIOReturnSuccess;\n'
     '    }\n'
     '    if(AdrenoPM4Type7(CpThreadControlOpcode,1,&words[0])!=AdrenoPM4Success ||\n'),
)

SMMUCTX_CPP_REPLACEMENTS = (
    ('#include <IOKit/IOLib.h>\n',
     '#include <IOKit/IOLib.h>\n'
     '/* 1: the pinned tree\'s dma-coherent table walks (the A730); the SM-T970 backend sets 0 (SM8250 walks are non-coherent) */\n'
     'extern "C" {volatile uint32_t native_adreno_smmu_coherent_walk=1;}\n'),
    ('.asid=asid,.coherent_walk=1,', '.asid=asid,.coherent_walk=native_adreno_smmu_coherent_walk?1u:0u,'),
)

IRQ_CPP_REPLACEMENTS = (
    ('uint32_t NativeAdrenoIrq::mask()\n{\n    return kEnable;\n}\n',
     '/* nonzero: the RBBM mask to program instead of A7XX_INT_MASK (the SM-T970 backend sets A6XX_INT_MASK 0x03d0e242) */\n'
     'extern "C" {volatile uint32_t native_adreno_irq_mask_override=0;}\n'
     'uint32_t NativeAdrenoIrq::mask()\n{\n    return native_adreno_irq_mask_override?native_adreno_irq_mask_override:kEnable;\n}\n'),
    ('    if(io_.write32(io_.cookie,IrqRbbmMask,kEnable))return poison();\n',
     '    const uint32_t want=mask();\n    if(io_.write32(io_.cookie,IrqRbbmMask,want))return poison();\n'),
    ('    if(readback!=kEnable)return poison();\n', '    if(readback!=want)return poison();\n'),
)


ACCEL_CPP_REPLACEMENTS = (
    # pong_metal_plugin=NAME: where Metal looks for the plugin bundle (MetalPluginName, relative to
    # /System/Library/Extensions); lets the SM-T970 keep it on the Data volume while the OS volume is read-only.
    ('extern "C" void PongAdrenoAcceleratorStaticInit(void);\n',
     'extern "C" void PongAdrenoAcceleratorStaticInit(void);\nextern "C" boolean_t PE_parse_boot_argn(const char *,void *,int);\n'),
    ('    service->setProperty("MetalPluginName","PongAdrenoMetal");\n',
     '    {char plugin[96]="../../../var/pong/PongAdrenoMetal";(void)PE_parse_boot_argn("pong_metal_plugin",plugin,sizeof(plugin));\n'
     '     service->setProperty("MetalPluginName",plugin);}\n'),
)


PUBLISH_CPP_REPLACEMENTS = (
    # USB `metaldiag` (kernel z91): what the Metal plugin and its processes did, from the diagnostic table above
    ('extern "C" {volatile uint32_t native_adreno_view_diag[6],native_adreno_view_kept;}   /* kept: BoCpuUnmap calls that left the view for another lease */\n}\n',
     'extern "C" {volatile uint32_t native_adreno_view_diag[6],native_adreno_view_kept;}   /* kept: BoCpuUnmap calls that left the view for another lease */\n}\n'
     'extern "C" int pong_metal_diag_usb(char *out,size_t cap)\n{\n'
     '    const NativeAdrenoMetalDiag &d=native_adreno_metal_diag;\n'
     '    return snprintf(out,cap,"metaldiag: opens=%u closes=%u busy=%u bad=%u grants=%u calls=%u last=%u/%#x attach=%#x props=%u names=%u reports=%u"\n'
     '        " foreign=%u/%#x\\n openers=%s,%s,%s readers=%s,%s,%s name_reader=%s starters=%s,%s,%s\\n report=%s\\n report2=%s\\n",\n'
     '        d.opens,d.closes,d.busy,d.bad,d.grants,d.calls,d.last_selector,d.last_status,d.attach_status,d.property_reads,d.name_reads,d.reports,\n'
     '        d.foreign_opens,d.foreign_type,d.opener[0],d.opener[1],d.opener[2],d.reader[0],d.reader[1],d.reader[2],d.name_reader,\n'
     '        d.starter[0],d.starter[1],d.starter[2],d.report,d.report2);\n}\n'),
)


def apply_replacements(path: Path, pairs: tuple[tuple[str, str], ...]) -> bool:
    if not path.is_file():
        return False
    raw = path.read_text(encoding='utf-8')
    eol = '\r\n' if '\r\n' in raw else '\n'
    txt = raw.replace('\r\n', '\n')
    changed = False
    for old, new in pairs:
        if new in txt:
            continue
        assert txt.count(old) == 1, f'{path.name}: expected {old!r} once, found {txt.count(old)}'
        txt = txt.replace(old, new)
        changed = True
    if changed:
        path.write_bytes(txt.replace('\n', eol).encode('utf-8'))
    return changed


def patch_pong_adreno(adreno_source: Path) -> None:
    root = adreno_source.resolve()
    if not root.is_dir():
        return
    modified = []
    for rel, pairs in (
        ('PongAdrenoZap.h', ZAP_H_REPLACEMENTS),
        ('PongAdrenoZap.c', ZAP_C_REPLACEMENTS),
        ('PongAdrenoCp.cpp', CP_CPP_REPLACEMENTS),
        ('PongAdrenoGmu.h', GMU_H_REPLACEMENTS),
        ('submit/NativeAdrenoRing.h', RING_H_REPLACEMENTS),
        ('submit/NativeAdrenoRing.cpp', RING_CPP_REPLACEMENTS),
        ('submit/NativeAdrenoCpInit.h', CPINIT_H_REPLACEMENTS),
        ('submit/NativeAdrenoCpInit.cpp', CPINIT_CPP_REPLACEMENTS),
        ('submit/NativeAdrenoSmmuContext.cpp', SMMUCTX_CPP_REPLACEMENTS),
        ('submit/NativeAdrenoIrq.cpp', IRQ_CPP_REPLACEMENTS),
        ('PongAdrenoAccelerator.cpp', ACCEL_CPP_REPLACEMENTS),
        ('submit/NativeAdrenoPublish.cpp', PUBLISH_CPP_REPLACEMENTS),
    ):
        target = root / rel
        if apply_replacements(target, pairs):
            modified.append(rel)
    if modified:
        print(f'Applied SM-T970 PongAdreno patch to {root}: {", ".join(modified)}', flush=True)


if __name__ == '__main__':
    patch_pong_adreno(Path(sys.argv[1]) if len(sys.argv) > 1 else Path('.'))
