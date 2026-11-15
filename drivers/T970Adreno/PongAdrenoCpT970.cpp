/* SPDX-License-Identifier: MIT */
/*
 * SM-T970 (SM8250 / Adreno 650): PongAdreno's native GPU backend on the A650 (stage D0).
 *
 * PongAdrenoCp.cpp (MIT) prepares and starts PongAdreno's backend on the A730 with gen7 hardware code. On the A650 the hardware
 * part is the kernel's own bsd/dev/pong_gpu_gmu.c, which the device already ran: `gpuprobe gmustart` (GMU cold start and HFI),
 * `gpuprobe gxon` (GX through the GMU), the a650 hw_init and the zap shader. This file keeps PongAdrenoCp's structure and backend
 * objects (NativeAdrenoSmmuContext, NativeAdrenoVm, NativeAdrenoRing, NativeAdrenoCpInit, NativeAdrenoSubmit, NativeAdrenoTimer)
 * and replaces only the hardware calls:
 *   prepare  the GPU's SMMU context (bank 0 / ASID 2, the DT iommus <0 0x401> as the four exact streams 0, 1, 0x400, 0x401 as
 *            UniXNU-gts7xlwifi GpuProbe.cpp gpuContext did on this tablet), its VM, the five kernel buffers, a650_sqe.fw (fwput);
 *   start    pong_gpu_a650_hw_init(), ring 0 as generation 6 (the A6xx forms of the T970 patch), CP_SQE_CNTL, a6xx CP_ME_INIT
 *            through the bootstrap path, the zap shader (pong_gpu_zap_auth), CP_SET_SECURE_MODE(0), one CP_MEM_WRITE fence, then
 *            NativeAdrenoSubmit/Timer and PongAdrenoPublishClient (the ABI 1 user client of the 22H374 Metal plugin and Turnip).
 * No GPU interrupt yet (fence waits poll; stage D1). `gpuprobe selftest` then runs the user-client path from the kernel, as
 * GpuProbe.cpp ucRound: open, two BOs, GPU and CPU maps, an IB of three CP_MEM_WRITEs, submit, fence, the target read back, close.
 * Everything runs once per boot from the USB control thread; a failure leaves the backend unpublished.
 */
#include "PongAdrenoCp.h"
#include "submit/NativeAdrenoSmmuContext.h"
#include "submit/NativeAdrenoCpInit.h"
#include "submit/AdrenoPM4.h"
#include "submit/NativeAdrenoIrq.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>

extern "C" {
/* bsd/dev/pong_gpu_gmu.c */
int pong_gpu_t970_ready(vm_offset_t *core, vm_offset_t *smmu);
int pong_gpu_a650_hw_init(void);
int pong_gpu_zap_auth(char *reply, size_t cap);
int pong_fw_lookup(const char *name, const uint8_t **data, uint32_t *len);
extern uint64_t (*native_adreno_vm_now_us)(void);
extern uint64_t (*native_adreno_gpu_ticks)(void);
extern volatile uint32_t native_adreno_wc_enabled;
IOService *native_adreno_published_service(void);
extern volatile uint32_t native_adreno_smmu_coherent_walk;   /* T970 patch of NativeAdrenoSmmuContext.cpp */
extern volatile uint32_t native_adreno_irq_mask_override;    /* T970 patch of NativeAdrenoIrq.cpp */
IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
int get_preemption_level(void);
int pong_t970_gpu_start(char *reply, size_t cap);
int pong_t970_gpu_selftest(char *reply, size_t cap);
}

