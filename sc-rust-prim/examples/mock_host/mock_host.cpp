// mock_host.cpp — a standalone implementation of the sc_host.h ABI.
//
// It fakes just enough of sclang (an argument stack, malloc-backed objects, a
// finalizer list) to load the Rust primitives and call them, so the whole
// pipeline — Rust extern "C" <-> C ABI <-> object allocation <-> finalizers —
// builds and runs without the SuperCollider tree. The *real* backend lives in
// integration/sc_rust_shim.cpp and wires the same ABI to sclang.

#include "sc_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Provided by the Rust static library.
extern "C" void sc_rust_register_all();

// ---- object model --------------------------------------------------------

namespace {

struct MockObj {
    int size = 0;
    int is_string = 0;
    std::vector<ScSlot> slots; // arrays (8-byte aligned)
    std::vector<unsigned char> bytes; // strings
};

struct PrimEntry {
    std::string name;
    ScPrimFn fn;
    int num_args;
};

struct Finalizer {
    ScFinalizerFn fn;
    ScObj* obj;
};

struct MockVm {
    ScSlot stack[256];
    ScSlot* sp = stack;
};

std::vector<PrimEntry> g_prims;
std::vector<MockObj*> g_objs;
std::vector<Finalizer> g_finalizers;
MockVm g_vm;

MockObj* as_mock(ScObj* o) { return reinterpret_cast<MockObj*>(o); }
ScObj* as_handle(MockObj* o) { return reinterpret_cast<ScObj*>(o); }

} // namespace

// ---- the sc_host.h ABI ---------------------------------------------------

extern "C" {

void sc_define_primitive(const char* name, ScPrimFn fn, int num_args, int /*var_args*/) {
    g_prims.push_back({ std::string(name), fn, num_args });
}

ScSlot* sc_stack_ptr(ScVm* /*g*/) { return g_vm.sp; }

ScObj* sc_new_array(ScVm* /*g*/, int size) {
    auto* o = new MockObj();
    o->size = size;
    o->is_string = 0;
    o->slots.assign(size > 0 ? size : 0, ScSlot { 0, { 0 } });
    g_objs.push_back(o);
    return as_handle(o);
}

ScObj* sc_new_string(ScVm* /*g*/, const unsigned char* bytes, int len) {
    auto* o = new MockObj();
    o->size = len;
    o->is_string = 1;
    o->bytes.assign(bytes, bytes + (len > 0 ? len : 0));
    g_objs.push_back(o);
    return as_handle(o);
}

ScSlot* sc_obj_slots(ScObj* o) {
    MockObj* m = as_mock(o);
    if (m->is_string)
        return reinterpret_cast<ScSlot*>(m->bytes.data());
    return m->slots.data();
}

int sc_obj_size(ScObj* o) { return as_mock(o)->size; }
void sc_obj_set_size(ScObj* o, int n) { as_mock(o)->size = n; }
int sc_obj_is_string(ScObj* o) { return as_mock(o)->is_string; }

void sc_gc_write(ScVm*, ScObj*, ScSlot*) {} // no-op: mock has no incremental GC
void sc_gc_enter_delayed(ScVm*) {}
void sc_gc_exit_delayed(ScVm*) {}

void sc_install_finalizer(ScVm*, ScObj* obj, int /*slot_index*/, ScFinalizerFn fn) {
    g_finalizers.push_back({ fn, obj });
}

void sc_post(const char* msg) { fputs(msg, stdout); }

} // extern "C"

// ---- a tiny driver -------------------------------------------------------

