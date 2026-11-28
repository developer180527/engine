// ── eng.hpp — the engine's C++ API: a header-only wrapper over the C API ─────
// WO-095; DR-0028 (one C API, with C++ as a thin wrapper, for everyone);
// docs/plans/engine-api.md §8.
//
// Everything here is inline over <eng/engine.h> and nothing else: no engine
// C++ header, no flecs, no exported symbol. A program linked with the engine
// and a module built with ENG_CLIENT (eng_client.h) use it the same way, since
// both see the eng* functions as ordinary declarations.
//
//   struct Health { float current = 100, max = 100; };
//   ENG_COMPONENT(Health, "combat.Health",
//                 eng::field("current", &Health::current),
//                 eng::field("max", &Health::max));
//
//   eng::World w = ...;                              // a view, owns nothing
//   auto health  = w.component<Health>();            // Result<Component<Health>>
//   auto e       = w.createEntity("player");         // Result<Entity>
//   e->add(*health, Health{50, 100});
//   eng::Query<eng::WriteMarked<Health>> q(w, *health);
//   q.each([](auto& b) {
//       auto hp = b.template column<0>();
//       for (uint32_t i = 0; i < b.count(); ++i)
//           if (hp[i].current < hp[i].max) hp.write(i).current += 1;   // marks row i
//   });
//
// RULES, the C API's, and what the wrapper adds:
//   * Results, not exceptions: every call that can fail returns eng::Result,
//     with the C error and its message. The header throws nothing and works
//     with -fno-exceptions.
//   * Components are plain data, standard-layout and trivially copyable: their
//     bytes are copied in and out. ENG_COMPONENT describes the fields once; the
//     offsets and the registered defaults come from a default-constructed value.
//   * A WRITE column marks every row (as the C API does). A WRITE_MARKED column
//     gives a mutable element ONLY through write(i), which marks row i, so a
//     write that is not marked cannot be spelled. Contiguous rows are marked in
//     one call.
//   * RAII: Query, System and ChangeStream end what they made when they go.
//     Destroy them before the world goes (a kit, in its on_world_stop). A C kit's
//     queries and systems are also dropped by the engine at its stop, and
//     ending one of those again only sets the last error.
//   * Callbacks are noexcept: an exception leaving a system or a module
//     callback would cross the C boundary, so it ends the program instead.
#ifndef ENG_HPP
#define ENG_HPP

#include "engine.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace eng {

// ── Results ─────────────────────────────────────────────────────────────────
struct Error {
    EngResult   code = ENG_OK;
    std::string message;
};

// The C API's last error, as an Error.
inline Error lastError(EngResult code) {
    const char* m = engGetLastErrorMessage();
    return Error{code, m ? m : ""};
}

template <class T>
class Result {
public:
    Result(T v) : m_ok(true), m_value(std::move(v)) {}
    Result(Error e) : m_ok(false), m_error(std::move(e)) {}
    explicit operator bool() const { return m_ok; }
    bool ok() const { return m_ok; }
    T& operator*() { return m_value; }
    const T& operator*() const { return m_value; }
    T* operator->() { return &m_value; }
    const T* operator->() const { return &m_value; }
    T& value() { return m_value; }
    const Error& error() const { return m_error; }
    EngResult code() const { return m_ok ? ENG_OK : m_error.code; }
private:
    bool  m_ok;
    T     m_value{};
    Error m_error;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Error e) : m_ok(false), m_error(std::move(e)) {}
    explicit operator bool() const { return m_ok; }
    bool ok() const { return m_ok; }
    const Error& error() const { return m_error; }
    EngResult code() const { return m_ok ? ENG_OK : m_error.code; }
private:
    bool  m_ok = true;
    Error m_error;
};

// A C call's EngResult as a Result<void>, with the message if it failed.
inline Result<void> check(EngResult r) {
    if (r == ENG_OK) return {};
    return lastError(r);
}

