# Applies "WebAssembly on ARMv7, BBQ only" (PLAN §76.3) to a WebKit source tree (the root that holds Source/). Idempotent:
# a change already there is skipped. Usage: python3 -I engine/tools/wasm32.py <tree> engine/tools/wasm32
import os, shutil, sys

root, newdir = sys.argv[1], sys.argv[2]
jsc = os.path.join(root, 'Source', 'JavaScriptCore')
wtf = os.path.join(root, 'Source', 'WTF')


def copy(source, target):
    # Only when different: a rewritten file is a rebuild of everything that includes it.
    if os.path.exists(target) and open(source, 'rb').read() == open(target, 'rb').read():
        return
    shutil.copyfile(source, target)
    print('copied', os.path.relpath(target, root))


def edit(path, old, new, marker=None):
    text = open(path).read()
    if (marker or new) in text:
        return
    if text.count(old) != 1:
        raise SystemExit(f'{path}: expected one match of {old[:120]!r}, found {text.count(old)}')
    open(path, 'w').write(text.replace(old, new))
    print('edited', os.path.relpath(path, root))


def upgrade(path, old, new):
    """An earlier form of a change, in a tree patched before it changed: replaced by the current form (before the step
    that makes the change, so that step finds it done)."""
    text = open(path).read()
    if old in text:
        open(path, 'w').write(text.replace(old, new, 1))
        print('upgraded', os.path.relpath(path, root))

# 0. The JIT and DFG of the port on Thumb-2 Linux too, for the test build of engine/tools/jsclinux (WP81_LINUX_TEST).
edit(os.path.join(wtf, 'wtf', 'PlatformEnable.h'), '#if USE(JSVALUE32_64) && !(OS(WINDOWS) && CPU(ARM_THUMB2))',
     '#if USE(JSVALUE32_64) && !(CPU(ARM_THUMB2) && (OS(WINDOWS) || defined(WP81_LINUX_TEST)))')
edit(os.path.join(wtf, 'wtf', 'PlatformEnable.h'), '#if !(OS(WINDOWS) && CPU(ARM_THUMB2))\n',
     '#if !(CPU(ARM_THUMB2) && (OS(WINDOWS) || defined(WP81_LINUX_TEST)))\n')
edit(os.path.join(jsc, 'runtime', 'Options.h'), '#if OS(WINDOWS) && CPU(ARM_THUMB2) && ENABLE(JIT)',
     '#if CPU(ARM_THUMB2) && (OS(WINDOWS) || defined(WP81_LINUX_TEST)) && ENABLE(JIT)')

# 1. The build switches: WebAssembly with the BBQ JIT on Thumb-2, where the port runs the JIT.
edit(os.path.join(wtf, 'wtf', 'PlatformEnable.h'), '''#if CPU(ARM)
#undef ENABLE_WEBASSEMBLY
#define ENABLE_WEBASSEMBLY 0
#undef ENABLE_B3_JIT
#define ENABLE_B3_JIT 0
#undef ENABLE_WEBASSEMBLY_OMGJIT
#define ENABLE_WEBASSEMBLY_OMGJIT 0
#undef ENABLE_WEBASSEMBLY_BBQJIT
#define ENABLE_WEBASSEMBLY_BBQJIT 0
#endif
''', '''#if CPU(ARM)
#if ENABLE(WEBASSEMBLY) && ENABLE(JIT) && CPU(ARM_THUMB2) && (OS(WINDOWS) || defined(WP81_LINUX_TEST))
/* WP81: WebAssembly in the BBQ JIT only (upstream removed it for ARMv7 in 306366@main; the port brings BBQ back, see
   InPlaceInterpreter32_64.asm). BBQ takes B3's types and Air's address forms; no OMG. */
#undef ENABLE_B3_JIT
#define ENABLE_B3_JIT 1
#undef ENABLE_WEBASSEMBLY_OMGJIT
#define ENABLE_WEBASSEMBLY_OMGJIT 0
#undef ENABLE_WEBASSEMBLY_BBQJIT
#define ENABLE_WEBASSEMBLY_BBQJIT 1
#else
#undef ENABLE_WEBASSEMBLY
#define ENABLE_WEBASSEMBLY 0
#undef ENABLE_B3_JIT
#define ENABLE_B3_JIT 0
#undef ENABLE_WEBASSEMBLY_OMGJIT
#define ENABLE_WEBASSEMBLY_OMGJIT 0
#undef ENABLE_WEBASSEMBLY_BBQJIT
#define ENABLE_WEBASSEMBLY_BBQJIT 0
#endif
#endif
''', marker='WP81: WebAssembly in the BBQ JIT only')

# 2. Run-time options: what BBQ on 32 bits does not do.
edit(os.path.join(jsc, 'runtime', 'Options.cpp'), '''#if !CPU(X86_64) && !CPU(ARM64)
    Options::useConcurrentGC() = false;
    Options::forceUnlinkedDFG() = false;
    Options::useWasmSIMD() = false;
    Options::useWasmIPInt() = false;
#if !CPU(ARM_THUMB2)
    Options::useBBQJIT() = false;
#endif
#endif
''', '''#if !CPU(X86_64) && !CPU(ARM64)
    Options::useConcurrentGC() = false;
    Options::forceUnlinkedDFG() = false;
    Options::useWasmSIMD() = false;
    Options::useWasmIPInt() = false;
#if !CPU(ARM_THUMB2)
    Options::useBBQJIT() = false;
#else
    // WP81: BBQ on 32 bits has no vectors, no 64-bit memories, no wide arithmetic, and the JSPI stack switching was
    // never written for this CPU; a 4 GiB reservation does not fit in the address space.
    Options::useWasmRelaxedSIMD() = false;
    Options::useWasmMemory64() = false;
    Options::useWasmWideArithmetic() = false;
    Options::useJSPI() = false;
    Options::useWasmFastMemory() = false;
#endif
#endif
''', marker='WP81: BBQ on 32 bits has no vectors')

