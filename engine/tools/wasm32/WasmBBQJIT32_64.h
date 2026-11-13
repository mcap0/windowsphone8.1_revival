/*
 * Copyright (C) 2019-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(WEBASSEMBLY_BBQJIT)
#if USE(JSVALUE32_64)

#include "WasmBBQJIT.h"
#include "WasmCallingConvention.h"
#include "WasmCompilationContext.h"
#include "WasmExceptionType.h"
#include "WasmFunctionParser.h"
#include "WasmLimits.h"
#include "js/JSWebAssemblyInstance.h"
#include <wtf/CheckedArithmetic.h>
#include <wtf/Scope.h>

namespace JSC { namespace Wasm { namespace BBQJITImpl {

template<typename Functor>
auto BBQJIT::emitCheckAndPrepareAndMaterializePointerApply(Value pointer, uint64_t uoffset, uint32_t sizeOfOperation, uint8_t memoryIndex, Functor&& functor) -> decltype(auto)
{
    // A 32-bit address space holds no memory64 (refused when the module is parsed, Options.cpp), and every byte of a
    // memory is below 4 GiB: an access whose last byte would be at or above it traps whatever the pointer is.
    RELEASE_ASSERT(!m_info.memory(memoryIndex).isMemory64());
    uint64_t boundary = static_cast<uint64_t>(sizeOfOperation) + uoffset - 1;

    ScratchScope<1, 0> scratches(*this);
    Location pointerLocation;

    // No pinned memory registers on this CPU: the base and the size are loaded for each access.
    ScratchScope<2, 0> globals(*this);
    GPRReg wasmBaseMemoryPointer = globals.gpr(0);
    GPRReg wasmBoundsCheckingSizeRegister = globals.gpr(1);
    if (!memoryIndex)
        loadWebAssemblyGlobalState(wasmBaseMemoryPointer, wasmBoundsCheckingSizeRegister);
    else {
        m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, JSWebAssemblyInstance::offsetOfCachedMemoryBaseSizePair(memoryIndex)), wasmBaseMemoryPointer);
        m_jit.loadPtr(Address(GPRInfo::wasmContextInstancePointer, JSWebAssemblyInstance::offsetOfCachedMemoryBaseSizePair(memoryIndex) + sizeof(void*)), wasmBoundsCheckingSizeRegister);
    }

    if (boundary > std::numeric_limits<uint32_t>::max()) {
        recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.jump());
        return functor(CCallHelpers::Address(wasmBaseMemoryPointer, 0));
    }

    // Only memory 0 can be a signaling memory (and on 32 bits none is: no 4 GiB reservation).
    MemoryMode mode = memoryIndex ? MemoryMode::BoundsChecking : m_mode;
    uint64_t maximum = m_info.memory(memoryIndex).maximum() ? m_info.memory(memoryIndex).maximum().bytes() : std::numeric_limits<uint32_t>::max();

    if (pointer.isConst()) {
        uint64_t constantPointer = static_cast<uint64_t>(static_cast<uint32_t>(pointer.asI32()));
        uint64_t lastByte = constantPointer + boundary;
        if (lastByte > std::numeric_limits<uint32_t>::max()) {
            recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.jump());
            return functor(CCallHelpers::Address(wasmBaseMemoryPointer, 0));
        }
        switch (mode) {
        case MemoryMode::BoundsChecking:
            m_jit.move(TrustedImm32(static_cast<uint32_t>(lastByte)), wasmScratchGPR);
            recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, wasmScratchGPR, wasmBoundsCheckingSizeRegister));
            break;
        case MemoryMode::Signaling:
            if (uoffset >= Memory::fastMappedRedzoneBytes() && lastByte >= maximum)
                recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.jump());
            break;
        }
        uint64_t finalOffset = constantPointer + uoffset;
        if (finalOffset <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) && B3::Air::Arg::isValidAddrForm(B3::Air::Move, static_cast<int32_t>(finalOffset), Width::Width128))
            return functor(CCallHelpers::Address(wasmBaseMemoryPointer, static_cast<int32_t>(finalOffset)));
        m_jit.addPtr(TrustedImm32(static_cast<uint32_t>(finalOffset)), wasmBaseMemoryPointer, wasmScratchGPR);
        return functor(Address(wasmScratchGPR));
    }

    pointerLocation = loadIfNecessary(pointer);
    ASSERT(pointerLocation.isGPR());

    GPRReg pointerReg = pointerLocation.asGPR();
    switch (mode) {
    case MemoryMode::BoundsChecking: {
        // Regardless of signaling, no access may exceed the current memory size. pointer + boundary is computed in 32
        // bits: a carry means past 4 GiB, out of bounds.
        if (boundary) {
            recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchAdd32(ResultCondition::Carry, pointerReg, TrustedImm32(static_cast<uint32_t>(boundary)), wasmScratchGPR));
            recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, wasmScratchGPR, wasmBoundsCheckingSizeRegister));
        } else
            recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, pointerReg, wasmBoundsCheckingSizeRegister));
        break;
    }

    case MemoryMode::Signaling: {
        // See the 64-bit version: the redzone catches small offsets, a large one is checked against the declared
        // maximum.
        if (uoffset >= Memory::fastMappedRedzoneBytes()) {
            if (boundary) {
                recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchAdd32(ResultCondition::Carry, pointerReg, TrustedImm32(static_cast<uint32_t>(boundary)), wasmScratchGPR));
                recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, wasmScratchGPR, TrustedImmPtr(static_cast<uintptr_t>(std::min<uint64_t>(maximum, std::numeric_limits<uint32_t>::max())))));
            } else
                recordJumpToThrowException(ExceptionType::OutOfBoundsMemoryAccess, m_jit.branchPtr(RelationalCondition::AboveOrEqual, pointerReg, TrustedImmPtr(static_cast<uintptr_t>(std::min<uint64_t>(maximum, std::numeric_limits<uint32_t>::max())))));
        }
        break;
    }
    }
    m_jit.add32(pointerReg, wasmBaseMemoryPointer, wasmScratchGPR);

    // uoffset < 4 GiB here (boundary fits in 32 bits).
    if (uoffset > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || !B3::Air::Arg::isValidAddrForm(B3::Air::Move, static_cast<int32_t>(uoffset), Width::Width128)) {
        m_jit.add32(TrustedImm32(static_cast<uint32_t>(uoffset)), wasmScratchGPR);
        return functor(Address(wasmScratchGPR));
    }
    return functor(Address(wasmScratchGPR, static_cast<int32_t>(uoffset)));
}

#define PREPARE_FOR_MOD_OR_DIV

template<typename IntType, bool IsMod>
void BBQJIT::emitModOrDiv(Value& lhs, Location lhsLocation, Value& rhs, Location rhsLocation, Value& result, Location)
{
    static_assert(sizeof(IntType) == 4 || sizeof(IntType) == 8);

    constexpr bool isSigned = std::is_signed<IntType>();
    constexpr bool is32 = sizeof(IntType) == 4;

    TypeKind argType, returnType;
    if constexpr (is32)
        argType = returnType = TypeKind::I32;
    else
        argType = returnType = TypeKind::I64;

    IntType (*modOrDiv)(IntType, IntType);
    if constexpr (IsMod) {
        if constexpr (is32) {
            if constexpr (isSigned)
                modOrDiv = Math::i32_rem_s;
            else
                modOrDiv = Math::i32_rem_u;
        } else {
            if constexpr (isSigned)
                modOrDiv = Math::i64_rem_s;
            else
                modOrDiv = Math::i64_rem_u;
        }
    } else {
        if constexpr (is32) {
            if constexpr (isSigned)
                modOrDiv = Math::i32_div_s;
            else
                modOrDiv = Math::i32_div_u;
        } else {
            if constexpr (isSigned)
                modOrDiv = Math::i64_div_s;
            else
                modOrDiv = Math::i64_div_u;
        }
    }

    // A constant operand stays a constant up to the call: the scratch registers are the temporaries of the argument
    // shuffle, nothing lives in them across it.
    if (rhs.isConst()) {
        bool isZero = is32 ? !rhs.asI32() : !rhs.asI64();
        if (isZero)
            recordJumpToThrowException(ExceptionType::DivisionByZero, m_jit.jump());
    } else if constexpr (is32)
        recordJumpToThrowException(ExceptionType::DivisionByZero, m_jit.branchTest32(ResultCondition::Zero, rhsLocation.asGPR()));
    else {
        auto loNotZero = m_jit.branchTest32(ResultCondition::NonZero, rhsLocation.asGPRlo());
        recordJumpToThrowException(ExceptionType::DivisionByZero, m_jit.branchTest32(ResultCondition::Zero, rhsLocation.asGPRhi()));
        loNotZero.link(&m_jit);
    }

    if constexpr (!IsMod && isSigned) {
        // The one overflow: the minimum divided by -1.
        bool rhsMayBeMinusOne = !rhs.isConst() || (is32 ? rhs.asI32() == -1 : rhs.asI64() == -1);
        bool lhsMayBeMinimum = !lhs.isConst() || (is32 ? lhs.asI32() == std::numeric_limits<int32_t>::min() : lhs.asI64() == std::numeric_limits<int64_t>::min());
        if (rhsMayBeMinusOne && lhsMayBeMinimum) {
            JumpList isOk;
            if constexpr (is32) {
                if (!rhs.isConst())
                    isOk.append(m_jit.branch32(RelationalCondition::NotEqual, rhsLocation.asGPR(), TrustedImm32(-1)));
                if (!lhs.isConst())
                    isOk.append(m_jit.branch32(RelationalCondition::NotEqual, lhsLocation.asGPR(), TrustedImm32(std::numeric_limits<int32_t>::min())));
            } else {
                if (!rhs.isConst()) {
                    isOk.append(m_jit.branch32(RelationalCondition::NotEqual, rhsLocation.asGPRlo(), TrustedImm32(-1)));
                    isOk.append(m_jit.branch32(RelationalCondition::NotEqual, rhsLocation.asGPRhi(), TrustedImm32(-1)));
                }
                if (!lhs.isConst()) {
                    isOk.append(m_jit.branchTest32(ResultCondition::NonZero, lhsLocation.asGPRlo()));
                    isOk.append(m_jit.branch32(RelationalCondition::NotEqual, lhsLocation.asGPRhi(), TrustedImm32(std::numeric_limits<int32_t>::min())));
                }
            }
            recordJumpToThrowException(ExceptionType::IntegerOverflow, m_jit.jump());
            isOk.link(&m_jit);
        }
    }

    auto lhsArg = lhs.isConst() ? lhs : Value::pinned(argType, lhsLocation);
    auto rhsArg = rhs.isConst() ? rhs : Value::pinned(argType, rhsLocation);
    consume(result);
    Vector<Value, 8> arguments = { lhsArg, rhsArg };
    emitCCall(modOrDiv, arguments, result);
}

#define PREPARE_FOR_SHIFT

template<size_t N, typename OverflowHandler>
void BBQJIT::emitShuffleMove(Vector<Value, N, OverflowHandler>& srcVector, Vector<Location, N, OverflowHandler>& dstVector, Vector<ShuffleStatus, N, OverflowHandler>& statusVector, unsigned index)
{
    Location srcLocation = locationOf(srcVector[index]);
    Location dst = dstVector[index];
    if (srcLocation == dst)
        return; // Easily eliminate redundant moves here.

    uint32_t dstSize = srcVector[index].size();
    statusVector[index] = ShuffleStatus::BeingMoved;
    for (unsigned i = 0; i < srcVector.size(); i ++) {
        // A pair that overlaps its own destination (r1:r2 to r0:r1) is no cycle: the move orders its halves.
        if (i == index)
            continue;
        // This check should handle constants too - constants always have location None, and no
        // dst should ever be a constant. But we assume that's asserted in the caller.
        auto aliasesDst = [&] (Location loc, uint32_t size) -> bool {
            if (loc.isGPR()) {
                if (dst.isGPR2())
                    return loc.asGPR() == dst.asGPRhi() || loc.asGPR() == dst.asGPRlo();
            } else if (loc.isGPR2()) {
                if (dst.isGPR())
                    return loc.asGPRhi() == dst.asGPR() || loc.asGPRlo() == dst.asGPR();
                if (dst.isGPR2()) {
                    return loc.asGPRhi() == dst.asGPRhi() || loc.asGPRhi() == dst.asGPRlo()
                        || loc.asGPRlo() == dst.asGPRhi() || loc.asGPRlo() == dst.asGPRlo();
                }
            }
            // Stack slots of different sizes (an i64 over two i32 slots) overlap as ranges.
            return Location::rangesOverlap(loc, size, dst, dstSize);
        };
        if (aliasesDst(locationOf(srcVector[i]), srcVector[i].size())) {
            switch (statusVector[i]) {
            case ShuffleStatus::ToMove:
                emitShuffleMove(srcVector, dstVector, statusVector, i);
                break;
            case ShuffleStatus::BeingMoved: {
                Location temp;
                if (srcVector[i].isFloat())
                    temp = Location::fromFPR(wasmScratchFPR);
                else if (typeNeedsGPR2(srcVector[i].type()))
                    temp = Location::fromGPR2(wasmScratchGPR2, wasmScratchGPR);
                else
                    temp = Location::fromGPR(wasmScratchGPR);
                emitMove(srcVector[i], temp);
                srcVector[i] = Value::pinned(srcVector[i].type(), temp);
                break;
            }
            case ShuffleStatus::Moved:
                break;
            }
        }
    }
    emitMove(srcVector[index], dst);
    statusVector[index] = ShuffleStatus::Moved;
}

template<typename Func>
void BBQJIT::emitCCall(Func function, std::span<const Value> arguments)
{
    // Currently, we assume the Wasm calling convention is the same as the C calling convention
    Vector<Type, 16> resultTypes;
    auto argumentTypes = WTF::map<16>(arguments, [](auto& value) {
        return Type { value.type(), 0u };
    });
    Ref<const RTT> functionRTT = TypeInformation::rttForFunction(resultTypes, argumentTypes);
    // C functions take an i64 in an even/odd register pair and the rest on the stack (AAPCS), unlike Wasm calls.
    CallInformation callInfo = cCallingConventionArmThumb2().callInformationFor(functionRTT.get(), CallRole::Caller);
    Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(callInfo.headerAndArgumentStackSizeInBytes);
    m_maxCalleeStackSizeForValidation = std::max<uint32_t>(calleeStackSize, m_maxCalleeStackSizeForValidation);
    ASSERT(static_cast<uint32_t>(alignedFrameSize(m_maxCalleeStackSizeForValidation + m_frameSizeForValidation)) <= m_frameSize);

    // Prepare wasm operation calls.
    m_jit.prepareWasmCallOperation(GPRInfo::wasmContextInstancePointer);

    // Preserve caller-saved registers and other info
    prepareForExceptions();
    saveValuesAcrossCallAndPassArguments(arguments, callInfo, functionRTT.get());

    // Materialize address of native function and call register
    void* taggedFunctionPtr = tagCFunctionPtr<void*, OperationPtrTag>(function);
    m_jit.move(TrustedImmPtr(std::bit_cast<uintptr_t>(taggedFunctionPtr)), wasmScratchGPR);
    m_jit.call(wasmScratchGPR, OperationPtrTag);
}

template<typename Func>
void BBQJIT::emitCCall(Func function, std::span<const Value> arguments, Value& result)
{
    ASSERT(result.isTemp());

    // Currently, we assume the Wasm calling convention is the same as the C calling convention
    Vector<Type, 16> resultTypes = { Type { result.type(), 0u } };
    auto argumentTypes = WTF::map<16>(arguments, [](auto& value) {
        return Type { value.type(), 0u };
    });

    Ref<const RTT> functionRTT = TypeInformation::rttForFunction(resultTypes, argumentTypes);
    // C functions take an i64 in an even/odd register pair and the rest on the stack (AAPCS), unlike Wasm calls.
    CallInformation callInfo = cCallingConventionArmThumb2().callInformationFor(functionRTT.get(), CallRole::Caller);
    Checked<int32_t> calleeStackSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(callInfo.headerAndArgumentStackSizeInBytes);
    m_maxCalleeStackSizeForValidation = std::max<uint32_t>(calleeStackSize, m_maxCalleeStackSizeForValidation);
    ASSERT(static_cast<uint32_t>(alignedFrameSize(m_maxCalleeStackSizeForValidation + m_frameSizeForValidation)) <= m_frameSize);

    // Prepare wasm operation calls.
    m_jit.prepareWasmCallOperation(GPRInfo::wasmContextInstancePointer);

    // Preserve caller-saved registers and other info
    prepareForExceptions();
    saveValuesAcrossCallAndPassArguments(arguments, callInfo, functionRTT.get());

    // Materialize address of native function and call register
    void* taggedFunctionPtr = tagCFunctionPtr<void*, OperationPtrTag>(function);
    m_jit.move(TrustedImmPtr(std::bit_cast<uintptr_t>(taggedFunctionPtr)), wasmScratchGPR);
    m_jit.call(wasmScratchGPR, OperationPtrTag);

    Location resultLocation;
    switch (result.type()) {
    case TypeKind::I32:
        resultLocation = Location::fromGPR(GPRInfo::returnValueGPR);
        break;
    case TypeKind::I31ref:
    case TypeKind::I64:
    case TypeKind::Ref:
    case TypeKind::RefNull:
    case TypeKind::Arrayref:
    case TypeKind::Structref:
    case TypeKind::Funcref:
    case TypeKind::Exnref:
    case TypeKind::Externref:
    case TypeKind::Eqref:
    case TypeKind::Anyref:
    case TypeKind::Noexnref:
    case TypeKind::Noneref:
    case TypeKind::Nofuncref:
    case TypeKind::Noexternref:
    case TypeKind::Rec:
    case TypeKind::Sub:
    case TypeKind::Subfinal:
    case TypeKind::Array:
    case TypeKind::Struct:
    case TypeKind::Func: {
        resultLocation = Location::fromGPR2(GPRInfo::returnValueGPR2, GPRInfo::returnValueGPR);
        ASSERT(validGPRs().contains(GPRInfo::returnValueGPR, IgnoreVectors));
        break;
    }
    case TypeKind::F32:
    case TypeKind::F64: {
        resultLocation = Location::fromFPR(FPRInfo::returnValueFPR);
        ASSERT(validFPRs().contains(FPRInfo::returnValueFPR, Width::Width128));
        break;
    }
    case TypeKind::V128: {
        resultLocation = Location::fromFPR(FPRInfo::returnValueFPR);
        ASSERT(validFPRs().contains(FPRInfo::returnValueFPR, Width::Width128));
        break;
    }
    case TypeKind::Void:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    RegisterBinding currentBinding;
    if (resultLocation.isGPR())
        currentBinding = bindingFor(resultLocation.asGPR());
    else if (resultLocation.isFPR())
        currentBinding = bindingFor(resultLocation.asFPR());
    else if (resultLocation.isGPR2())
        currentBinding = bindingFor(resultLocation.asGPRhi());
    RELEASE_ASSERT(!currentBinding.isScratch());

    bind(result, resultLocation);
}

} } } // namespace JSC::Wasm::BBQJITImpl

#endif // USE(JSVALUE32_64)
#endif // ENABLE(WEBASSEMBLY_BBQJIT)