// A small span, so the header needs no C++20.
template <class T>
class Span {
public:
    Span() = default;
    Span(T* p, uint32_t n) : m_p(p), m_n(n) {}
    T& operator[](uint32_t i) const { return m_p[i]; }
    uint32_t size() const { return m_n; }
    T* data() const { return m_p; }
    T* begin() const { return m_p; }
    T* end() const { return m_p + m_n; }
private:
    T*       m_p = nullptr;
    uint32_t m_n = 0;
};

// ── Fields: what a component's bytes mean ───────────────────────────────────
// A field's EngFieldType from its C++ type, where the type says it. Where it
// does not (an EngEntity and an EngBlob are both uint64_t; a BOOL32 is a
// uint32_t), say it: eng::field("target", &T::target, ENG_FIELD_ENTITY).
namespace detail {
template <class M> struct FieldOf { static constexpr bool known = false; };
#define ENG_HPP_FIELD(Type, Code, Count) \
    template <> struct FieldOf<Type> { static constexpr bool known = true; \
        static constexpr EngFieldType type = Code; static constexpr uint32_t count = Count; };
ENG_HPP_FIELD(int8_t,   ENG_FIELD_I8, 1)
ENG_HPP_FIELD(uint8_t,  ENG_FIELD_U8, 1)
ENG_HPP_FIELD(int16_t,  ENG_FIELD_I16, 1)
ENG_HPP_FIELD(uint16_t, ENG_FIELD_U16, 1)
ENG_HPP_FIELD(int32_t,  ENG_FIELD_I32, 1)
ENG_HPP_FIELD(uint32_t, ENG_FIELD_U32, 1)
ENG_HPP_FIELD(int64_t,  ENG_FIELD_I64, 1)
ENG_HPP_FIELD(uint64_t, ENG_FIELD_U64, 1)
ENG_HPP_FIELD(float,    ENG_FIELD_F32, 1)
ENG_HPP_FIELD(double,   ENG_FIELD_F64, 1)
ENG_HPP_FIELD(EngVec3,  ENG_FIELD_VEC3, 1)
ENG_HPP_FIELD(EngQuat,  ENG_FIELD_QUAT, 1)
ENG_HPP_FIELD(float[2], ENG_FIELD_VEC2, 1)
ENG_HPP_FIELD(float[3], ENG_FIELD_VEC3, 1)
ENG_HPP_FIELD(float[4], ENG_FIELD_VEC4, 1)
ENG_HPP_FIELD(float[16], ENG_FIELD_MAT4, 1)
#undef ENG_HPP_FIELD
// char[N]: a fixed string of N bytes, NUL included.
template <std::size_t N> struct FieldOf<char[N]> {
    static constexpr bool known = true;
    static constexpr EngFieldType type = ENG_FIELD_FIXED_STRING;
    static constexpr uint32_t count = (uint32_t)N;
};
}  // namespace detail

namespace detail {
// The one value of T that field offsets are measured on and that is
// registered as T's defaults.
template <class T> const T& prototype() { static const T v{}; return v; }
}  // namespace detail

// eng::field("name", &T::member): the type from the member's C++ type.
// eng::field("name", &T::member, ENG_FIELD_ENTITY): the type stated (an
// EngEntity, an EngBlob, a BOOL32), one value of it.
template <class T, class M>
struct FieldMaker {
    const char*  name;
    M T::*       member;
    EngFieldType type;
    uint32_t     count;
};
template <class T, class M>
FieldMaker<T, M> field(const char* name, M T::*member) {
    static_assert(detail::FieldOf<M>::known,
                  "this member's type has no EngFieldType of its own: state it, "
                  "eng::field(name, &T::m, ENG_FIELD_...)");
    return {name, member, detail::FieldOf<M>::type, detail::FieldOf<M>::count};
}
template <class T, class M>
FieldMaker<T, M> field(const char* name, M T::*member, EngFieldType type) {
    // A char array as a FIXED_STRING is its byte length; any other array is
    // count values of `type`; anything else one value.
    uint32_t count = 1;
    if constexpr (std::is_array_v<M>)
        count = type == ENG_FIELD_FIXED_STRING ? (uint32_t)sizeof(M) : (uint32_t)std::extent_v<M>;
    return {name, member, type, count};
}