# 3. The interpreter's ARMv7 file: the entry that tiers up, and the common file's label stand-ins for everything else.
llint = os.path.join(jsc, 'llint')
copy(os.path.join(newdir, 'InPlaceInterpreter32_64.asm'), os.path.join(llint, 'InPlaceInterpreter32_64.asm'))
asm = os.path.join(llint, 'InPlaceInterpreter.asm')
edit(asm, '''if JSVALUE64 and (ARM64 or ARM64E or X86_64)
    include InPlaceInterpreter64
else
''', '''if JSVALUE64 and (ARM64 or ARM64E or X86_64)
    include InPlaceInterpreter64
else
if ARMv7
    # WP81: the entry of BBQ-only WebAssembly; the labels below stand in for the interpreter's instructions.
    include InPlaceInterpreter32_64
end
''', marker='include InPlaceInterpreter32_64')
edit(asm, '''.continue:
    if ARMv7
        break # FIXME: ipint support.
    end # ARMv7
end # WEBASSEMBLY_BBQJIT''', '''.continue:
    # WP81: on ARMv7 the caller (ipintEntry of InPlaceInterpreter32_64.asm) throws when there is no compiled code.
end # WEBASSEMBLY_BBQJIT''')
edit(asm, '''    elsif X86_64
        addp sizeOfExtraRegistersPreserved + (2 * MachineRegisterSize), sp
    end
    jmp _wasm_throw_from_slow_path_trampoline''', '''    else
        # X86_64, and ARMv7 (WP81): PC and MC pushed.
        addp sizeOfExtraRegistersPreserved + (2 * MachineRegisterSize), sp
    end
    jmp _wasm_throw_from_slow_path_trampoline''')

cmake = os.path.join(jsc, 'CMakeLists.txt')
edit(cmake, '''    llint/InPlaceInterpreter.asm
    llint/InPlaceInterpreter64.asm
''', '''    llint/InPlaceInterpreter.asm
    llint/InPlaceInterpreter32_64.asm
    llint/InPlaceInterpreter64.asm
''')

# 4. BBQ for 32 bits.
wasm = os.path.join(jsc, 'wasm')
copy(os.path.join(newdir, 'WasmBBQJIT32_64.h'), os.path.join(wasm, 'WasmBBQJIT32_64.h'))
copy(os.path.join(newdir, 'WasmBBQJIT32_64.cpp'), os.path.join(wasm, 'WasmBBQJIT32_64.cpp'))
edit(os.path.join(jsc, 'Sources.txt'), '''wasm/WasmBBQJIT.cpp @no-unify-when(bundle<=8) @cost:5
wasm/WasmBBQJIT64.cpp @no-unify-when(bundle<=8) @cost:3
''', '''wasm/WasmBBQJIT.cpp @no-unify-when(bundle<=8) @cost:5
wasm/WasmBBQJIT32_64.cpp @no-unify
wasm/WasmBBQJIT64.cpp @no-unify-when(bundle<=8) @cost:3
''')
edit(os.path.join(wasm, 'WasmBBQJIT.cpp'), '''#include "WasmBBQJIT.h"
#include "WasmBBQJIT64.h"
''', '''#include "WasmBBQJIT.h"
#include "WasmBBQJIT32_64.h"
#include "WasmBBQJIT64.h"
''')
print('done')

