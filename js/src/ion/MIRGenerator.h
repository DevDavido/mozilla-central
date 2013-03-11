/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsion_mirgen_h__
#define jsion_mirgen_h__

// This file declares the data structures used to build a control-flow graph
// containing MIR.
#include <stdarg.h>

#include "jscntxt.h"
#include "jscompartment.h"
#include "IonAllocPolicy.h"
#include "IonCompartment.h"
#include "CompileInfo.h"
#include "RegisterSets.h"

namespace js {
namespace ion {

class MBasicBlock;
class MIRGraph;
class MStart;

struct AsmJSGlobalAccess
{
    unsigned offset;
    unsigned globalDataOffset;

    AsmJSGlobalAccess(unsigned offset, unsigned globalDataOffset)
      : offset(offset), globalDataOffset(globalDataOffset)
    {}
};

class MIRGenerator
{
  public:
    MIRGenerator(JSCompartment *compartment, TempAllocator *temp, MIRGraph *graph, CompileInfo *info);

    TempAllocator &temp() {
        return *temp_;
    }
    MIRGraph &graph() {
        return *graph_;
    }
    bool ensureBallast() {
        return temp().ensureBallast();
    }
    IonCompartment *ionCompartment() const {
        return compartment->ionCompartment();
    }
    CompileInfo &info() {
        return *info_;
    }

    template <typename T>
    T * allocate(size_t count = 1) {
        return reinterpret_cast<T *>(temp().allocate(sizeof(T) * count));
    }

    // Set an error state and prints a message. Returns false so errors can be
    // propagated up.
    bool abort(const char *message, ...);
    bool abortFmt(const char *message, va_list ap);

    bool errored() const {
        return error_;
    }

    bool instrumentedProfiling() {
        return compartment->rt->spsProfiler.enabled();
    }

    // Whether the main thread is trying to cancel this build.
    bool shouldCancel(const char *why) {
        return cancelBuild_;
    }
    void cancel() {
        cancelBuild_ = 1;
    }

    bool compilingAsmJS() const {
        return info_->script() == NULL;
    }

    uint32_t maxAsmStackArgBytes() const {
        JS_ASSERT(compilingAsmJS());
        return maxAsmStackArgBytes_;
    }
    uint32_t resetAsmMaxStackArgBytes() {
        JS_ASSERT(compilingAsmJS());
        uint32_t old = maxAsmStackArgBytes_;
        maxAsmStackArgBytes_ = 0;
        return old;
    }
    void setAsmMaxStackArgBytes(uint32_t n) {
        JS_ASSERT(compilingAsmJS());
        maxAsmStackArgBytes_ = n;
    }
    void setPerformsAsmCall() {
        JS_ASSERT(compilingAsmJS());
        performsAsmCall_ = true;
    }
    bool performsAsmCall() const {
        JS_ASSERT(compilingAsmJS());
        return performsAsmCall_;
    }
    bool noteAsmLoadHeap(uint32_t offsetBefore, uint32_t offsetAfter, ArrayBufferView::ViewType vt,
                         AnyRegister dest) {
        uint32_t opLength = offsetAfter - offsetBefore;
        uint8_t f32Load = vt == ArrayBufferView::TYPE_FLOAT32;
        JS_ASSERT(opLength <= UINT8_MAX);
        return asmHeapAccesses_.append(AsmJSHeapAccess(offsetBefore, opLength, f32Load, dest));
    }
    bool noteAsmStoreHeap(uint32_t offsetBefore, uint32_t offsetAfter) {
        uint32_t opLength = offsetAfter - offsetBefore;
        JS_ASSERT(opLength <= UINT8_MAX);
        return asmHeapAccesses_.append(AsmJSHeapAccess(offsetBefore, opLength));
    }
    const Vector<AsmJSHeapAccess> &asmHeapAccesses() const {
        return asmHeapAccesses_;
    }
    bool noteAsmGlobalAccess(unsigned offset, unsigned globalDataOffset) {
        return asmGlobalAccesses_.append(AsmJSGlobalAccess(offset, globalDataOffset));
    }
    const Vector<AsmJSGlobalAccess> &asmGlobalAccesses() const {
        return asmGlobalAccesses_;
    }

  public:
    JSCompartment *compartment;

  protected:
    CompileInfo *info_;
    TempAllocator *temp_;
    JSFunction *fun_;
    uint32_t nslots_;
    MIRGraph *graph_;
    bool error_;
    size_t cancelBuild_;

    uint32_t maxAsmStackArgBytes_;
    bool performsAsmCall_;
    Vector<AsmJSHeapAccess> asmHeapAccesses_;
    Vector<AsmJSGlobalAccess> asmGlobalAccesses_;
};

} // namespace ion
} // namespace js

#endif // jsion_mirgen_h__