namespace {
/* Sizes as PongAdrenoCp.cpp (SQE, ring 0, memptrs, restore list (unused on the A650), scratch), IOVAs at 32 MiB: PongAdrenoCp's
 * 0x100000.. puts the SQE inside the A650's UCHE GMEM window (base 0, 1 MiB + 128 KiB), the reason GpuProbe.cpp moved its kernel
 * buffers up; `gpuprobe cpinit` ran from 32 MiB on this tablet. The ring stays 32 KiB aligned. */
constexpr uint64_t iovas[] = {0x02000000, 0x02020000, 0x02030000, 0x02031000, 0x02032000};
constexpr size_t sizes[] = {0x13000, 0x8000, 0x1000, 0x1000, 0x1000};
constexpr uint16_t gpu_sids[] = {0, 1, 0x400, 0x401};
constexpr unsigned gpu_stream_count = sizeof(gpu_sids) / sizeof(*gpu_sids);
constexpr uint32_t SmmuContextFaultMask = 0xc00001feu;
/* a6xx.xml dword offsets */
constexpr uint32_t RegSqeInstrBase = 0x0830, RegSqeCntl = 0x0808, RegRbRptr = 0x0806, RegRbWptr = 0x0807,
    RegRbbmStatus = 0x0210, RegRbbmInt0Status = 0x0201, RegCpHwFault = 0x0821, RegCpIntStatus = 0x0823,
    RegCpProtectStatus = 0x0824, RegSqeStatAddr = 0x0908, RegSqeStatData = 0x0909;
constexpr uint32_t A650ChipId = 0x06050002u, A650GmemBytes = 0x120000u;   /* Linux a650: SZ_1M + SZ_128K */

struct Device {
    vm_offset_t core = 0, smmu = 0;
    NativeAdrenoRingIo regs{};
    ArmSmmu4KIo smmu_io{};
    NativeAdrenoMemory tables, buffers[5];
    NativeAdrenoSmmuContext context;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoCpInit init;
    NativeAdrenoSubmit submit;
    NativeAdrenoTimer timer;
    NativeAdrenoStatus status;
    NativeAdrenoIrq irq;
    PongCpBackend backend{};
    uint32_t irq_count = 0, irq_fatal = 0, irq_route = 0;
    uint64_t irq_affinity = 0;
    bool irq_attached = false;
    unsigned output_bits = 0, slots[gpu_stream_count] = {};
    uint32_t alias_scratch[262144]{};
    bool started = false, poisoned = false;
    unsigned step = 0;
    char why[96] = "";
};
Device *gpu;
int tries, selftests;

uint64_t now(void *) {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;}
uint64_t vm_now() {return now(nullptr);}
/* With the GPU interrupt attached a waiter sleeps until the next one or 1 ms (PongAdrenoCp.cpp idle); before that it spins 20 us */
int idle(void *)
{
    if (gpu && gpu->irq_attached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && !get_preemption_level()) {
        const uint32_t before = __atomic_load_n(&gpu->irq_count, __ATOMIC_ACQUIRE);
        assert_wait_timeout(event_t(&gpu->irq_count), THREAD_UNINT, 1, NSEC_PER_MSEC);
        if (__atomic_load_n(&gpu->irq_count, __ATOMIC_ACQUIRE) != before) thread_wakeup(event_t(&gpu->irq_count));
        thread_block(THREAD_CONTINUE_NULL);
    } else {
        IODelay(20);
    }
    return 0;
}
int barrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int reg_write(void *, uint32_t reg, uint32_t v)
{
    if (!gpu || !gpu->core || reg >= 0x10000) return 1;
    *reinterpret_cast<volatile uint32_t *>(gpu->core + reg * 4) = v;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
int reg_read(void *, uint32_t reg, uint32_t *v)
{
    if (!gpu || !gpu->core || reg >= 0x10000) return 1;
    *v = *reinterpret_cast<volatile uint32_t *>(gpu->core + reg * 4);
    return 0;
}
int smmu_read(void *, size_t at, uint32_t *v)
{if ((at & 3) || at > 0x10000 - 4) return 1; *v = *reinterpret_cast<volatile uint32_t *>(gpu->smmu + at); return 0;}
int smmu_write(void *, size_t at, uint32_t v)
{if ((at & 3) || at > 0x10000 - 4) return 1; *reinterpret_cast<volatile uint32_t *>(gpu->smmu + at) = v; return barrier(nullptr);}
int smmu_read64(void *, size_t at, uint64_t *v)
{if ((at & 7) || at > 0x10000 - 8) return 1; *v = *reinterpret_cast<volatile uint64_t *>(gpu->smmu + at); return 0;}
int smmu_write64(void *, size_t at, uint64_t v)
{if ((at & 7) || at > 0x10000 - 8) return 1; *reinterpret_cast<volatile uint64_t *>(gpu->smmu + at) = v; return barrier(nullptr);}
uint32_t rd(uint32_t reg) {uint32_t v = 0; if (reg_read(nullptr, reg, &v)) gpu->poisoned = true; return v;}
bool wr(uint32_t reg, uint32_t v) {return !reg_write(nullptr, reg, v);}
uint32_t sr(size_t at) {uint32_t v = 0; if (smmu_read(nullptr, at, &v)) gpu->poisoned = true; return v;}
bool sw(size_t at, uint32_t v) {return !smmu_write(nullptr, at, v);}
void stop(const char *why) {strlcpy(gpu->why, why, sizeof gpu->why);}
size_t bank0() {const uint32_t id1 = sr(0x24); return size_t((id1 >> 31) ? 0x10000u : 0x1000u) << (((id1 >> 28) & 7) + 1);}

bool faulted()
{
    if (!gpu || gpu->poisoned) return true;
    const uint32_t fsr = sr(bank0() + 0x58), global = sr(0x48), cp = rd(RegCpHwFault);
    if ((fsr & SmmuContextFaultMask) || global || cp) {gpu->poisoned = true; gpu->status.noteFault();}
    return gpu->poisoned;
}
bool client_healthy() {return gpu && gpu->started && !gpu->irq_fatal && !faulted();}
/* SPI 300 (INTID 332, DT kgsl_3d0_irq, level high): no logging or allocation here; NativeAdrenoIrq reads and clears RBBM status,
 * a fatal status masks the GPU interrupts and poisons the client (PongAdrenoCp.cpp interrupt) */
void interrupt(void *)
{
    NativeAdrenoIrqResult result = {};
    const IOReturn st = gpu->irq.handle(&result);
    __atomic_fetch_add(&gpu->irq_count, 1, __ATOMIC_RELAXED);
    thread_wakeup(event_t(&gpu->irq_count));
    if (st || (result.flags & NativeAdrenoIrqFatal)) {
        __atomic_fetch_or(&gpu->irq_fatal, result.status ? result.status : 0x80000000u, __ATOMIC_RELEASE);
        gpu->regs.write32(gpu->regs.cookie, 0x0038, 0);   /* RBBM_INT_0_MASK */
        gpu->regs.barrier(gpu->regs.cookie);
    }
}

int publish_ring(void *, const volatile uint32_t *words, size_t bytes)
{
    const auto &m = gpu->buffers[1];
    auto base = reinterpret_cast<uintptr_t>(m.mapping()), at = reinterpret_cast<uintptr_t>(words);
    if (at < base || at - base > m.bytes() || bytes > m.bytes() - (at - base)) return 1;
    const auto result = gpu->buffers[1].publishForDevice(at - base, bytes, false);
    __asm__ volatile("dsb sy" ::: "memory");
    return result != kIOReturnSuccess;
}
int acquire_fence(void *, volatile uint32_t *words, size_t bytes)
{
    const auto &m = gpu->buffers[2];
    auto base = reinterpret_cast<uintptr_t>(m.mapping()), at = reinterpret_cast<uintptr_t>(words);
    if (at < base || at - base > m.bytes() || bytes > m.bytes() - (at - base)) return 1;
    const auto result = gpu->buffers[2].acquireDeviceWrites(at - base, bytes);
    __asm__ volatile("dsb sy" ::: "memory");
    return result != kIOReturnSuccess;
}
/* RPTR at the bootstrap WPTR and RBBM_STATUS.GPU_BUSY_IGN_AHB (bit 23) clear, as PongAdrenoCp wait_bootstrap; 200 ms */
bool wait_bootstrap(uint32_t wptr)
{
    const uint64_t begin = now(nullptr);
    while (now(nullptr) - begin < 200000) {
        if (faulted()) return false;
        if (rd(RegRbRptr) == wptr && !(rd(RegRbbmStatus) & (1u << 23))) return true;
        IODelay(50);
    }
    return false;
}
bool wait_fence(uint32_t fence)
{
    const NativeAdrenoRingClock clock{nullptr, now, idle};
    uint32_t observed = 0;
    const auto result = gpu->ring.wait(fence, &clock, 200000, &observed);
    return result == NativeAdrenoRingFenceComplete && !faulted();
}

/* The GPU's SMMU context and the backend's memory, as PongAdrenoCpPrepare with the A650's streams and ACTLR */
IOReturn prepare()
{
    const uint32_t id0 = sr(0x20), id1 = sr(0x24), id2 = sr(0x28);
    const unsigned count = (id0 & 255) > 8 ? 8 : (id0 & 255);
    const bool ex = (id0 >> 8) & 1;
    const uint8_t bits[] = {32, 36, 40, 42, 44, 48};
    const unsigned code = (id2 >> 4) & 15;
    if ((id1 >> 31) || ex || count < gpu_stream_count + 1 || code >= 6) {stop("smmu_ids"); return kIOReturnUnsupported;}
    gpu->output_bits = bits[code] > 40 ? 40 : bits[code];
    /* memory before any register: a failure here may run again (8 MiB of contiguous tables after boot may not be there) */
    IOReturn status = gpu->tables.allocate(2053 * 4096, gpu->output_bits, true, 2053 * 4096);
    if (status) {stop("tables_alloc"); gpu->step = 0; return status;}
    gpu->step = 1;   /* from here on registers change */
    /* free slots, the precedent's {0,1,4,5} first (GpuProbe.cpp:582), then 3; SMR invalid and S2CR of type FAULT (the GMU holds
     * SMR 2 for stream 5 under mask 0x400); every valid SMR checked for overlap with the GPU's streams */
    for (unsigned i = 0; i < count; i++) {
        const uint32_t smr = sr(0x800 + 4 * i);
        if (!(smr & 0x80000000u)) continue;
        const uint32_t mask = (smr >> 16) & 0x7fff, sid = smr & 0x7fff;
        for (uint16_t wanted : gpu_sids)
            if (!((sid ^ wanted) & ~mask & 0x7fff)) {stop("stream_owned"); return kIOReturnBusy;}
    }
    static const unsigned order[] = {0, 1, 4, 5, 3, 2, 6, 7};
    unsigned found = 0;
    for (unsigned k = 0; k < sizeof order / sizeof *order && found < gpu_stream_count; k++) {
        const unsigned i = order[k];
        if (i >= count || (sr(0x800 + 4 * i) & 0x80000000u) || (sr(0xc00 + 4 * i) & 0x30000u) != 0x20000u) continue;
        gpu->slots[found++] = i;
    }
    if (found != gpu_stream_count) {stop("no_slots"); return kIOReturnNoResources;}
    for (unsigned k = 0; k < gpu_stream_count; k++) {
        const unsigned i = gpu->slots[k];
        if (!sw(0xc00 + 4 * i, 0x20000u) || sr(0xc00 + 4 * i) != 0x20000u) {stop("s2cr_normalize"); return kIOReturnIOError;}
    }
    const size_t bank = bank0();
    if (!sw(bank, 0) || (sr(bank) & 1)) {stop("bank_busy"); return kIOReturnIOError;}
    /* ACTLR without CPRE (arm_mmu500_reset), as GpuProbe.cpp:680-688 on this tablet; read back (cpinit saw 0x3 -> 0x1) */
    const uint32_t actlr = sr(bank + 4);
    if (!sw(bank + 4, actlr & ~2u) || (sr(bank + 4) & 2u)) {stop("actlr"); return kIOReturnIOError;}
    status = gpu->context.prepare(&gpu->tables, 0, 0, UINT64_C(0x100000000), 1048576, 0, 2, 4096, gpu->output_bits, 32);
    if (!status) gpu->context.setAliasScratch(gpu->alias_scratch, 262144);
    if (!status) status = gpu->context.program(&gpu->smmu_io);
    ArmSmmu4KStream streams[gpu_stream_count] = {};
    for (unsigned i = 0; i < gpu_stream_count; i++) streams[i] = {gpu->slots[i], gpu_sids[i]};
    if (!status) status = gpu->context.attach(streams, gpu_stream_count);
    if (status) {stop("context"); return status;}
    /* TLBIALL of bank 0 (ArmSmmu4KProgramDisabledContext only invalidates the ASID), as GpuProbe.cpp:686-688 */
    if (!sw(bank + 0x618, 0) || !sw(bank + 0x7f0, 0)) {stop("tlbiall"); return kIOReturnIOError;}
    for (unsigned i = 0; i < 1000 && (sr(bank + 0x7f4) & 1); i++) IODelay(1);
    if (sr(bank + 0x7f4) & 1) {stop("tlbsync"); return kIOReturnTimeout;}
    NativeAdrenoVmRegion lower = {&gpu->tables, gpu->context.arenaHandle(), gpu->context.stateHandle()};
    status = gpu->vm.attach(&gpu->smmu_io, gpu->context.configHandle(), gpu->context.streamsHandle(), &lower, nullptr, 131072, 262144);
    for (unsigned i = 0; i < 5 && !status; i++) {
        uint64_t handle = 0;
        status = gpu->buffers[i].allocate(sizes[i], gpu->output_bits, true, sizes[i]);
        if (!status) status = gpu->buffers[i].makeWriteCombined();
        if (!status) status = gpu->vm.map(&gpu->buffers[i], 0, iovas[i], i ? 3 : 5, 0, 2, &handle);
    }
    if (status) {stop("vm_buffers"); return status;}
    const uint8_t *fw = nullptr;
    uint32_t fw_len = 0;
    if (pong_fw_lookup("a650_sqe.fw", &fw, &fw_len) || !fw || fw_len < 8 || fw_len - 4 > sizes[0]) {stop("sqe_missing"); return kIOReturnBadArgument;}
    memcpy(gpu->buffers[0].mapping(), fw + 4, fw_len - 4);
    __asm__ volatile("dsb sy" ::: "memory");
    if (memcmp(gpu->buffers[0].mapping(), fw + 4, fw_len - 4)) {stop("sqe_readback"); return kIOReturnIOError;}
    status = gpu->buffers[0].publishForDevice(0, sizes[0], false);
    if (status) stop("sqe_publish");
    return status;
}

/* hw_init, SQE, ring 0, CP_ME_INIT, zap, secure exit, a fence, then the backend published (PongAdrenoCpStart's order) */
IOReturn start(char *zap, size_t zap_cap)
{
    gpu->step = 2;
    if (pong_gpu_a650_hw_init()) {stop("hw_init"); return kIOReturnIOError;}
    NativeAdrenoRingCache cache = {nullptr, publish_ring, acquire_fence};
    IOReturn status = gpu->ring.setGeneration(6);
    if (!status) status = gpu->ring.attach(&gpu->regs, &cache, gpu->buffers[1].mapping(), sizes[1], iovas[1],
        gpu->buffers[2].mapping(), sizes[2], iovas[2]);
    if (status) {stop("ring_attach"); return status;}
    if (!wr(RegSqeInstrBase, uint32_t(iovas[0])) || !wr(RegSqeInstrBase + 1, uint32_t(iovas[0] >> 32)) || !wr(RegSqeCntl, 1)) {
        stop("sqe_start"); return kIOReturnIOError;
    }
    gpu->step = 3;
    NativeAdrenoCpInitConfig config = {iovas[0], 0, 1, 0, 6};
    uint32_t wptr = 0;
    status = gpu->init.run(&gpu->regs, &gpu->ring, &config, &wptr);
    if (status || !wait_bootstrap(wptr)) {stop("me_init"); return status ? status : kIOReturnTimeout;}
    gpu->step = 4;
    if (pong_gpu_zap_auth(zap, zap_cap)) {stop("zap"); return kIOReturnNotPermitted;}
    uint32_t words[2] = {};
    if (AdrenoPM4Type7(0x66, 1, words) != AdrenoPM4Success || gpu->ring.submitBootstrap(words, 2, &wptr) || !wait_bootstrap(wptr)) {
        stop("secure_exit"); return kIOReturnIOError;
    }
    gpu->step = 5;
    auto *scratch = static_cast<volatile uint32_t *>(gpu->buffers[4].mapping());
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t write[4] = {0, uint32_t(iovas[4]), uint32_t(iovas[4] >> 32), 0xa6500d00u};
    uint32_t fence = 0;
    if (AdrenoPM4Type7(0x3d, 3, write) != AdrenoPM4Success || gpu->ring.submit(write, 4, &fence) || !wait_fence(fence)) {
        stop("first_fence"); return kIOReturnIOError;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (scratch[0] != write[3]) {stop("first_write"); return kIOReturnIOError;}
    /* the GPU interrupt (D1): pong_gpu_irq=0 keeps the polled waits */
    unsigned irq_on = 1;
    PE_parse_boot_argn("pong_gpu_irq", &irq_on, sizeof irq_on);
    if (irq_on) {
        native_adreno_irq_mask_override = 0x03d0e242u;   /* A6XX_INT_MASK */
        status = gpu->irq.attach(&gpu->regs);
        if (!status) status = armcpu_spi_attach(332, false, interrupt, nullptr, &gpu->irq_route, &gpu->irq_affinity);
        if (!status) status = gpu->irq.programMask(nullptr);
        if (status) {wr(0x0038, 0); stop("irq_attach"); return status;}
        gpu->irq_attached = true;
    }
    gpu->step = 6;
    status = gpu->submit.attach(&gpu->vm, &gpu->ring);
    NativeAdrenoTimeIo timer = {nullptr, reg_read};
    if (!status) status = gpu->timer.attach(&timer, NativeAdrenoTimeCp);
    uint64_t ticks = 0;
    if (!status) status = gpu->timer.read(&ticks);
    if (status || !ticks) {stop("submit_timer"); return status ? status : kIOReturnNotReady;}
    native_adreno_gpu_ticks = []() -> uint64_t {uint64_t v = 0; if (gpu && !gpu->poisoned) (void)gpu->timer.read(&v); return v;};
    gpu->started = true;
    native_adreno_wc_enabled = 1;
    gpu->backend = {&gpu->vm, &gpu->submit, &gpu->ring, &gpu->timer, &gpu->status, {nullptr, now, idle},
        {A650ChipId, A650GmemBytes, 0, 0x1fffffffff000ULL, 0x20000000, 0xA0000000, 16, 6, 1, 4, NativeAdrenoDeviceClientIova, 0},
        gpu->output_bits, client_healthy};
    gpu->step = 7;
    if (!PongAdrenoPublishClient(&gpu->backend)) {stop("publish"); return kIOReturnNoResources;}
    gpu->step = 8;
    return kIOReturnSuccess;
}

IOReturn uc_call(IOUserClient *uc, uint32_t method, const void *in, void *out)
{
    IOExternalMethodArguments a{};
    a.version = kIOExternalMethodArgumentsCurrentVersion;
    a.selector = method;
    const size_t is = NativeAdrenoUserClient::inputSize(method), os = NativeAdrenoUserClient::outputSize(method);
    a.structureInput = is ? in : nullptr;
    a.structureInputSize = uint32_t(is);
    a.structureOutput = os ? out : nullptr;
    a.structureOutputSize = uint32_t(os);
    return uc->externalMethod(method, &a, nullptr, nullptr, nullptr);
}

/* GpuProbe.cpp ucRound with the ABI 1 structures: returns the matching dwords (of 4) or -1; steps/first error in *pass/*fail */
int uc_round(IOService *svc, unsigned *pass, unsigned *steps, uint32_t *first_error, unsigned *first_step)
{
    auto step = [&](IOReturn st) {
        const unsigned at = (*steps)++;
        if (!st) ++*pass;
        else if (!*first_error) {*first_error = uint32_t(st); *first_step = at;}
        return !st;
    };
    IOUserClient *uc = nullptr;
    if (!step(svc->newUserClient(kernel_task, nullptr, 0x4147, nullptr, &uc)) || !uc) return -1;
    NativeAdrenoAbiParams params{};
    step(uc_call(uc, NativeAdrenoMethodQuery, nullptr, &params));
    NativeAdrenoAbiDeviceInfo info{};
    step(uc_call(uc, NativeAdrenoMethodGetDeviceInfo, nullptr, &info));
    step(info.chip_id == A650ChipId ? kIOReturnSuccess : kIOReturnError);
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, tgt{};
    int matched = -1;
    IOMemoryDescriptor *desc[2] = {};
    IOMemoryMap *maps[2] = {};
    if (step(uc_call(uc, NativeAdrenoMethodBoAlloc, &alloc, &ib)) && step(uc_call(uc, NativeAdrenoMethodBoAlloc, &alloc, &tgt))) {
        const uint64_t ib_iova = info.va_start + 0x4000, tgt_iova = info.va_start + 0x5000;
        NativeAdrenoAbiBoMap map_ib{ib.handle, ib_iova, 1, 0, 2, 0}, map_tgt{tgt.handle, tgt_iova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoAbiBoCpuMap cm_ib{}, cm_tgt{};
        cm_ib.handle = ib.handle;
        cm_tgt.handle = tgt.handle;
        NativeAdrenoAbiBoCpuMapReply cr_ib{}, cr_tgt{};
        if (step(uc_call(uc, NativeAdrenoMethodBoMap, &map_ib, &r)) && step(uc_call(uc, NativeAdrenoMethodBoMap, &map_tgt, &r)) &&
            step(uc_call(uc, NativeAdrenoMethodBoCpuMap, &cm_ib, &cr_ib)) && step(uc_call(uc, NativeAdrenoMethodBoCpuMap, &cm_tgt, &cr_tgt))) {
            IOOptionBits opt[2] = {};
            const uint32_t tokens[2] = {uint32_t(cr_ib.token), uint32_t(cr_tgt.token)};
            bool mapped = true;
            for (int i = 0; i < 2; ++i) {
                if (uc->clientMemoryForType(tokens[i], &opt[i], &desc[i]) || !desc[i]) {mapped = false; continue;}
                maps[i] = desc[i]->createMappingInTask(kernel_task, 0, opt[i]);
                if (!maps[i]) mapped = false;
            }
            if (step(mapped ? kIOReturnSuccess : kIOReturnNoMemory)) {
                volatile uint32_t *iw = reinterpret_cast<volatile uint32_t *>(maps[0]->getVirtualAddress());
                volatile uint32_t *tw = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress());
                for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
                const uint32_t values[3] = {0x0badc0deu, 0xfeed0001u, 0x600d0003u};
                uint32_t words[12];
                bool built = true;
                for (unsigned i = 0; i < 3; ++i) {
                    built = built && AdrenoPM4Type7(0x3d, 3, &words[i * 4]) == AdrenoPM4Success;
                    words[i * 4 + 1] = uint32_t(tgt_iova + i * 4);
                    words[i * 4 + 2] = uint32_t(tgt_iova >> 32);
                    words[i * 4 + 3] = values[i];
                }
                for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
                __asm__ volatile("dsb sy" ::: "memory");
                NativeAdrenoAbiSubmit submit{};
                submit.buffer_count = 1;
                submit.buffers[0] = {ib.handle, ib_iova, sizeof words};
                NativeAdrenoAbiSubmitReply sr{};
                if (step(built ? kIOReturnSuccess : kIOReturnInternalError) && step(uc_call(uc, NativeAdrenoMethodSubmit, &submit, &sr))) {
                    NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
                    if (step(uc_call(uc, NativeAdrenoMethodWaitFence, &wait, nullptr))) {
                        __asm__ volatile("dsb sy" ::: "memory");
                        matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
                    }
                }
            }
        }
    }
    for (auto *m : maps) if (m) m->release();
    for (auto *d : desc) if (d) d->release();
    if (ib.handle) {
        NativeAdrenoAbiBoFree f{ib.handle};
        step(uc_call(uc, NativeAdrenoMethodBoFree, &f, nullptr));
    }
    if (tgt.handle) {
        NativeAdrenoAbiBoFree f{tgt.handle};
        step(uc_call(uc, NativeAdrenoMethodBoFree, &f, nullptr));
    }
    step(uc->clientClose());
    uc->release();
    return matched;
}
}

extern "C" int pong_t970_gpu_start(char *reply, size_t cap)
{
    static char zap[256];
    vm_offset_t core = 0, smmu = 0;

    if (pong_gpu_t970_ready(&core, &smmu)) {
        snprintf(reply, cap, "gpustart: stop=no_gx (gmustart, gxon first; not after cpinit)\n");
        return -1;
    }
    if (tries++) {
        snprintf(reply, cap, "gpustart: stop=once_per_boot step=%u why=%s\n", gpu ? gpu->step : 0, gpu ? gpu->why : "");
        return -1;
    }
    gpu = new Device;
    if (!gpu) {snprintf(reply, cap, "gpustart: stop=no_memory\n"); return -1;}
    gpu->core = core;
    gpu->smmu = smmu;
    gpu->regs = {nullptr, reg_write, reg_read, barrier};
    /* polled_faults 0: ArmSmmu4KStreams global() then expects sCR0 & 0x407 == 0x406, what this tablet reads (the hypervisor
     * keeps GFRE); 1 would require 0x402 and fail every attach (review 11-15) */
    gpu->smmu_io = {nullptr, 0x10000, smmu_read, smmu_read64, smmu_write, smmu_write64, barrier, 0};
    native_adreno_smmu_coherent_walk = 0;   /* SM8250's GPU SMMU walks are non-coherent (GpuProbe.cpp:644, our cpinit TCR) */
    native_adreno_vm_now_us = vm_now;
    gpu->step = 0;
    IOReturn status = prepare();
    if (status && gpu->step == 0) {
        snprintf(reply, cap, "gpustart: stop=%s status=%#x (no register touched; may run again)\n", gpu->why, status);
        delete gpu;
        gpu = nullptr;
        tries = 0;
        return -1;
    }
    if (!status) status = start(zap, sizeof zap);
    uint32_t sqe_pc = 0, sqe_op = 0;
    if (status && gpu->step >= 3 && wr(RegSqeStatAddr, 0)) {sqe_pc = rd(RegSqeStatData); sqe_op = rd(RegSqeStatData);}
    const size_t bank = bank0();
    snprintf(reply, cap,
        "gpustart: status=%#x step=%u why=%s slots=%u/%u/%u/%u smr=%#x/%#x s2cr=%#x bits=%u rptr=%#x wptr=%#x rbbm=%#x int0=%#x"
        " hwfault=%#x cpint=%#x prot=%#x sqe=%#x/%#x cb0[sctlr=%#x fsr=%#x far=%#x:%#x fsynr=%#x] gfsr=%#x zap[%s]\n",
        status, gpu->step, gpu->why[0] ? gpu->why : "none", gpu->slots[0], gpu->slots[1], gpu->slots[2], gpu->slots[3],
        sr(0x800 + 4 * gpu->slots[0]), sr(0x800 + 4 * gpu->slots[3]), sr(0xc00 + 4 * gpu->slots[0]), gpu->output_bits,
        gpu->step >= 2 ? rd(RegRbRptr) : 0, gpu->step >= 2 ? rd(RegRbWptr) : 0, gpu->step >= 2 ? rd(RegRbbmStatus) : 0,
        gpu->step >= 2 ? rd(RegRbbmInt0Status) : 0, gpu->step >= 2 ? rd(RegCpHwFault) : 0, gpu->step >= 2 ? rd(RegCpIntStatus) : 0,
        gpu->step >= 2 ? rd(RegCpProtectStatus) : 0, sqe_pc, sqe_op, sr(bank), sr(bank + 0x58), sr(bank + 0x64), sr(bank + 0x60),
        sr(bank + 0x68), sr(0x48), zap[0] ? zap : "-");
    return status ? -1 : 0;
}

extern "C" int pong_t970_gpu_selftest(char *reply, size_t cap)
{
    IOService *svc = native_adreno_published_service();
    if (!gpu || !gpu->started || !svc) {
        snprintf(reply, cap, "selftest: stop=not_published started=%d service=%d\n", gpu ? gpu->started : 0, svc != nullptr);
        return -1;
    }
    unsigned pass[2] = {}, steps[2] = {}, first_step[2] = {};
    uint32_t first_error[2] = {};
    int matched[2];
    ++selftests;
    for (int i = 0; i < 2; i++) matched[i] = uc_round(svc, &pass[i], &steps[i], &first_error[i], &first_step[i]);
    const bool ok = matched[0] == 4 && matched[1] == 4;
    snprintf(reply, cap, "selftest: status=%d run=%d round1=%d/4 steps=%u/%u err=%#x@%u round2=%d/4 steps=%u/%u err=%#x@%u"
        " rptr=%#x wptr=%#x hwfault=%#x cb0_fsr=%#x gfsr=%#x irq=%u/%u fatal=%#x route=%#x\n", ok ? 0 : -1, selftests, matched[0],
        pass[0], steps[0], first_error[0], first_step[0], matched[1], pass[1], steps[1], first_error[1], first_step[1], rd(RegRbRptr),
        rd(RegRbWptr), rd(RegCpHwFault), sr(bank0() + 0x58), sr(0x48), gpu->irq_attached, gpu->irq_count, gpu->irq_fatal, gpu->irq_route);
    return ok ? 0 : -1;
}