# 5. BBQ's shared header: the pointer check of atomics on 32 bits (no pinned memory registers, the last byte of an
# access must stay below 4 GiB), and the two-word load that takes any address.
bbqh = os.path.join(wasm, 'WasmBBQJIT.h')
edit(bbqh, '''    inline Location emitCheckAndPreparePointer(Value pointer, uint64_t uoffset, uint32_t sizeOfOperation, uint8_t memoryIndex)
    {
        if (WTF::sumOverflows<uint64_t>(static_cast<uint64_t>(sizeOfOperation), uoffset)) {''', '''#if USE(JSVALUE32_64)
    // WP81: the pointer of an atomic access, checked; the caller adds uoffset to the returned register.
    inline Location emitCheckAndPreparePointer(Value pointer, uint64_t uoffset, uint32_t sizeOfOperation, uint8_t memoryIndex)
    {
        // No memory64 on 32 bits (Options.cpp), and every byte of a memory is below 4 GiB.
        RELEASE_ASSERT(!m_info.memory(memoryIndex).isMemory64());
        uint64_t boundary = static_cast<uint64_t>(sizeOfOperation) + uoffset - 1;
        if (boundary > std::numeric_limits<uint32_t>::max()) {
            consume(pointer);
            recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.jump());
            return Location::fromGPR(wasmScratchGPR);
        }

        ScratchScope<1, 0> scratches(*this);
        Location pointerLocation;
        if (pointer.isConst()) {
            uint64_t constantPointer = static_cast<uint32_t>(pointer.asI32());
            if (constantPointer + boundary > std::numeric_limits<uint32_t>::max()) {
                consume(pointer);
                recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.jump());
                return Location::fromGPR(wasmScratchGPR);
            }
            pointerLocation = Location::fromGPR(scratches.gpr(0));
            emitMoveConst(pointer, pointerLocation);
        } else
            pointerLocation = loadIfNecessary(pointer);
        ASSERT(pointerLocation.isGPR());

        ScratchScope<2, 0> globals(*this);
        GPRReg memoryBase = globals.gpr(0);
        GPRReg memorySize = globals.gpr(1);
        if (!memoryIndex)
            loadWebAssemblyGlobalState(memoryBase, memorySize);
        else {
            m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, JSWebAssemblyInstance::offsetOfCachedMemoryBaseSizePair(memoryIndex)), memoryBase);
            m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, JSWebAssemblyInstance::offsetOfCachedMemoryBaseSizePair(memoryIndex) + sizeof(void*)), memorySize);
        }

        // A 32-bit memory is always bounds checked here (no 4 GiB reservation): pointer + boundary, a carry being past
        // 4 GiB, against the size.
        if (boundary)
            recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchAdd32(ResultCondition::Carry, pointerLocation.asGPR(), TrustedImm32(static_cast<uint32_t>(boundary)), wasmScratchGPR));
        else
            m_jit.move(pointerLocation.asGPR(), wasmScratchGPR);
        recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, wasmScratchGPR, memorySize));

        m_jit.addPtr(memoryBase, pointerLocation.asGPR(), wasmScratchGPR);
        consume(pointer);
        return Location::fromGPR(wasmScratchGPR);
    }

    template<typename AddressType>
    void loadUnalignedPair32(AddressType, GPRReg lo, GPRReg hi);
#else
    inline Location emitCheckAndPreparePointer(Value pointer, uint64_t uoffset, uint32_t sizeOfOperation, uint8_t memoryIndex)
    {
        if (WTF::sumOverflows<uint64_t>(static_cast<uint64_t>(sizeOfOperation), uoffset)) {''', marker='WP81: the pointer of an atomic access')
edit(bbqh, '''        consume(pointer);
        return Location::fromGPR(wasmScratchGPR);
    }

    template<typename Functor>
    auto emitCheckAndPrepareAndMaterializePointerApply(''', '''        consume(pointer);
        return Location::fromGPR(wasmScratchGPR);
    }
#endif // USE(JSVALUE32_64)

    template<typename Functor>
    auto emitCheckAndPrepareAndMaterializePointerApply(''')
print('done 5')

# 6. BBQ only: a thread that enters a function another thread is compiling has nothing to run meanwhile, it waits.
slow = os.path.join(wasm, 'WasmIPIntSlowPaths.cpp')
edit(slow, '''    bool compile = false;
    {
        Locker locker { tierUpCounter.m_lock };
        switch (tierUpCounter.compilationStatus(memoryMode)) {
        case Wasm::IPIntTierUpCounter::CompilationStatus::NotCompiled:
            compile = true;
            tierUpCounter.setCompilationStatus(memoryMode, Wasm::IPIntTierUpCounter::CompilationStatus::Compiling);
            break;
        case Wasm::IPIntTierUpCounter::CompilationStatus::Compiling:
            tierUpCounter.optimizeAfterWarmUp();
            break;''', '''    bool compile = false;
    bool waitForOtherThread = false;
    {
        Locker locker { tierUpCounter.m_lock };
        switch (tierUpCounter.compilationStatus(memoryMode)) {
        case Wasm::IPIntTierUpCounter::CompilationStatus::NotCompiled:
            compile = true;
            tierUpCounter.setCompilationStatus(memoryMode, Wasm::IPIntTierUpCounter::CompilationStatus::Compiling);
            break;
        case Wasm::IPIntTierUpCounter::CompilationStatus::Compiling:
            // WP81: without the interpreter (BBQ only, 32 bits) the function has no other code to run.
            if (osrFor == OSRFor::Prologue && !Options::useWasmIPInt()) [[unlikely]]
                waitForOtherThread = true;
            else
                tierUpCounter.optimizeAfterWarmUp();
            break;''', marker='WP81: without the interpreter')
edit(slow, '''    return getReplacement();
}

WASM_IPINT_EXTERN_CPP_DECL(prologue_osr, CallFrame* callFrame)''', '''    if (waitForOtherThread) [[unlikely]] {
        for (;;) {
            if (RefPtr replacement = getReplacement())
                return replacement;
            {
                Locker locker { tierUpCounter.m_lock };
                if (tierUpCounter.compilationStatus(memoryMode) == Wasm::IPIntTierUpCounter::CompilationStatus::Failed)
                    return nullptr;
            }
            WTF::sleep(1_ms);
        }
    }

    return getReplacement();
}

WASM_IPINT_EXTERN_CPP_DECL(prologue_osr, CallFrame* callFrame)''', marker='if (waitForOtherThread) [[unlikely]]')
print('done 6')

# 7. Shared code that rotted on 32 bits after 306366@main.
edit(os.path.join(wasm, 'js', 'JSWebAssemblyInstance.h'), '''#if CPU(ARM)
                    m_memories[i]->size();
#else''', '''#if CPU(ARM)
                    // WP81: no 4 GiB reservation on 32 bits (always bounds checking): the bound is the size.
                    m_memories[i]->memory().size()
#else''')
edit(os.path.join(wasm, 'WasmTable.h'), '''static_assert(sizeof(FuncRefTable::Function) == sizeof(void*) * 4);''',
     '''// WP81: on 32 bits CalleeBits is 8 bytes and the struct is aligned to 8 (24 bytes); BBQ indexes with the real size.
static_assert(sizeof(FuncRefTable::Function) == sizeof(void*) * 4 || sizeof(void*) == 4);''')
print('done 7')