// What ENG_COMPONENT specializes: a component's name, revision and fields.
template <class T> struct ComponentInfo;   // not defined: describe T with ENG_COMPONENT

namespace detail {
template <class T, class... Ms>
std::vector<EngFieldDesc> describe(const FieldMaker<T, Ms>&... fs) {
    std::vector<EngFieldDesc> out;
    const T& v = prototype<T>();
    auto add = [&](auto const& f) {
        EngFieldDesc d{};
        d.name   = f.name;
        d.type   = f.type;
        d.offset = (uint32_t)(reinterpret_cast<const unsigned char*>(&(v.*(f.member)))
                              - reinterpret_cast<const unsigned char*>(&v));
        d.count  = f.count;
        out.push_back(d);
    };
    (add(fs), ...);
    return out;
}
}  // namespace detail

// Describes T once, at global namespace scope (it specializes
// eng::ComponentInfo<T>; T itself may live in any namespace):
//   ENG_COMPONENT(Health, "combat.Health", eng::field("current", &Health::current), ...)
// Revision 0; ENG_COMPONENT_REVISION states another.
#define ENG_COMPONENT_REVISION(Type, Name, Revision, ...)                          \
    template <> struct eng::ComponentInfo<Type> {                                    \
        static constexpr const char* name = Name;                                    \
        static constexpr uint32_t revision = Revision;                               \
        static const std::vector<EngFieldDesc>& fields() {                           \
            static const std::vector<EngFieldDesc> f = ::eng::detail::describe<Type>(__VA_ARGS__); \
            return f;                                                                \
        }                                                                            \
    };
#define ENG_COMPONENT(Type, Name, ...) ENG_COMPONENT_REVISION(Type, Name, 0, __VA_ARGS__)

// A registered component of type T in one world.
template <class T>
struct Component {
    EngComponent id = 0;
    explicit operator bool() const { return id != 0; }
};

template <class T>
constexpr void checkComponentType() {
    static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>,
                  "a component is plain data: standard-layout and trivially copyable");
    static_assert(std::is_default_constructible_v<T>,
                  "a component is default-constructible: its default value is what "
                  "engAddComponent gives an entity, and where field offsets are read");
}

// ── The world, and its entities ─────────────────────────────────────────────
class World;

class Entity {
public:
    Entity() = default;
    Entity(EngWorld* w, EngEntity id) : m_w(w), m_id(id) {}
    EngEntity id() const { return m_id; }
    EngWorld* world() const { return m_w; }
    explicit operator bool() const { return m_id != 0; }
    bool operator==(const Entity& o) const { return m_w == o.m_w && m_id == o.m_id; }
    bool operator!=(const Entity& o) const { return !(*this == o); }

    bool alive() const { return m_w && m_id && engIsEntityAlive(m_w, m_id); }
    void destroy() { if (m_w && m_id) engDestroyEntity(m_w, m_id); }

    Result<void> setParent(Entity parent) { return check(engSetEntityParent(m_w, m_id, parent.m_id)); }
    Entity parent() const { return Entity(m_w, engGetEntityParent(m_w, m_id)); }

    template <class T> bool has(Component<T> c) const {
        return engHasComponent(m_w, m_id, c.id) == ENG_TRUE;
    }
    // Added with `value`, or the registered default.
    template <class T> Result<void> add(Component<T> c) {
        return check(engAddComponent(m_w, m_id, c.id, nullptr));
    }
    template <class T> Result<void> add(Component<T> c, const T& value) {
        return check(engAddComponent(m_w, m_id, c.id, &value));
    }
    template <class T> Result<void> remove(Component<T> c) {
        return check(engRemoveComponent(m_w, m_id, c.id));
    }
    template <class T> Result<T> get(Component<T> c) const {
        T v{};
        const EngResult r = engGetComponent(m_w, m_id, c.id, &v, (uint32_t)sizeof(T));
        if (r != ENG_OK) return lastError(r);
        return v;
    }
    // A set the change stream sees (OnSet).
    template <class T> Result<void> set(Component<T> c, const T& value) {
        return check(engSetComponent(m_w, m_id, c.id, &value, (uint32_t)sizeof(T)));
    }

private:
    EngWorld* m_w = nullptr;
    EngEntity m_id = 0;
};

