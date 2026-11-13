# Copyright (C) 2023-2025 Apple Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
# THE POSSIBILITY OF SUCH DAMAGE.

# ARMv7 (WP81 port): WebAssembly runs in the BBQ JIT only. Options.cpp turns useWasmIPInt off on 32-bit CPUs, which
# sets the BBQ threshold to 0, so ipint_entry tiers up in its prologue: the function is compiled (once, synchronously)
# and entered as if it had been called directly, the interpreter's frame popped first. The interpreter itself (its
# frame of locals, its instructions) never runs here; the instruction labels are the common file's stand-ins for the
# architectures without an interpreter. Upstream's 32-bit interpreter (removed in 306366@main) can come back on top
# of this file.

##########
# Macros #
##########

# Callee saves, where the unwinder reads them (RegisterAtOffsetList::ipintCalleeSaveRegisters, frame-pointer based,
# in register order): csr0 = r10 = wasmInstance at -8, csr1 = r11 = PC at -4. MC (r8) is not a callee save; it is kept
# at -12 for the interpreter's own use.
const IPIntCalleeSaveSpaceWIPC = -8
const IPIntCalleeSaveSpaceMC = -12

macro saveIPIntRegisters()
    # NOTE: We intentionally don't restore memoryBase and boundsCheckingSize here. These are saved
    # and restored when entering Wasm by the JSToWasm wrapper and changes to them are meant
    # to be observable within the same Wasm module.
    subp IPIntCalleeSaveSpaceStackAligned, sp
    store2ia wasmInstance, PC, IPIntCalleeSaveSpaceWIPC[cfr]
    storep MC, IPIntCalleeSaveSpaceMC[cfr]
end

macro restoreIPIntRegisters()
    load2ia IPIntCalleeSaveSpaceWIPC[cfr], wasmInstance, PC
    loadp IPIntCalleeSaveSpaceMC[cfr], MC
    addp IPIntCalleeSaveSpaceStackAligned, sp
end

# No instruction is interpreted on this CPU.
macro nextIPIntInstruction()
    break
end

macro ipintEntry()
    # PC at the first byte of the function: a call site index saved by a slow path below (the stack check's) is then
    # outside every try region.
    loadp Wasm::IPIntCallee::m_bytecode[ws0], PC
    # The scratch must not be an argument register (t0-t3 hold the call's arguments: an i64's high word in t3).
    checkStackOverflow(ws0, ws1)

    # Compile with BBQ (or take the compiled code) and jump to it in place of this frame; the argument registers and
    # the stack arguments are those of the call.
    ipintPrologueOSR(500000)

    # No code: the compilation failed (out of executable memory, mostly). The function cannot run.
    handleDebuggerTrapIfNeededAndThrowWasmTrap(OutOfMemory)
end

# The interpreter's own frame is never built (ipintEntry leaves first): the common file's local initialization path
# is unreachable.
macro argumINTInitializeDefaultLocals()
    break
end

macro argumINTFinish()
    break
end

# The interpreter's call trampolines, named by the LLInt's gate table and the stack walker: an interpreter frame never
# makes a call on this CPU.
_wasm_trampoline_wasm_ipint_call:
_wasm_trampoline_wasm_ipint_call_wide16:
_wasm_trampoline_wasm_ipint_call_wide32:
_wasm_trampoline_wasm_ipint_tail_call:
_wasm_trampoline_wasm_ipint_tail_call_wide16:
_wasm_trampoline_wasm_ipint_tail_call_wide32:
_wasm_ipint_call_return_location:
_wasm_ipint_call_return_location_wide16:
_wasm_ipint_call_return_location_wide32:
    break