# 8. offlineasm: pcrtoaddr on ARMv7 (upstream made the common file use it unconditionally once ARMv7 had no IPInt):
# the address of a global label, as leap of a label (globaladdr).
edit(os.path.join(jsc, 'offlineasm', 'arm.rb'), '''        if node.is_a? Instruction
            case node.opcode
            when "leai", "leap", "leaq"
                labelRef = node.operands[0]''', '''        if node.is_a? Instruction
            case node.opcode
            when "leai", "leap", "leaq", "pcrtoaddr"
                # WP81: pcrtoaddr (the address of a label) is leap of that label here.
                labelRef = node.operands[0]''')
print('done 8')

# 9. JSPI's stack switching was never written for ARMv7 (useJSPI is off there): its entry points as stubs.
edit(asm, '''# Low-level logic for JSPI
''', '''# Low-level logic for JSPI

if not ARMv7
''', marker='''# Low-level logic for JSPI

if not ARMv7''')
edit(asm, '''    restoreSentinelVMEntryRecordAndReturnWithStackOverflow()


#################################
# 5. Instruction implementation #
#################################
''', '''    restoreSentinelVMEntryRecordAndReturnWithStackOverflow()

else
# WP81: JSPI is off on 32 bits (Options.cpp); the C++ still names these (declared global in the other branch).
_relocateJITReturnPC:
_relocate_jit_return_pc_return_location:
_relocate_jit_return_pc_trampoline:
_getSentinelFrameReturnPC:
_get_sentinel_frame_return_pc_trampoline:
_get_sentinel_frame_return_pc_return_location:
_enterWebAssemblySuspendingFunction:
_pinballHandlerFulfillFunction:
_exit_implanted_slice:
_pinballHandlerRejectFunction:
    break
end # not ARMv7


#################################
# 5. Instruction implementation #
#################################
''', marker='# WP81: JSPI is off on 32 bits')
print('done 9')

# 10. More 64-bit-only code under shared headers: an FP immediate check, and the restore frame of cross-instance tail
# calls (64-bit pairs and a 64-bit boxed callee), which 32 bits do without (17).
edit(os.path.join(jsc, 'b3', 'air', 'AirArg.h'), '''        auto pattern = X86ContiguousBitPattern64::create(u64);
        if (pattern.isValid())
            return true;
#endif
''', '''        auto pattern = X86ContiguousBitPattern64::create(u64);
        if (pattern.isValid())
            return true;
#else
        // WP81: 32-bit ARM returned false above; this only has to compile.
        uint64_t u64 = static_cast<uint64_t>(value);
#endif
''')
helpers = os.path.join(wasm, 'WasmIRGeneratorHelpers.h')
RESTORE_FRAME_HEADER = '''inline void emitRestoreInstanceFrameIfNeeded(CCallHelpers& jit, GPRReg currentInstanceReg, Checked<int32_t> callerStackSize, Checked<int32_t> frameSize, Checked<int32_t> topSourceOffsetFromFP, GPRReg scratch1, GPRReg scratch2)
{
'''
RESTORE_FRAME_32 = '''#if USE(JSVALUE32_64)
    // WP81: no restore frame on 32 bits. Every caller of Wasm code there reloads the instance from its own frame after
    // a call (BBQ after every call, see BBQJIT::addCall; the JS-to-Wasm entries always), so a callee that tail called
    // into another instance may return with that instance in the register. Upstream's restore frame serves callers
    // that keep the instance and the memory registers across a call (IPInt, OMG): none on this CPU.
    UNUSED_PARAM(jit);
    UNUSED_PARAM(currentInstanceReg);
    UNUSED_PARAM(callerStackSize);
    UNUSED_PARAM(frameSize);
    UNUSED_PARAM(topSourceOffsetFromFP);
    UNUSED_PARAM(scratch1);
    UNUSED_PARAM(scratch2);
#else
'''
# Trees patched while tail calls were off on 32 bits had a stub that could not be reached.
upgrade(helpers, '''#if USE(JSVALUE32_64)
    // WP81: tail calls are off on 32 bits (Options.cpp), so no cross-instance tail call is compiled.
    UNUSED_PARAM(jit);
    UNUSED_PARAM(currentInstanceReg);
    UNUSED_PARAM(callerStackSize);
    UNUSED_PARAM(frameSize);
    UNUSED_PARAM(topSourceOffsetFromFP);
    UNUSED_PARAM(scratch1);
    UNUSED_PARAM(scratch2);
    RELEASE_ASSERT_NOT_REACHED();
#else
''', RESTORE_FRAME_32)
edit(helpers, RESTORE_FRAME_HEADER, RESTORE_FRAME_HEADER + RESTORE_FRAME_32)
text = open(helpers).read()
# close the #else at the end of that function: the first "\n}\n" after the marker
marker = '    // WP81: no restore frame on 32 bits.'
if '#endif // USE(JSVALUE32_64) (restore frame)' not in text:
    i = text.index(marker)
    j = text.index('\n}\n', i)
    text = text[:j] + '\n#endif // USE(JSVALUE32_64) (restore frame)' + text[j:]
    open(helpers, 'w').write(text)
    print('edited', os.path.relpath(helpers, root))
# Tail calls were off on 32 bits until 17.
upgrade(os.path.join(jsc, 'runtime', 'Options.cpp'), '''    Options::useWasmFastMemory() = false;
    Options::useWasmTailCalls() = false;
#endif''', '''    Options::useWasmFastMemory() = false;
#endif''')
print('done 10')