// A view of an EngWorld: owns nothing. A kit gets one in on_world_start.
class World {
public:
    World() = default;
    explicit World(EngWorld* w) : m_w(w) {}
    EngWorld* get() const { return m_w; }
    explicit operator bool() const { return m_w != nullptr; }

    Result<Entity> createEntity(const char* name = nullptr) {
        EngEntity e = 0;
        const EngResult r = engCreateEntity(m_w, name, &e);
        if (r != ENG_OK) return lastError(r);
        return Entity(m_w, e);
    }
    Entity find(const char* name) const { return Entity(m_w, engFindEntity(m_w, name)); }
    Entity entity(EngEntity id) const { return Entity(m_w, id); }

    // Registered from ENG_COMPONENT's description (registering it again, the
    // same, returns the same id; anything different is refused by name).
    template <class T> Result<Component<T>> component() {
        checkComponentType<T>();
        const auto& fields = ComponentInfo<T>::fields();
        EngComponentDesc d = ENG_INIT(EngComponentDesc);
        d.revision    = ComponentInfo<T>::revision;
        d.name        = ComponentInfo<T>::name;
        d.size        = (uint32_t)sizeof(T);
        d.align       = (uint32_t)alignof(T);
        d.fields      = fields.data();
        d.field_count = (uint32_t)fields.size();
        d.defaults    = &detail::prototype<T>();
        EngComponent id = 0;
        const EngResult r = engRegisterComponent(m_w, &d, &id);
        if (r != ENG_OK) return lastError(r);
        return Component<T>{id};
    }
    // One some other code registered (another kit's), by its name, unchecked:
    // get and set check T's size, a query's columns are only cast. Prefer
    // describing the other kit's component with ENG_COMPONENT and calling
    // component<T>(): the engine then checks the description against the
    // registered one (layout, fields, defaults) and refuses a mismatch.
    template <class T> Result<Component<T>> findComponent(const char* name) const {
        checkComponentType<T>();
        const EngComponent id = engFindComponent(m_w, name);
        if (!id) return Error{ENG_ERROR_NOT_FOUND, std::string("no component '") + name + "' in this world"};
        return Component<T>{id};
    }

    Result<EngBlob> createBlob(const void* bytes, uint32_t size) {
        EngBlob b = 0;
        const EngResult r = engCreateBlob(m_w, bytes, size, &b);
        if (r != ENG_OK) return lastError(r);
        return b;
    }
    // A blob's bytes, valid until it is set again or freed.
    Result<Span<const uint8_t>> blob(EngBlob b) const {
        const void* p = nullptr;
        uint32_t n = 0;
        const EngResult r = engGetBlob(m_w, b, &p, &n);
        if (r != ENG_OK) return lastError(r);
        return Span<const uint8_t>(static_cast<const uint8_t*>(p), n);
    }
    Result<void> setBlob(EngBlob b, const void* bytes, uint32_t size) {
        return check(engSetBlob(m_w, b, bytes, size));
    }

private:
    EngWorld* m_w = nullptr;
};