namespace {

ScSlot s_int(long long i) { ScSlot s; s.tag = 2; s.u.i = i; return s; }
ScSlot s_float(double f) { ScSlot s; s.tag = 9; s.u.f = f; return s; }
ScSlot s_nil() { ScSlot s; s.tag = 5; s.u.i = 0; return s; }
ScSlot s_obj(ScObj* o) { ScSlot s; s.tag = 1; s.u.ptr = o; return s; }

ScPrimFn find(const char* name) {
    for (auto& p : g_prims)
        if (p.name == name)
            return p.fn;
    fprintf(stderr, "primitive %s not registered\n", name);
    exit(1);
}

// Push receiver + args, call the primitive, return the result slot (index 0).
ScSlot call(const char* name, std::vector<ScSlot> stack) {
    int n = static_cast<int>(stack.size());
    for (int i = 0; i < n; ++i)
        g_vm.stack[i] = stack[i];
    g_vm.sp = &g_vm.stack[n - 1];
    int err = find(name)(reinterpret_cast<ScVm*>(&g_vm), n);
    if (err != 0)
        printf("  (primitive returned error %d)\n", err);
    return g_vm.stack[0];
}

void print_int_array(const char* label, ScSlot r) {
    MockObj* o = as_mock(reinterpret_cast<ScObj*>(r.u.ptr));
    printf("%s[", label);
    for (int i = 0; i < o->size; ++i)
        printf("%s%lld", i ? ", " : "", static_cast<long long>(o->slots[i].u.i));
    printf("]\n");
}

void print_string(const char* label, ScSlot r) {
    MockObj* o = as_mock(reinterpret_cast<ScObj*>(r.u.ptr));
    printf("%s\"%.*s\"\n", label, o->size, reinterpret_cast<char*>(o->bytes.data()));
}

} // namespace

int main() {
    sc_rust_register_all();
    printf("Registered %zu Rust primitives.\n\n", g_prims.size());

    printf("== value primitives ==\n");
    printf("nthPrime(10)   -> %lld\n", static_cast<long long>(call("_RustNthPrime", { s_nil(), s_int(10) }).u.i));
    printf("hypot(3, 4)    -> %g\n", call("_RustHypot", { s_nil(), s_int(3), s_int(4) }).u.f);

    printf("\n== object builders ==\n");
    print_int_array("primesUpTo(30) -> ", call("_RustPrimesUpTo", { s_nil(), s_int(30) }));

    ScObj* data = sc_new_array(nullptr, 6);
    double vals[6] = { 0.0, 0.05, 0.2, 0.8, 0.95, 1.0 };
    for (int i = 0; i < 6; ++i)
        sc_obj_slots(data)[i] = s_float(vals[i]);
    print_int_array("histogram(.., 3) -> ", call("_RustHistogram", { s_nil(), s_obj(data), s_int(3) }));

    printf("\n== strings ==\n");
    print_string("reverse(\"hello\") -> ", call("_RustReverseString", { s_nil(), s_obj(sc_new_string(nullptr, (const unsigned char*)"hello", 5)) }));
    print_string("shout(\"hi there\") -> ", call("_RustShout", { s_nil(), s_obj(sc_new_string(nullptr, (const unsigned char*)"hi there", 8)) }));

    printf("\n== foreign object (Rust value owned by an sclang object) ==\n");
    // Path A: explicit free -> Drop runs immediately.
    ScObj* c1 = sc_new_array(nullptr, 2); // 2 instance-var slots: ptr + finalizer
    call("_RustCounterNew", { s_obj(c1), s_obj(sc_new_string(nullptr, (const unsigned char*)"c1", 2)) });
    printf("c1.next -> %lld\n", static_cast<long long>(call("_RustCounterNext", { s_obj(c1) }).u.i));
    printf("c1.next -> %lld\n", static_cast<long long>(call("_RustCounterNext", { s_obj(c1) }).u.i));
    printf("c1.free (Drop should print next):\n");
    call("_RustCounterFree", { s_obj(c1) });

    // Path B: no free -> the finalizer runs the Drop at "collection".
    ScObj* c2 = sc_new_array(nullptr, 2);
    call("_RustCounterNew", { s_obj(c2), s_obj(sc_new_string(nullptr, (const unsigned char*)"c2", 2)) });
    call("_RustCounterNext", { s_obj(c2) });
    printf("c2 left un-freed; simulating GC (finalizers run, Drop should print):\n");
    for (auto& f : g_finalizers)
        f.fn(reinterpret_cast<ScVm*>(&g_vm), f.obj);

    printf("\nDone.\n");
    return 0;
}