# 11. IPInt::initialize: nothing to set up on ARMv7 (no instruction is dispatched).
edit(os.path.join(jsc, 'llint', 'InPlaceInterpreter.cpp'), '''    FOR_EACH_IPINT_UINT_OPCODE(VALIDATE_IPINT_UINT_OPCODE);
#else
    RELEASE_ASSERT_NOT_REACHED("IPInt only supports ARM64 and X86_64 (for now).");
#endif''', '''    FOR_EACH_IPINT_UINT_OPCODE(VALIDATE_IPINT_UINT_OPCODE);
#elif CPU(ARM_THUMB2)
    // WP81: WebAssembly runs in BBQ only on ARMv7 (InPlaceInterpreter32_64.asm): no dispatch table to set up.
#else
    RELEASE_ASSERT_NOT_REACHED("IPInt only supports ARM64 and X86_64 (for now).");
#endif''')
print('done 11')

# 12. C operations that take memory64-wide (uint64_t / int64_t) addresses and deltas: on ARM an i64 argument is an
# even/odd register pair (AAPCS), so BBQ's 32-bit values land in the wrong registers. 32-bit BBQ calls 32-bit forms
# (memory64 is off on 32 bits).
ops_h = os.path.join(wasm, 'WasmOperations.h')
edit(ops_h, '''JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationWasmMemoryInit, UCPUStrictInt32, (JSWebAssemblyInstance*, unsigned dataSegmentIndex, uint64_t dstAddress, uint32_t srcAddress, uint32_t length, uint8_t memoryIndex));
''', '''JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationWasmMemoryInit, UCPUStrictInt32, (JSWebAssemblyInstance*, unsigned dataSegmentIndex, uint64_t dstAddress, uint32_t srcAddress, uint32_t length, uint8_t memoryIndex));
#if USE(JSVALUE32_64)
// WP81: the forms 32-bit BBQ calls (32-bit memories only), see WasmBBQJIT.cpp.
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationGrowMemory32, int32_t, (JSWebAssemblyInstance*, uint32_t delta, uint8_t memoryIndex));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationWasmMemoryFill32, UCPUStrictInt32, (JSWebAssemblyInstance*, uint32_t dstAddress, uint32_t targetValue, uint32_t count, uint8_t memoryIndex));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationWasmMemoryCopy32, UCPUStrictInt32, (JSWebAssemblyInstance*, uint32_t dstAddress, uint32_t srcAddress, uint32_t count, uint8_t dstMemoryIndex, uint8_t srcMemoryIndex));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationWasmMemoryInit32, UCPUStrictInt32, (JSWebAssemblyInstance*, unsigned dataSegmentIndex, uint32_t dstAddress, uint32_t srcAddress, uint32_t length, uint8_t memoryIndex));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationMemoryAtomicWait32For32, UCPUStrictInt32, (JSWebAssemblyInstance*, uint32_t base, uint32_t offset, int32_t value, int64_t timeout, uint8_t memoryIndex));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationMemoryAtomicWait64For32, UCPUStrictInt32, (JSWebAssemblyInstance*, uint32_t base, uint32_t offset, int64_t value, int64_t timeout, uint8_t memoryIndex));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationMemoryAtomicNotify32, UCPUStrictInt32, (JSWebAssemblyInstance*, uint32_t base, uint32_t offset, int32_t count, uint8_t memoryIndex));
#endif
''')
ops_cpp = os.path.join(wasm, 'WasmOperations.cpp')
edit(ops_cpp, '''JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationWasmDataDrop, void, (JSWebAssemblyInstance* instance, unsigned dataSegmentIndex))''', '''#if USE(JSVALUE32_64)
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationGrowMemory32, int32_t, (JSWebAssemblyInstance* instance, uint32_t delta, uint8_t memoryIndex))
{
    CallFrame* callFrame = DECLARE_WASM_CALL_FRAME(instance);
    assertCalleeIsReferenced(callFrame, instance);
    VM& vm = instance->vm();
    WasmOperationPrologueCallFrameTracer tracer(vm, callFrame, OUR_RETURN_ADDRESS);
    // memory.grow of a 32-bit memory: the delta is a page count (unsigned), the result the old count or -1.
    return static_cast<int32_t>(growMemory(instance, static_cast<int64_t>(delta), memoryIndex));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationWasmMemoryFill32, UCPUStrictInt32, (JSWebAssemblyInstance* instance, uint32_t dstAddress, uint32_t targetValue, uint32_t count, uint8_t memoryIndex))
{
    return toUCPUStrictInt32(memoryFill(instance, dstAddress, targetValue, count, memoryIndex));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationWasmMemoryCopy32, UCPUStrictInt32, (JSWebAssemblyInstance* instance, uint32_t dstAddress, uint32_t srcAddress, uint32_t count, uint8_t dstMemoryIndex, uint8_t srcMemoryIndex))
{
    return toUCPUStrictInt32(memoryCopy(instance, dstAddress, srcAddress, count, dstMemoryIndex, srcMemoryIndex));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationWasmMemoryInit32, UCPUStrictInt32, (JSWebAssemblyInstance* instance, unsigned dataSegmentIndex, uint32_t dstAddress, uint32_t srcAddress, uint32_t length, uint8_t memoryIndex))
{
    return toUCPUStrictInt32(memoryInit(instance, dataSegmentIndex, dstAddress, srcAddress, length, memoryIndex));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationMemoryAtomicWait32For32, UCPUStrictInt32, (JSWebAssemblyInstance* instance, uint32_t base, uint32_t offset, int32_t value, int64_t timeoutInNanoseconds, uint8_t memoryIndex))
{
    return toUCPUStrictInt32(memoryAtomicWait32(instance, base, offset, value, timeoutInNanoseconds, memoryIndex));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationMemoryAtomicWait64For32, UCPUStrictInt32, (JSWebAssemblyInstance* instance, uint32_t base, uint32_t offset, int64_t value, int64_t timeoutInNanoseconds, uint8_t memoryIndex))
{
    return toUCPUStrictInt32(memoryAtomicWait64(instance, base, offset, value, timeoutInNanoseconds, memoryIndex));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationMemoryAtomicNotify32, UCPUStrictInt32, (JSWebAssemblyInstance* instance, uint32_t base, uint32_t offset, int32_t countValue, uint8_t memoryIndex))
{
    return toUCPUStrictInt32(memoryAtomicNotify(instance, base, offset, countValue, memoryIndex));
}
#endif

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationWasmDataDrop, void, (JSWebAssemblyInstance* instance, unsigned dataSegmentIndex))''')