// ── Owning handles, for a program (a kit gets its world from the engine) ────
class Engine {
public:
    Engine() = default;
    static Result<Engine> create(const char* appName = nullptr) {
        EngEngineDesc d = ENG_INIT(EngEngineDesc);
        d.api_version = ENG_API_VERSION;
        d.app_name = appName;
        Engine e;
        const EngResult r = engCreateEngine(&d, &e.m_e);
        if (r != ENG_OK) return lastError(r);
        return e;
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&& o) noexcept : m_e(o.m_e) { o.m_e = nullptr; }
    Engine& operator=(Engine&& o) noexcept {
        if (this != &o) { reset(); m_e = o.m_e; o.m_e = nullptr; }
        return *this;
    }
    ~Engine() { reset(); }
    EngEngine* get() const { return m_e; }
    explicit operator bool() const { return m_e != nullptr; }
    void reset() { if (m_e) engDestroyEngine(m_e); m_e = nullptr; }
    // One frame with this delta: the fixed steps it covers, then frame systems.
    Result<EngStepInfo> step(double dt) {
        EngStepInfo info = ENG_INIT(EngStepInfo);
        const EngResult r = engStepWithDelta(m_e, dt, &info);
        if (r != ENG_OK) return lastError(r);
        return info;
    }
private:
    EngEngine* m_e = nullptr;
};

// The engine's world, destroyed when this goes. Destroy the queries, systems
// and streams made in it first.
class OwnedWorld {
public:
    OwnedWorld() = default;
    static Result<OwnedWorld> create(Engine& e) {
        OwnedWorld w;
        EngWorld* p = nullptr;
        const EngResult r = engCreateWorld(e.get(), nullptr, &p);
        if (r != ENG_OK) return lastError(r);
        w.m_w = World(p);
        return w;
    }
    OwnedWorld(const OwnedWorld&) = delete;
    OwnedWorld& operator=(const OwnedWorld&) = delete;
    OwnedWorld(OwnedWorld&& o) noexcept : m_w(o.m_w) { o.m_w = World(); }
    OwnedWorld& operator=(OwnedWorld&& o) noexcept {
        if (this != &o) { reset(); m_w = o.m_w; o.m_w = World(); }
        return *this;
    }
    ~OwnedWorld() { reset(); }
    void reset() { if (m_w) engDestroyWorld(m_w.get()); m_w = World(); }
    World& operator*() { return m_w; }
    World* operator->() { return &m_w; }
    operator World() const { return m_w; }
private:
    World m_w;
};

// ── Queries ─────────────────────────────────────────────────────────────────
// A query's terms, as types. Read, Write and WriteMarked give a column; With
// and Without only filter.
template <class T> struct Read        { using type = T; static constexpr EngAccess access = ENG_ACCESS_READ;          static constexpr bool column = true; };
template <class T> struct Write       { using type = T; static constexpr EngAccess access = ENG_ACCESS_WRITE;         static constexpr bool column = true; };
template <class T> struct WriteMarked { using type = T; static constexpr EngAccess access = ENG_ACCESS_WRITE_MARKED; static constexpr bool column = true; };
template <class T> struct With        { using type = T; static constexpr EngAccess access = ENG_ACCESS_WITH;          static constexpr bool column = false; };
template <class T> struct Without     { using type = T; static constexpr EngAccess access = ENG_ACCESS_WITHOUT;       static constexpr bool column = false; };

// A WRITE_MARKED column: reading is free, writing marks. write(i) returns the
// mutable element and records row i; contiguous rows are marked in one call
// when the batch ends.
template <class T>
class MarkedColumn {
public:
    MarkedColumn(T* p, uint32_t n, EngQueryIter* it, uint32_t term)
        : m_p(p), m_n(n), m_it(it), m_term(term) {}
    MarkedColumn(const MarkedColumn&) = delete;
    MarkedColumn& operator=(const MarkedColumn&) = delete;
    MarkedColumn(MarkedColumn&& o) noexcept
        : m_p(o.m_p), m_n(o.m_n), m_it(o.m_it), m_term(o.m_term),
          m_first(o.m_first), m_count(o.m_count) { o.m_count = 0; o.m_it = nullptr; }
    ~MarkedColumn() { flush(); }

    const T& operator[](uint32_t i) const { return m_p[i]; }
    uint32_t size() const { return m_n; }
    T& write(uint32_t i) {
        if (m_count && i == m_first + m_count) ++m_count;          // extends the run
        else if (!(m_count && i >= m_first && i < m_first + m_count)) {
            flush();
            m_first = i;
            m_count = 1;
        }
        return m_p[i];
    }
    // Marks every row still pending. Called at the end of the batch.
    void flush() {
        if (m_count && m_it) engMarkChanged(m_it, m_term, m_first, m_count);
        m_count = 0;
    }

private:
    T*            m_p;
    uint32_t      m_n;
    EngQueryIter* m_it;
    uint32_t      m_term;
    uint32_t      m_first = 0, m_count = 0;
};

