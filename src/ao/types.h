#pragma once

#include <cstdint>
#include <cstring>

// Reconstructed AO type definitions.
//
// Reverse-engineered from Utils.dll using Ghidra. These represent the
// in-memory layout of objects as the MSVC x86 AO client sees them.
// All types must exactly match the client's ABI.
//
// The game is built with MSVC 2010 (links msvcr100.dll), targeting x86.

// Match MSVC x86 struct packing. The game was compiled with MSVC 2010
// default packing (8), but on x86 double is 8-byte aligned, which
// matches our layout. Use explicit packing to avoid clang surprises.
#pragma pack(push, 4)

namespace aor {

// ── AO's String class ──────────────────────────────────────────────────
// Funcom's String is a thin wrapper — it IS an MSVC 2010 std::string
// with no additional members. Confirmed by constructors that just forward
// to std::basic_string::basic_string, and by the destructor calling
// std::basic_string::_Tidy directly on `this`.
//
// MSVC 2010 x86 release std::basic_string<char> layout:
//   +0x00: union { char _Buf[16]; char* _Ptr; }  — SSO or heap
//   +0x10: uint32_t _Mysize   (length)
//   +0x14: uint32_t _Myres    (capacity; if >= 16, _Ptr is used)
//
// Total: 0x18 (24) bytes.
struct AOString {
    union {
        char    sso_buf[16];     // +0x00  inline storage (capacity < 16)
        char*   heap_ptr;        // +0x00  heap pointer (capacity >= 16)
    };
    uint32_t    length;          // +0x10
    uint32_t    capacity;        // +0x14

    const char* c_str() const {
        return capacity >= 16 ? heap_ptr : sso_buf;
    }

    // Construct in-place from a C string. Only for strings <= 15 chars
    // (SSO path). For longer strings, use the game's own String ctor
    // via game_api to avoid heap ownership mismatches.
    static AOString FromShort(const char* str) {
        AOString s;
        std::memset(&s, 0, sizeof(s));
        uint32_t len = static_cast<uint32_t>(std::strlen(str));
        // Must fit in SSO buffer (15 chars + null).
        if (len < 16) {
            std::memcpy(s.sso_buf, str, len + 1);
            s.length = len;
            s.capacity = 15;  // MSVC SSO capacity is always 15
        }
        return s;
    }
};
static_assert(sizeof(AOString) == 0x18);

// ── AO's Variant class ────────────────────────────────────────────────
// Tagged union, 0x10 (16) bytes. Confirmed from Utils.dll constructors
// and destructor.
//
// Layout:
//   +0x00: uint32_t type      — type tag (see VariantType enum)
//   +0x04: (padding/unused)
//   +0x08: union {            — value payload (8 bytes)
//            int32_t   i;
//            uint32_t  u;
//            float     f;
//            double    d;       (uses full 8 bytes)
//            int64_t   i64;     (uses full 8 bytes)
//            bool      b;
//            struct { uint32_t size; void* ptr; }  heap;  (complex types)
//          }
//
// The destructor frees heap.ptr for types 9 (string), 10-12, and 16-21.
// Simple types (int, float, bool, double, int64) are stored inline.
enum class VariantType : uint32_t {
    None     = 0,
    // 1-3 unknown/unused
    Int      = 4,   // int32 or uint32 at +0x08
    Int64    = 5,   // int64 at +0x08
    Bool     = 6,   // bool at +0x08
    Float    = 7,   // float at +0x08
    Double   = 8,   // double at +0x08
    String   = 9,   // heap-allocated char*, ptr at +0x08
    IRect    = 10,  // heap blob, size at +0x08, ptr at +0x0C
    IPoint   = 11,  // heap blob, size at +0x08, ptr at +0x0C
    Message  = 12,  // heap blob (flattened), size at +0x08, ptr at +0x0C
    // 13-15 unknown
    Rect     = 16,  // heap blob, size at +0x08, ptr at +0x0C
    Point    = 17,  // heap blob, size at +0x08, ptr at +0x0C
};

struct AOVariant {
    uint32_t type;            // +0x00
    uint32_t _pad;            // +0x04
    union {
        int32_t   as_int;     // +0x08  (type 4)
        int64_t   as_int64;   // +0x08  (type 5, uses 8 bytes)
        uint32_t  as_uint;    // +0x08  (type 4)
        float     as_float;   // +0x08  (type 7)
        double    as_double;  // +0x08  (type 8, uses 8 bytes)
        bool      as_bool;    // +0x08  (type 6)
        struct {
            uint32_t size;    // +0x08  (complex types)
            void*    ptr;     // +0x0C
        } heap;
    };

    // Construct simple variants inline. These match the game's own
    // Variant constructors exactly.
    static AOVariant FromInt(int32_t v) {
        AOVariant var{};
        var.type = static_cast<uint32_t>(VariantType::Int);
        var.as_int = v;
        return var;
    }
    static AOVariant FromFloat(float v) {
        AOVariant var{};
        var.type = static_cast<uint32_t>(VariantType::Float);
        var.as_float = v;
        return var;
    }
    static AOVariant FromBool(bool v) {
        AOVariant var{};
        var.type = static_cast<uint32_t>(VariantType::Bool);
        var.as_bool = v;
        return var;
    }
    static AOVariant FromDouble(double v) {
        AOVariant var{};
        var.type = static_cast<uint32_t>(VariantType::Double);
        var.as_double = v;
        return var;
    }
};
static_assert(sizeof(AOVariant) == 0x10);

// ── DistributedValue_c ────────────────────────────────────────────────
// Size: 0x50 (80) bytes. Confirmed from consecutive instances in
// GUI.dll's ChatView_c constructor (spaced 0x50 apart).
//
// Layout (from Utils.dll constructor at 0x100035bb):
//   +0x00: void**       vftable
//   +0x04: uint32_t     (unknown, gap to SignalBase_c)
//   +0x08: SignalBase_c signal           (4 bytes — just a slot list head)
//   +0x0C: AOString     name             (0x18 bytes, the DValue key)
//   +0x24: uint32_t     (padding)
//   +0x28: container    observed_names   (0x10 bytes, init by FUN_10003fa4)
//   +0x38: uint8_t      is_observing_all (bool flag)
//   +0x39-0x3F:         (padding, 7 bytes)
//   +0x40: AOVariant    cached_value     (0x10 bytes)
//
// The static methods (SetDValue, GetDValue) operate on a global registry
// (DAT_1002e618), protected by an ACE_Thread_Mutex (DAT_1002e648).
// Individual instances are observers that receive change notifications.
struct AODistributedValue {
    void*       vftable;                   // +0x00
    uint32_t    _unk_04;                   // +0x04
    uint32_t    signal_slot_head;          // +0x08  SignalBase_c
    AOString    name;                      // +0x0C
    uint32_t    _pad_24;                   // +0x24
    uint8_t     observed_container[0x10];  // +0x28
    uint8_t     is_observing_all;          // +0x38
    uint8_t     _pad_39[7];               // +0x39
    AOVariant   cached_value;              // +0x40
};
static_assert(sizeof(AODistributedValue) == 0x50);

}  // namespace aor

#pragma pack(pop)