bbq = os.path.join(wasm, 'WasmBBQJIT.cpp')
edit(bbq, '''    emitCCall(&operationGrowMemory, arguments, result);''', '''#if USE(JSVALUE32_64)
    emitCCall(&operationGrowMemory32, arguments, result);
#else
    emitCCall(&operationGrowMemory, arguments, result);
#endif''')
edit(bbq, '''    emitCCall(&operationWasmMemoryFill, arguments, shouldThrow);''', '''#if USE(JSVALUE32_64)
    emitCCall(&operationWasmMemoryFill32, arguments, shouldThrow);
#else
    emitCCall(&operationWasmMemoryFill, arguments, shouldThrow);
#endif''')
edit(bbq, '''    emitCCall(&operationWasmMemoryCopy, arguments, shouldThrow);''', '''#if USE(JSVALUE32_64)
    emitCCall(&operationWasmMemoryCopy32, arguments, shouldThrow);
#else
    emitCCall(&operationWasmMemoryCopy, arguments, shouldThrow);
#endif''')
edit(bbq, '''    emitCCall(&operationWasmMemoryInit, arguments, shouldThrow);''', '''#if USE(JSVALUE32_64)
    emitCCall(&operationWasmMemoryInit32, arguments, shouldThrow);
#else
    emitCCall(&operationWasmMemoryInit, arguments, shouldThrow);
#endif''')
edit(bbq, '''    Vector<Value, 8> arguments = {
        instanceValue(),
        pointer,
        Value::fromI64(uoffset),
        value,
        timeout,
        Value::fromI32(memoryIndex)
    };

    result = topValue(TypeKind::I32);
    if (op == ExtAtomicOpType::MemoryAtomicWait32)
        emitCCall(&operationMemoryAtomicWait32, arguments, result);
    else
        emitCCall(&operationMemoryAtomicWait64, arguments, result);''', '''    Vector<Value, 8> arguments = {
        instanceValue(),
        pointer,
#if USE(JSVALUE32_64)
        Value::fromI32(static_cast<uint32_t>(uoffset)),
#else
        Value::fromI64(uoffset),
#endif
        value,
        timeout,
        Value::fromI32(memoryIndex)
    };

    result = topValue(TypeKind::I32);
#if USE(JSVALUE32_64)
    if (op == ExtAtomicOpType::MemoryAtomicWait32)
        emitCCall(&operationMemoryAtomicWait32For32, arguments, result);
    else
        emitCCall(&operationMemoryAtomicWait64For32, arguments, result);
#else
    if (op == ExtAtomicOpType::MemoryAtomicWait32)
        emitCCall(&operationMemoryAtomicWait32, arguments, result);
    else
        emitCCall(&operationMemoryAtomicWait64, arguments, result);
#endif''')
edit(bbq, '''    Vector<Value, 8> arguments = {
        instanceValue(),
        pointer,
        Value::fromI64(uoffset),
        count,
        Value::fromI32(memoryIndex)
    };
    result = topValue(TypeKind::I32);
    emitCCall(&operationMemoryAtomicNotify, arguments, result);''', '''    Vector<Value, 8> arguments = {
        instanceValue(),
        pointer,
#if USE(JSVALUE32_64)
        Value::fromI32(static_cast<uint32_t>(uoffset)),
#else
        Value::fromI64(uoffset),
#endif
        count,
        Value::fromI32(memoryIndex)
    };
    result = topValue(TypeKind::I32);
#if USE(JSVALUE32_64)
    emitCCall(&operationMemoryAtomicNotify32, arguments, result);
#else
    emitCCall(&operationMemoryAtomicNotify, arguments, result);
#endif''')
print('done 12')

# 13. select: upstream's conditional-move path (added after 306366@main) takes one GPR per operand. An i64 or a reference
# is a pair of registers on 32 bits: one arm per operand there, each reading only its own source (the result's pair may
# share a register with either operand, in either half).
edit(bbq, '''        TypeKind type = lhs.type();
        if (!lhs.isConst() && !rhs.isConst() && type != TypeKind::V128) {
            consume(condition);''', '''        TypeKind type = lhs.type();
#if USE(JSVALUE32_64)
        if (typeNeedsGPR2(type)) {
            ASSERT(conditionLocation.asGPR() != resultLocation.asGPRlo() && conditionLocation.asGPR() != resultLocation.asGPRhi());
            Jump ifZero = m_jit.branchTest32(ResultCondition::Zero, conditionLocation.asGPR(), conditionLocation.asGPR());
            consume(condition);
            if (lhs.isConst())
                emitMoveConst(lhs, resultLocation);
            else
                emitMove(type, lhsLocation, resultLocation);
            Jump done = m_jit.jump();
            ifZero.link(&m_jit);
            if (rhs.isConst())
                emitMoveConst(rhs, resultLocation);
            else
                emitMove(type, rhsLocation, resultLocation);
            done.link(&m_jit);
        } else
#endif
        if (!lhs.isConst() && !rhs.isConst() && type != TypeKind::V128) {
            consume(condition);''')