namespace detail {
template <class Term> struct ColumnOf {
    using type = std::conditional_t<Term::access == ENG_ACCESS_READ,
                                    Span<const typename Term::type>, Span<typename Term::type>>;
};
template <class T> struct ColumnOf<WriteMarked<T>> { using type = MarkedColumn<T>; };

template <std::size_t I, class... Ts> struct Nth;
template <class T, class... Ts> struct Nth<0, T, Ts...> { using type = T; };
template <std::size_t I, class T, class... Ts> struct Nth<I, T, Ts...> { using type = typename Nth<I - 1, Ts...>::type; };
}  // namespace detail

// One batch of a query: count() rows, their entities, and a column per term.
template <class... Terms>
class Batch {
public:
    Batch(EngWorld* w, const EngQueryBatch& b, EngQueryIter* it) : m_w(w), m_b(b), m_it(it) {}
    uint32_t count() const { return m_b.count; }
    Span<const EngEntity> entities() const { return Span<const EngEntity>(m_b.entities, m_b.count); }
    Entity entity(uint32_t i) const { return Entity(m_w, m_b.entities[i]); }
    World world() const { return World(m_w); }

    // Term I's column: Span<const T> for Read, Span<T> for Write (every row is
    // marked), MarkedColumn<T> for WriteMarked.
    template <std::size_t I>
    typename detail::ColumnOf<typename detail::Nth<I, Terms...>::type>::type column() const {
        using Term = typename detail::Nth<I, Terms...>::type;
        static_assert(Term::column, "With and Without terms have no column");
        using T = typename Term::type;
        T* p = static_cast<T*>(m_b.columns[I]);
        if constexpr (Term::access == ENG_ACCESS_WRITE_MARKED)
            return MarkedColumn<T>(p, m_b.count, m_it, (uint32_t)I);
        else if constexpr (Term::access == ENG_ACCESS_READ)
            return Span<const T>(p, m_b.count);
        else
            return Span<T>(p, m_b.count);
    }

private:
    EngWorld*     m_w;
    EngQueryBatch m_b;
    EngQueryIter* m_it;
};

// A query over the given terms. Ends the C query when it goes. Built from the
// components' handles, in term order: Query<Read<A>, Write<B>> q(w, a, b).
template <class... Terms>
class Query {
    static_assert(sizeof...(Terms) >= 1 && sizeof...(Terms) <= 16, "a query has 1 to 16 terms");
public:
    Query() = default;
    Query(World w, Component<typename Terms::type>... cs) : m_w(w.get()) {
        const EngComponent ids[] = {cs.id...};
        const EngAccess access[] = {Terms::access...};
        EngQueryTerm terms[sizeof...(Terms)];
        for (std::size_t i = 0; i < sizeof...(Terms); ++i) {
            terms[i] = EngQueryTerm{};
            terms[i].component = ids[i];
            terms[i].access = access[i];
        }
        EngQueryDesc d = ENG_INIT(EngQueryDesc);
        d.term_count = (uint32_t)sizeof...(Terms);
        d.terms = terms;
        const EngResult r = engCreateQuery(m_w, &d, &m_q);
        if (r != ENG_OK) { m_error = lastError(r); m_q = 0; }
    }
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    Query(Query&& o) noexcept : m_w(o.m_w), m_q(o.m_q), m_error(std::move(o.m_error)) { o.m_q = 0; }
    Query& operator=(Query&& o) noexcept {
        if (this != &o) { reset(); m_w = o.m_w; m_q = o.m_q; m_error = std::move(o.m_error); o.m_q = 0; }
        return *this;
    }
    ~Query() { reset(); }

