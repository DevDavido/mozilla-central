/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(jsion_asmjs_h__)
#define jsion_asmjs_h__

namespace js {

class SPSProfiler;
class AsmJSModule;
namespace frontend { struct TokenStream; struct ParseNode; }

// Called after parsing a function 'fn' which contains the "use asm" directive.
// This function performs type-checking and code-generation. If type-checking
// succeeds, the generated module is assigned to script->asmJS. Otherwise, a
// warning will be emitted and script->asmJS is left null. The function returns
// 'false' only if a real JS semantic error (probably OOM) is pending.
extern bool
CompileAsmJS(JSContext *cx, frontend::TokenStream &ts, frontend::ParseNode *fn, HandleScript s);

// Called by the JSOP_LINKASMJS opcode (which is emitted as the first opcode of
// a "use asm" function which successfully typechecks). This function performs
// the validation and dynamic linking of a module to it's given arguments. If
// validation succeeds, the module's return value (it's exports) are returned
// as an object in 'rval' and the interpreter should return 'rval' immediately.
// Otherwise, there was a validation error and execution should continue
// normally in the interpreter. The function returns 'false' only if a real JS
// semantic error (OOM or exception thrown when executing GetProperty on the
// arguments) is pending.
// (Implemented in AsmJSLink.cpp.)
extern bool
LinkAsmJS(JSContext *cx, StackFrame *fp, MutableHandleValue rval);

// The JSRuntime maintains a stack of AsmJSModule activations. An "activation"
// of module A is an initial call from outside A into a function inside A,
// followed by a sequence of calls inside A, and terminated by a call that
// leaves A. The AsmJSActivation stack serves three purposes:
//  - record the correct cx to pass to VM calls from asm.js;
//  - record enough information to pop all the frames of an activation if an
//    exception is thrown;
//  - record the information necessary for asm.js signal handlers to safely
//    recover from (expected) out-of-bounds access, the operation callback,
//    stack overflow, division by zero, etc.
class AsmJSActivation
{
    AsmJSActivation *prev_;
    JSContext *cx_;
    void *errorRejoinSP_;
    const AsmJSModule &module_;
    unsigned entryIndex_;
    uint8_t *heap_;
    SPSProfiler *profiler_;

  public:
    AsmJSActivation(JSContext *cx, const AsmJSModule &module, unsigned entryIndex, uint8_t *heap);
    ~AsmJSActivation();

    const AsmJSModule &module() const { return module_; }
    uint8_t *heap() const { return heap_; }

    // Read by JIT code:
    static unsigned offsetOfContext() { return offsetof(AsmJSActivation, cx_); }

    // Initialized by JIT code:
    static unsigned offsetOfErrorRejoinSP() { return offsetof(AsmJSActivation, errorRejoinSP_); }
};

// On x64, the internal ArrayBuffer data array is inflated to 4GiB (only the
// byteLength portion of which is accessible) so that out-of-bounds accesses
// (made using a uint32 index) are guaranteed to raise a SIGSEGV.
#ifdef JS_CPU_X64
static const size_t FourGiB = 4 * 1024ULL * 1024ULL * 1024ULL;
#endif

} // namespace js

#endif // jsion_asmjs_h__