print('done 13')

# 14. The fault handler (shared memories trap by a fault past the committed pages) resumes at a trampoline by writing
# the saved PC. A Thumb code pointer has bit 0 set; a saved PC is the bare address, the Thumb state is in the saved
# CPSR (set already: the faulting instruction is JIT code, Thumb too).
edit(os.path.join(jsc, 'runtime', 'MachineContext.h'), '''    __darwin_arm_thread_state64_set_pc_fptr(regs, value.taggedPtr());
#else
    instructionPointerImpl(regs) = value.taggedPtr();
#endif''', '''    __darwin_arm_thread_state64_set_pc_fptr(regs, value.taggedPtr());
#elif CPU(ARM_THUMB2)
    instructionPointerImpl(regs) = std::bit_cast<void*>(std::bit_cast<uintptr_t>(value.taggedPtr()) & ~static_cast<uintptr_t>(1));
#else
    instructionPointerImpl(regs) = value.taggedPtr();
#endif''')
print('done 14')

# 15. A maximum of 65536 pages is 4 GiB: not a size_t on 32 bits. A resizable buffer's maxByteLength saturates at the
# largest page-aligned buffer size (it wrapped to 0); a shared memory, which reserves its maximum, fails as out of
# memory (it reserved 0 bytes and then protected a negative range).
edit(os.path.join(wasm, 'js', 'JSWebAssemblyMemory.cpp'),
     '''            size_t maxByteLength = memoryMax.isValid() ? memoryMax.bytes() : defaultMaxByteLengthIfMemoryHasNoMax;''',
     '''            size_t maxByteLength = memoryMax.isValid() ? static_cast<size_t>(std::min<uint64_t>(memoryMax.bytes(), defaultMaxByteLengthIfMemoryHasNoMax)) : defaultMaxByteLengthIfMemoryHasNoMax;''')
edit(os.path.join(wasm, 'WasmMemory.cpp'), '''    case MemorySharingMode::Shared: {
        char* slowMemory = nullptr;
        tryAllocate(vm,''', '''    case MemorySharingMode::Shared: {
        char* slowMemory = nullptr;
        if (maximumBytes > std::numeric_limits<size_t>::max() - PageCount::pageSize) {
            BufferMemoryManager::singleton().freePhysicalBytes(initialBytes);
            return nullptr; // The reservation cannot exist in this address space.
        }
        tryAllocate(vm,''')
print('done 15')

# 16. JS string builtins (on by default): the call trampolines pass the instance in the register after the arguments,
# one register per argument. On ARMv7 a reference is a pair of registers in both conventions, an i32 is the payload
# register of its pair in Wasm's and one register in C's, and the third Wasm argument is on the stack: one marshalling
# per signature shape. The callee slot gets its tag, and the callee table is of CalleeBits (8 bytes), not pointers.
edit(os.path.join(wasm, 'js', 'WebAssemblyBuiltin.h'),
     '''#define CALLEE_ENTRY(setName, builtinName, type) void* BUILTIN_FULL_NAME(setName, builtinName);''',
     '''#define CALLEE_ENTRY(setName, builtinName, type) CalleeBits BUILTIN_FULL_NAME(setName, builtinName);''')