    explicit operator bool() const { return m_q != 0; }
    const Error& error() const { return m_error; }   // why it was not made
    EngQuery get() const { return m_q; }
    void reset() { if (m_q && m_w) engDestroyQuery(m_w, m_q); m_q = 0; }

    // fn(Batch<Terms...>&) per batch. Structural changes made inside wait for
    // the end of the iteration (the C API's rule).
    template <class Fn>
    Result<void> each(Fn&& fn) const {
        EngQueryIter* it = nullptr;
        const EngResult r = engQueryBegin(m_w, m_q, &it);
        if (r != ENG_OK) return lastError(r);
        EngQueryBatch b = ENG_INIT(EngQueryBatch);
        while (engQueryNext(it, &b)) {
            Batch<Terms...> batch(m_w, b, it);
            fn(batch);
        }
        engQueryEnd(it);
        return {};
    }

private:
    EngWorld* m_w = nullptr;
    EngQuery  m_q = 0;
    Error     m_error;
};

// ── Systems ─────────────────────────────────────────────────────────────────
struct SystemOptions {
    const char*              name = nullptr;     // namespaced and unique
    EngPhase                 phase = ENG_PHASE_UPDATE;
    std::vector<const char*> runAfter;           // systems of the same phase
};

// What a system's function gets: the step, the tick, and the world.
struct Step {
    double    dt;
    uint64_t  tick;
    World     world;
};

// A system: fn(const Step&, Batch<Terms...>&) per batch of its query, or
// fn(const Step&) once a step with no query. Ends the C system when it goes.
class System {
public:
    System() = default;
    System(const System&) = delete;
    System& operator=(const System&) = delete;
    System(System&& o) noexcept : m_w(o.m_w), m_s(o.m_s), m_fn(std::move(o.m_fn)), m_error(std::move(o.m_error)) { o.m_s = 0; }
    System& operator=(System&& o) noexcept {
        if (this != &o) { reset(); m_w = o.m_w; m_s = o.m_s; m_fn = std::move(o.m_fn); m_error = std::move(o.m_error); o.m_s = 0; }
        return *this;
    }
    ~System() { reset(); }

    explicit operator bool() const { return m_s != 0; }
    const Error& error() const { return m_error; }
    void reset() { if (m_s && m_w) engDestroySystem(m_w, m_s); m_s = 0; m_fn.reset(); }

    template <class... Terms, class Fn>
    static System over(World w, const Query<Terms...>& q, const SystemOptions& o, Fn fn) {
        struct Holder {
            Fn fn;
            static void run(const EngSystemContext* ctx, const EngQueryBatch* b) noexcept {
                Holder& h = *static_cast<Holder*>(ctx->user);
                Step s{ctx->delta_seconds, ctx->tick, World(ctx->world)};
                Batch<Terms...> batch(ctx->world, *b, ctx->iter);
                h.fn(s, batch);
            }
        };
        return make(w, q.get(), o, std::shared_ptr<void>(new Holder{std::move(fn)},
                                                         [](void* p) { delete static_cast<Holder*>(p); }),
                    &Holder::run);
    }
    template <class Fn>
    static System every(World w, const SystemOptions& o, Fn fn) {
        struct Holder {
            Fn fn;
            static void run(const EngSystemContext* ctx, const EngQueryBatch*) noexcept {
                Holder& h = *static_cast<Holder*>(ctx->user);
                h.fn(Step{ctx->delta_seconds, ctx->tick, World(ctx->world)});
            }
        };
        return make(w, 0, o, std::shared_ptr<void>(new Holder{std::move(fn)},
                                                   [](void* p) { delete static_cast<Holder*>(p); }),
                    &Holder::run);
    }

private:
    static System make(World w, EngQuery q, const SystemOptions& o, std::shared_ptr<void> holder,
                       EngSystemFn run) {
        System s;
        s.m_w = w.get();
        EngSystemDesc d = ENG_INIT(EngSystemDesc);
        d.phase = o.phase;
        d.name = o.name;
        d.query = q;
        d.fn = run;
        d.user = holder.get();
        d.run_after = o.runAfter.empty() ? nullptr : o.runAfter.data();
        d.run_after_count = (uint32_t)o.runAfter.size();
        const EngResult r = engCreateSystem(s.m_w, &d, &s.m_s);
        if (r != ENG_OK) { s.m_error = lastError(r); s.m_s = 0; return s; }
        s.m_fn = std::move(holder);
        return s;
    }

