/*
 * sc_host.h — the abstract C ABI between Rust primitives and a SuperCollider host.
 *
 * The Rust crate (sc-prim) depends ONLY on this header's symbols. It never sees
 * a real PyrSlot/PyrObject/VMGlobals layout beyond the 16-byte slot below.
 *
 * There are two implementations of this ABI:
 *   - integration/sc_rust_shim.cpp : the real backend, wired to sclang. Compiled
 *     inside SuperCollider's build (where the SC headers are available).
 *   - examples/mock_host/mock_host.cpp : a standalone backend backed by malloc,
 *     so the whole thing builds and runs without the SC tree.
 *
 * Keeping the boundary this thin is what makes the Rust side robust against
 * SC's internal layout changing between versions: only the shim must track it.
 */
#ifndef SC_HOST_H
#define SC_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles. The Rust side treats these as pointers and nothing more. */
typedef struct ScVm ScVm;     /* == VMGlobals  */
typedef struct ScObj ScObj;   /* == PyrObject  */

/*
 * A value slot. Binary-compatible with sclang's 64-bit PyrSlot
 * (lang/LangSource/PyrSlot64.h): an 8-byte tag followed by an 8-byte union.
 * Tag values (must match SC):
 *   0 notInit, 1 obj, 2 int, 3 sym, 4 char, 5 nil, 6 false, 7 true, 8 ptr, 9 float
 */
typedef struct ScSlot {
    int64_t tag;
    union {
        int64_t i;
        double f;
        void* ptr;
    } u;
} ScSlot;

/* Primitive handler: matches SC's PrimitiveHandler = int(*)(VMGlobals*, int). */
typedef int (*ScPrimFn)(ScVm* g, int num_args_pushed);

/* Finalizer: matches SC's ObjFuncPtr = int(*)(VMGlobals*, PyrObject*). */
typedef int (*ScFinalizerFn)(ScVm* g, ScObj* obj);

/* --- registration -------------------------------------------------------- */
/* Register one primitive. `name` must start with '_' and be NUL-terminated. */
void sc_define_primitive(const char* name, ScPrimFn fn, int num_args, int var_args);

/* --- stack --------------------------------------------------------------- */
/* Top of the argument stack (sclang's g->sp). The receiver of a call with N
 * args pushed lives at sp-(N-1); explicit args follow it up to sp. */
ScSlot* sc_stack_ptr(ScVm* g);

/* --- allocation (objects are GC-owned; never freed by the caller) --------- */
/* A slot array (an `Array`) of `size` slots. Caller fills then sets size. */
ScObj* sc_new_array(ScVm* g, int size);
/* A `String` holding `len` bytes copied from `bytes`. */
ScObj* sc_new_string(ScVm* g, const unsigned char* bytes, int len);

/* --- object access ------------------------------------------------------- */
ScSlot* sc_obj_slots(ScObj* o);       /* start of the element/data region    */
int     sc_obj_size(ScObj* o);
void    sc_obj_set_size(ScObj* o, int n);
int     sc_obj_is_string(ScObj* o);   /* 1 if the object is a String         */

/* --- garbage collector --------------------------------------------------- */
/* Write barrier: call after storing a reference into `parent` at `slot`. */
void sc_gc_write(ScVm* g, ScObj* parent, ScSlot* slot);
/* Defer collection for the duration of a primitive that builds objects. */
void sc_gc_enter_delayed(ScVm* g);
void sc_gc_exit_delayed(ScVm* g);

/* --- foreign objects ----------------------------------------------------- */
/* Install `fn` to run when `obj` is collected. The finalizer reference is
 * stored in instance-var slot `slot_index` of `obj`. */
void sc_install_finalizer(ScVm* g, ScObj* obj, int slot_index, ScFinalizerFn fn);

/* --- diagnostics --------------------------------------------------------- */
void sc_post(const char* msg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SC_HOST_H */
