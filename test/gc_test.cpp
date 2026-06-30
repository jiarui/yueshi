// Heap/GC unit tests — verify the memory layer in isolation, before the
// evaluator is built on top of it. Compiled directly into the test exe (same
// pattern as the lexer/parser tests: src/lua/heap.cpp is a listed source).

#include "doctest.h"

#include <limits>
#include <string>
#include <vector>

#include "lua/heap.h"
#include "lua/value.h"

using namespace ys::lua;

namespace {
// A tiny test rig: a Heap + the root set the collector will see. Tests build
// object graphs by hand (no evaluator), then call heap.collect(roots_fn) and
// assert on heap.live_count().
struct GCRig {
    Heap heap;
    std::vector<GCObject*> roots;   // raw roots for the mark phase

    void mark_all(auto& visitor) const
    {
        for (GCObject* r : roots) visitor(r);
    }
};
} // namespace

TEST_CASE("gc: allocators stamp type and link into the live list")
{
    GCRig g;
    CHECK(g.heap.live_count() == 0);

    auto* s = g.heap.make_string("hello");
    auto* t = g.heap.make_table();
    auto* e = g.heap.make_env(nullptr);

    CHECK(g.heap.live_count() == 3);
    CHECK(s->type == ObjType::String);
    CHECK(t->type == ObjType::Table);
    CHECK(e->type == ObjType::Env);
    CHECK(s->marked == false);
    CHECK(s->data == "hello");   // round-trip through the typed pointer
    CHECK(e->parent == nullptr);
}

TEST_CASE("gc: destructor frees every live object (no leak)")
{
    // Wrap in a block so ~Heap runs before we lose the pointer; the leak
    // detector (ASan) would flag a leak if the destructor missed anything.
    std::size_t after_destroy;
    {
        GCRig g;
        for (int i = 0; i < 50; ++i) (void)g.heap.make_string(std::to_string(i));
        CHECK(g.heap.live_count() == 50);
    } // ~Heap
    after_destroy = 0; (void)after_destroy;
    // If we reach here without ASan leak reports, the destructor reclaimed all.
    CHECK(true);
}

TEST_CASE("gc: unreachable single object is swept")
{
    GCRig g;
    auto* keep = g.heap.make_string("keep");
    (void)g.heap.make_string("garbage");   // no root -> unreachable
    CHECK(g.heap.live_count() == 2);

    g.roots.push_back(static_cast<GCObject*>(keep));
    std::size_t swept = g.heap.collect([&](auto visitor) { g.mark_all(visitor); });

    CHECK(swept == 1);
    CHECK(g.heap.live_count() == 1);
    // The survivor is the one we rooted.
    CHECK(static_cast<String*>(g.roots[0])->data == "keep");
}

TEST_CASE("gc: a reachable cycle is NOT swept (reachability beats refcounting)")
{
    // The case shared_ptr gets wrong: t1.x = t2; t2.x = t1, both rooted via t1.
    GCRig g;
    auto* t1 = g.heap.make_table();
    auto* t2 = g.heap.make_table();
    // t1[1] = t2 ; t2[1] = t1   (a 2-cycle)
    LuaKey k1; k1.k = LuaKey::K::Int; k1.i = 1;
    t1->hash[k1] = LuaValue::table(t2);
    t2->hash[k1] = LuaValue::table(t1);

    g.roots.push_back(static_cast<GCObject*>(t1));   // root only t1
    std::size_t swept = g.heap.collect([&](auto visitor) { g.mark_all(visitor); });

    CHECK(swept == 0);                  // t2 reachable through t1 -> t1[1]
    CHECK(g.heap.live_count() == 2);
}

TEST_CASE("gc: an UNreachable cycle IS swept")
{
    // Same cycle, but now neither t1 nor t2 is rooted -> both must die. This is
    // the whole point of mark-sweep over refcounting.
    GCRig g;
    auto* t1 = g.heap.make_table();
    auto* t2 = g.heap.make_table();
    LuaKey k1; k1.k = LuaKey::K::Int; k1.i = 1;
    t1->hash[k1] = LuaValue::table(t2);
    t2->hash[k1] = LuaValue::table(t1);
    CHECK(g.heap.live_count() == 2);

    std::size_t swept = g.heap.collect([&](auto /*visitor*/) { /* no roots */ });

    CHECK(swept == 2);
    CHECK(g.heap.live_count() == 0);
}

TEST_CASE("gc: escaping closure keeps its captured environment alive")
{
    // The motivation for Environment being a GCObject: a closure references an
    // env that has otherwise gone out of scope. Root the closure; the env and
    // a value it captures must survive.
    GCRig g;
    auto* captured_str = g.heap.make_string("captured");
    auto* env = g.heap.make_env(nullptr);
    env->vars["x"] = LuaValue::str(captured_str);

    auto* clo = g.heap.make_closure(nullptr, env, false);  // body unused here
    CHECK(g.heap.live_count() == 3);   // str + env + closure

    g.roots.push_back(static_cast<GCObject*>(clo));
    std::size_t swept = g.heap.collect([&](auto visitor) { g.mark_all(visitor); });

    CHECK(swept == 0);                  // closure -> env -> str, all reachable
    CHECK(g.heap.live_count() == 3);
}

TEST_CASE("gc: environment chain traces through parent links")
{
    GCRig g;
    auto* root_env   = g.heap.make_env(nullptr);
    auto* middle_env = g.heap.make_env(root_env);
    auto* leaf_env   = g.heap.make_env(middle_env);
    auto* deep_str   = g.heap.make_string("deep");
    root_env->vars["d"] = LuaValue::str(deep_str);
    CHECK(g.heap.live_count() == 4);

    // Root only the leaf; the whole chain + the deep string must survive.
    g.roots.push_back(static_cast<GCObject*>(leaf_env));
    std::size_t swept = g.heap.collect([&](auto visitor) { g.mark_all(visitor); });

    CHECK(swept == 0);
    CHECK(g.heap.live_count() == 4);
}

TEST_CASE("gc: integral float keys collapse to int keys (t[1] == t[1.0])")
{
    GCRig g;
    auto* t = g.heap.make_table();

    LuaKey k_int; k_int.k = LuaKey::K::Int; k_int.i = 1;
    LuaKey k_flt; k_flt.k = LuaKey::K::Flt; k_flt.f = 1.0;

    // Insert via the int key, then verify to_key() maps a float 1.0 to it.
    LuaValue v = LuaValue::integer(42);
    t->hash[k_int] = v;

    LuaKey from_flt;
    bool ok = to_key(LuaValue::flt(1.0), from_flt);
    REQUIRE(ok);
    CHECK(from_flt.k == LuaKey::K::Int);     // collapsed, not Flt
    CHECK(from_flt.i == 1);
    CHECK(from_flt == k_int);
    CHECK(t->hash.find(from_flt) != t->hash.end());

    (void)k_flt;  // silence unused
}

TEST_CASE("gc: NaN is rejected as a table key")
{
    double nan = std::numeric_limits<double>::quiet_NaN();
    LuaKey k;
    CHECK_FALSE(to_key(LuaValue::flt(nan), k));
    CHECK_FALSE(to_key(LuaValue::nil(), k));
}