    EngWorld*             m_w = nullptr;
    EngSystem             m_s = 0;
    std::shared_ptr<void> m_fn;   // the function; lives as long as the system
    Error                 m_error;
};

// ── The change stream ───────────────────────────────────────────────────────
class ChangeStream {
public:
    ChangeStream() = default;
    template <class T>
    ChangeStream(World w, Component<T> c) : m_w(w.get()) {
        const EngResult r = engSubscribeChanges(m_w, c.id, &m_s);
        if (r != ENG_OK) { m_error = lastError(r); m_s = 0; }
    }
    ChangeStream(const ChangeStream&) = delete;
    ChangeStream& operator=(const ChangeStream&) = delete;
    ChangeStream(ChangeStream&& o) noexcept : m_w(o.m_w), m_s(o.m_s), m_error(std::move(o.m_error)) { o.m_s = 0; }
    ChangeStream& operator=(ChangeStream&& o) noexcept {
        if (this != &o) { reset(); m_w = o.m_w; m_s = o.m_s; m_error = std::move(o.m_error); o.m_s = 0; }
        return *this;
    }
    ~ChangeStream() { reset(); }

    explicit operator bool() const { return m_s != 0; }
    const Error& error() const { return m_error; }
    void reset() { if (m_s && m_w) engUnsubscribeChanges(m_w, m_s); m_s = 0; }

    // Everything pending, merged (one entry per entity), oldest first.
    Result<std::vector<EngChange>> read() {
        std::vector<EngChange> out;
        EngChange buf[256];
        for (;;) {
            uint32_t n = 256;
            const EngResult r = engReadChanges(m_w, m_s, buf, &n);
            if (r != ENG_OK) return lastError(r);
            out.insert(out.end(), buf, buf + n);
            if (n < 256) return out;
        }
    }

private:
    EngWorld*       m_w = nullptr;
    EngChangeStream m_s = 0;
    Error           m_error;
};

// ── Modules: a C++ kit as a class ───────────────────────────────────────────
// Kit is default-constructible, with
//   Result<void> start(World)   at each world start (on_world_start)
//   void         stop(World)    at each world stop (on_world_stop)
// One Kit object lives from load to unload. In the module's engModuleEntry:
//   if (engClientLoad(get_proc, NULL) != ENG_OK) return NULL;
//   return eng::module<MyKit>("mykit", "1.0");
template <class Kit>
const EngModuleDesc* module(const char* name, const char* version = nullptr) {
    struct Glue {
        static EngResult load(EngEngine*, void** state) noexcept {
            Kit* k = new (std::nothrow) Kit();
            if (!k) return ENG_ERROR_OUT_OF_MEMORY;
            *state = k;
            return ENG_OK;
        }
        static void unload(EngEngine*, void* state) noexcept { delete static_cast<Kit*>(state); }
        static EngResult start(EngWorld* w, void* state) noexcept {
            Result<void> r = static_cast<Kit*>(state)->start(World(w));
            return r ? ENG_OK : r.code();
        }
        static void stop(EngWorld* w, void* state) noexcept { static_cast<Kit*>(state)->stop(World(w)); }
    };
    static EngModuleDesc d = [] {
        EngModuleDesc m{};
        m.struct_size = (uint32_t)sizeof(EngModuleDesc);
        m.api_version = ENG_API_VERSION;
        m.on_load = &Glue::load;
        m.on_unload = &Glue::unload;
        m.on_world_start = &Glue::start;
        m.on_world_stop = &Glue::stop;
        return m;
    }();
    d.name = name;
    d.version = version;
    return &d;
}

}  // namespace eng

#endif  // ENG_HPP
