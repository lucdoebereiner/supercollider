// sc_rust_shim.cpp — the REAL backend for sc_host.h, wired to sclang.
//
// Compiled inside SuperCollider's build (it needs the SC headers), this file is
// the single place that knows sclang's actual PyrSlot/PyrObject/VMGlobals/GC
// layout. The Rust crate only ever sees the thin C ABI declared in sc_host.h.
//
// Integration (3 edits inside the SC tree — see integration/README.md):
//   1. add this file + the Rust static lib to lang/CMakeLists.txt
//   2. call sc_rust_register_all() from initPrimitives() in PyrPrimitive.cpp
//   3. drop the .sc class files into your extensions dir
//
// NOTE: this file is NOT compiled by the standalone build; it is provided as the
// production wiring. Field/function names match SuperCollider as of this writing
// (lang/LangSource/PyrSlot64.h, PyrObject.h, GC.h, PyrPrimitive.h).

#include "sc_host.h"

// SuperCollider language headers.
#include "GC.h"
#include "PyrKernel.h"
#include "PyrObject.h"
#include "PyrPrimitive.h"
#include "PyrSignal.h"
#include "SCBase.h"
#include "VMGlobals.h"

#include <cstring>

static inline VMGlobals* vm(ScVm* g) { return reinterpret_cast<VMGlobals*>(g); }
static inline PyrObject* obj(ScObj* o) { return reinterpret_cast<PyrObject*>(o); }

extern "C" {

// --- registration --------------------------------------------------------
// All Rust primitives share one contiguous block of indices, allocated lazily
// on the first registration (mirrors how each C++ primitive group is set up).
void sc_define_primitive(const char* name, ScPrimFn fn, int num_args, int var_args) {
    static int base = -1;
    static int index = 0;
    if (base < 0)
        base = nextPrimitiveIndex();
    definePrimitive(base, index++, name, reinterpret_cast<PrimitiveHandler>(fn), num_args, var_args);
}

// --- stack ---------------------------------------------------------------
ScSlot* sc_stack_ptr(ScVm* g) { return reinterpret_cast<ScSlot*>(vm(g)->sp); }

// --- allocation ----------------------------------------------------------
ScObj* sc_new_array(ScVm* g, int size) {
    PyrObject* a = newPyrArray(vm(g)->gc, size, 0, true);
    a->size = 0; // the Rust ArrayBuilder fills then calls sc_obj_set_size
    return reinterpret_cast<ScObj*>(a);
}

ScObj* sc_new_string(ScVm* g, const unsigned char* bytes, int len) {
    PyrString* s = newPyrStringN(vm(g)->gc, len, 0, true);
    if (len > 0)
        memcpy(s->s, bytes, len);
    s->size = len;
    return reinterpret_cast<ScObj*>(s);
}

ScObj* sc_new_signal(ScVm* g, int size) {
    PyrObject* s = newPyrSignal(vm(g), size);
    s->size = size;
    return reinterpret_cast<ScObj*>(s);
}

// --- object access -------------------------------------------------------
// PyrObject::slots, PyrString::s and PyrFloatArray::f all sit at the same offset
// (right after the header), so one base accessor serves slots, bytes and floats.
ScSlot* sc_obj_slots(ScObj* o) { return reinterpret_cast<ScSlot*>(obj(o)->slots); }
float* sc_obj_float_data(ScObj* o) { return reinterpret_cast<float*>(obj(o)->slots); }
int sc_obj_size(ScObj* o) { return obj(o)->size; }
void sc_obj_set_size(ScObj* o, int n) { obj(o)->size = n; }
int sc_obj_is_string(ScObj* o) { return obj(o)->classptr == class_string ? 1 : 0; }
int sc_obj_is_signal(ScObj* o) {
    PyrClass* c = obj(o)->classptr;
    return (c == class_signal || c == class_floatarray) ? 1 : 0;
}

// --- garbage collector ---------------------------------------------------
void sc_gc_write(ScVm* g, ScObj* parent, ScSlot* slot) {
    vm(g)->gc->GCWrite(reinterpret_cast<PyrObjectHdr*>(parent), reinterpret_cast<PyrSlot*>(slot));
}
void sc_gc_enter_delayed(ScVm* g) { vm(g)->gc->enterDelayedCollectionContext(); }
void sc_gc_exit_delayed(ScVm* g) { vm(g)->gc->exitDelayedCollectionContext(); }

// --- foreign objects -----------------------------------------------------
void sc_install_finalizer(ScVm* g, ScObj* o, int slot_index, ScFinalizerFn fn) {
    InstallFinalizer(vm(g), obj(o), slot_index, reinterpret_cast<ObjFuncPtr>(fn));
}

// --- diagnostics ---------------------------------------------------------
void sc_post(const char* msg) { post("%s", msg); }

} // extern "C"