edit(asm, '''macro wasmBuiltinCallTrampoline(setName, builtinName, wasmInstanceArgGPR)
    functionPrologue()
''', '''
# ARMv7: the C arguments of each signature shape (R a reference, I an i32), from Wasm's: the first two arguments in
# a0:a1 and a2:a3 (an i32 in a0 or a2), the third in the first stack slot after the
# header and the slot of "this", FirstArgumentOffset[cfr]. C takes a reference in a0:a1 or a2:a3,
# an i32 in the next register, and the rest (the instance last) on the stack.
macro wasmBuiltinArgumentsR()
    move wasmInstance, a2
end

macro wasmBuiltinArgumentsI()
    move wasmInstance, a1
end

macro wasmBuiltinArgumentsRR()
    subp 8, sp
    storep wasmInstance, [sp]
end

macro wasmBuiltinArgumentsRI()
    move wasmInstance, a3
end

macro wasmBuiltinArgumentsRII()
    loadi FirstArgumentOffset[cfr], a3
    subp 8, sp
    storep wasmInstance, [sp]
end

macro wasmBuiltinArgumentsRRI()
    loadi FirstArgumentOffset[cfr], t5
    subp 8, sp
    storei t5, [sp]
    storep wasmInstance, 4[sp]
end

macro wasmBuiltinCallTrampoline(setName, builtinName, wasmInstanceArgGPR, argumentsARMv7)
    functionPrologue()
''')
edit(asm, '''    storep t5, Callee[cfr]
    storep wasmInstance, CodeBlock[cfr]

    # Set VM topCallFrame to null to not build an unnecessary stack trace if the function throws an exception.
    loadp JSWebAssemblyInstance::m_vm[wasmInstance], t5
    storep 0, VM::topCallFrame[t5]

    # Add the extra wasmInstance arg
    move wasmInstance, wasmInstanceArgGPR
    call _wasm_builtin__%setName%__%builtinName%  # [!] BUILTIN_WASM_ENTRY_NAME(setName, builtinName)
''', '''    storep t5, Callee[cfr]
if ARMv7
    storei (constexpr JSValue::NativeCalleeTag), Callee + TagOffset[cfr]
end
    storep wasmInstance, CodeBlock[cfr]

    # Set VM topCallFrame to null to not build an unnecessary stack trace if the function throws an exception.
    loadp JSWebAssemblyInstance::m_vm[wasmInstance], t5
    storep 0, VM::topCallFrame[t5]

    # Add the extra wasmInstance arg
if ARMv7
    argumentsARMv7()
    call _wasm_builtin__%setName%__%builtinName%  # [!] BUILTIN_WASM_ENTRY_NAME(setName, builtinName)
    move cfr, sp
else
    move wasmInstance, wasmInstanceArgGPR
    call _wasm_builtin__%setName%__%builtinName%  # [!] BUILTIN_WASM_ENTRY_NAME(setName, builtinName)
end
''')
edit(asm, '''macro defineWasmBuiltinTrampoline(setName, builtinName, wasmInstanceArgGPR)
global _wasm_builtin_trampoline__%setName%__%builtinName%    # [!] BUILTIN_TRAMPOLINE_NAME(setName, builtinName)
_wasm_builtin_trampoline__%setName%__%builtinName%:
    wasmBuiltinCallTrampoline(setName, builtinName, wasmInstanceArgGPR)
end''', '''macro defineWasmBuiltinTrampoline(setName, builtinName, wasmInstanceArgGPR, argumentsARMv7)
global _wasm_builtin_trampoline__%setName%__%builtinName%    # [!] BUILTIN_TRAMPOLINE_NAME(setName, builtinName)
_wasm_builtin_trampoline__%setName%__%builtinName%:
    wasmBuiltinCallTrampoline(setName, builtinName, wasmInstanceArgGPR, argumentsARMv7)
end''')
for name, reg, shape in (('cast', 'a1', 'R'), ('test', 'a1', 'R'), ('fromCharCodeArray', 'a3', 'RII'),
                         ('intoCharCodeArray', 'a3', 'RRI'), ('fromCharCode', 'a1', 'I'), ('fromCodePoint', 'a1', 'I'),
                         ('charCodeAt', 'a2', 'RI'), ('codePointAt', 'a2', 'RI'), ('length', 'a1', 'R'),
                         ('concat', 'a2', 'RR'), ('substring', 'a3', 'RII'), ('equals', 'a2', 'RR'), ('compare', 'a2', 'RR')):
    edit(asm, f'defineWasmBuiltinTrampoline(jsstring, {name}, {reg})\n',
         f'defineWasmBuiltinTrampoline(jsstring, {name}, {reg}, wasmBuiltinArgumentsARMv7{shape})\n'.replace('ArgumentsARMv7', 'Arguments'))
print('done 16')

# 17. Tail calls on 32 bits. BBQ's tail calls are common code (WasmBBQJIT.cpp); what 32 bits lacked is the restore
# frame of a cross-instance tail call (10). Without it, a direct call (same instance, the instance register kept by
# the callee) is followed by a reload of the instance, as an import call or an indirect call already is.
edit(bbq, '''    // Our callee could have tail called someone else and changed SP so we need to restore it.
    m_jit.subPtr(GPRInfo::callFrameRegister, TrustedImm32(m_frameSize), MacroAssembler::stackPointerRegister);

    if (m_info.isImportedFunctionFromFunctionIndexSpace(functionIndexSpace))
        restoreWebAssemblyGlobalStateAfterWasmCall();
''', '''    // Our callee could have tail called someone else and changed SP so we need to restore it.
    m_jit.subPtr(GPRInfo::callFrameRegister, TrustedImm32(m_frameSize), MacroAssembler::stackPointerRegister);

#if USE(JSVALUE32_64)
    // WP81: no restore frame on 32 bits (WasmIRGeneratorHelpers.h): a callee that tail called into another instance
    // returns with that instance in the register.
    if (m_info.isImportedFunctionFromFunctionIndexSpace(functionIndexSpace) || Options::useWasmTailCalls())
#else
    if (m_info.isImportedFunctionFromFunctionIndexSpace(functionIndexSpace))
#endif
        restoreWebAssemblyGlobalStateAfterWasmCall();
''')
print('done 17')

# 18. B3 is built on 32 bits for what BBQ takes from it (step 1), and B3Generate.cpp's generateToAir names lowerToAir,
# whose body is 64-bit only (upstream's 32-bit lowering went with 306366@main). The Linux test jsc links anyway (a
# shared library: never called, never resolved); webkit.dll does not. Nothing lowers a B3 procedure on 32 bits (no FTL,
# no OMG): a stand-in that is never reached.
edit(os.path.join(jsc, 'b3', 'B3LowerToAir.cpp'), '''WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(JSVALUE64)
''', '''WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#else // USE(JSVALUE64)

namespace JSC { namespace B3 {

// WP81: B3 is built on 32 bits for the types and address forms BBQ takes from it; nothing lowers a B3 procedure there
// (no FTL, no OMG). Upstream's 32-bit lowering went with 306366@main; B3Generate.cpp still names this.
void lowerToAir(Procedure&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::B3

#endif // USE(JSVALUE64)
''')
print('done 18')
