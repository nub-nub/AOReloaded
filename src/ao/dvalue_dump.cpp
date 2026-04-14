#include "ao/dvalue_dump.h"
#include "ao/types.h"
#include "core/logging.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Walk the internal std::map<String, ValueEntry> that backs the
// DistributedValue_c system. This is a direct memory walk of the
// MSVC 2010 red-black tree structure inside Utils.dll.
//
// RVAs determined from Ghidra analysis of Utils.dll:
//   _Myhead (sentinel node pointer): RVA 0x2e61c
//   _Mysize (element count):         RVA 0x2e620
//
// Tree node layout (MSVC 2010 std::map internal _Node):
//   +0x00: Node* _Left
//   +0x04: Node* _Parent
//   +0x08: Node* _Right
//   +0x0C: AOString key        (0x18 bytes)
//   +0x24: ...mapped value...
//   +0x50: AOVariant value     (0x10 bytes)
//   +0x88: int32_t category    (DValueCategory_e)
//   +0xA0: char _Color         (0=red, 1=black)
//   +0xA1: char _Isnil         (0=real node, 1=sentinel)

namespace aor {

// Needs to be outside the anonymous namespace for offsetof on clang.
#pragma pack(push, 4)
struct DValueTreeNode {
    DValueTreeNode* left;    // +0x00
    DValueTreeNode* parent;  // +0x04
    DValueTreeNode* right;   // +0x08
    AOString   key;       // +0x0C (0x18 bytes)
    uint8_t    _gap[0x2C]; // +0x24 to +0x50 (gap — default variant, flags, etc.)
    AOVariant  value;     // +0x50 (0x10 bytes)
    uint8_t    _gap2[0x28]; // +0x60 to +0x88
    int32_t    category;  // +0x88
    uint8_t    _gap3[0x14]; // +0x8C to +0xA0 (observers vector, etc.)
    char       color;     // +0xA0
    char       isnil;     // +0xA1
};
#pragma pack(pop)
static_assert(offsetof(DValueTreeNode, key)      == 0x0C);
static_assert(offsetof(DValueTreeNode, value)    == 0x50);
static_assert(offsetof(DValueTreeNode, category) == 0x88);
static_assert(offsetof(DValueTreeNode, isnil)    == 0xA1);

namespace {

constexpr uint32_t kMyHeadRVA = 0x2e61c;
constexpr uint32_t kMySizeRVA = 0x2e620;

// In-order successor in MSVC's red-black tree.
// Mirrors the game's own iterator increment at Utils.dll+0x3a49.
DValueTreeNode* TreeNext(DValueTreeNode* node) {
    if (node->isnil) return node;

    if (!node->right->isnil) {
        // Go right, then left as far as possible.
        DValueTreeNode* cur = node->right;
        while (!cur->left->isnil) {
            cur = cur->left;
        }
        return cur;
    }

    // Go up while we're the right child.
    DValueTreeNode* par = node->parent;
    while (!par->isnil && node == par->right) {
        node = par;
        par = par->parent;
    }
    return par;
}

const char* VariantTypeName(uint32_t type) {
    switch (type) {
        case 0:  return "None";
        case 4:  return "Int";
        case 5:  return "Int64";
        case 6:  return "Bool";
        case 7:  return "Float";
        case 8:  return "Double";
        case 9:  return "String";
        case 10: return "IRect";
        case 11: return "IPoint";
        case 12: return "Message";
        case 16: return "Rect";
        case 17: return "Point";
        default: return "Unknown";
    }
}

}  // namespace

void DumpAllDValues() {
    HMODULE utils = GetModuleHandleA("Utils.dll");
    if (!utils) {
        Log("[dump] Utils.dll not loaded");
        return;
    }

    auto base = reinterpret_cast<uint8_t*>(utils);

    // Read the map's internal pointers (raw byte offsets, no struct).
    uint8_t* head = nullptr;
    uint32_t count = 0;

    __try {
        head = *reinterpret_cast<uint8_t**>(base + kMyHeadRVA);
        count = *reinterpret_cast<uint32_t*>(base + kMySizeRVA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[dump] failed to read map header (AV)");
        return;
    }

    if (!head || count == 0) {
        Log("[dump] DValue map empty/uninit (head=%p, size=%u)", head, count);
        return;
    }

    Log("[dump] ===== DValue Registry (%u entries) =====", count);

    // Verified node layout from hex dump:
    //   +0x00: Node* Left
    //   +0x04: Node* Parent
    //   +0x08: Node* Right
    //   +0x0C: uint32_t (unknown — possibly _Color/_Isnil packed, or padding)
    //   +0x10: AOString key         (0x18 bytes)
    //   +0x28: AOString default_key (0x18 bytes, duplicate of key)
    //   +0x50: uint32_t variant_type (Variant tag at +0x50, NOT at +0x54)
    //   +0x58: value payload        (8 bytes)
    //   +0x88: int32_t category
    //   +0xA1: char _Isnil
    constexpr int kKeyOffset     = 0x10;
    constexpr int kVariantOffset = 0x50;  // Variant type tag
    constexpr int kValueOffset   = 0x58;  // Variant value payload
    constexpr int kCatOffset     = 0x88;
    constexpr int kIsnilOffset   = 0xA1;

    uint8_t* cur = nullptr;
    __try {
        cur = *reinterpret_cast<uint8_t**>(head);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[dump] failed to read head->left");
        return;
    }

    uint32_t logged = 0;
    while (logged < count + 10) {
        __try {
            if (cur[kIsnilOffset] != 0) break;

            auto* key = reinterpret_cast<AOString*>(cur + kKeyOffset);
            const char* name = key->c_str();
            if (!name) name = "(null)";

            uint32_t vtype = *reinterpret_cast<uint32_t*>(cur + kVariantOffset);
            int32_t cat = *reinterpret_cast<int32_t*>(cur + kCatOffset);

            switch (vtype) {
                case 4: {
                    int32_t v = *reinterpret_cast<int32_t*>(cur + kValueOffset);
                    Log("[dv] [%d] %-45s = %d (Int)", cat, name, v);
                    break;
                }
                case 5: {
                    int64_t v = *reinterpret_cast<int64_t*>(cur + kValueOffset);
                    Log("[dv] [%d] %-45s = %lld (Int64)", cat, name, v);
                    break;
                }
                case 6: {
                    bool v = *reinterpret_cast<bool*>(cur + kValueOffset);
                    Log("[dv] [%d] %-45s = %s (Bool)", cat, name,
                        v ? "true" : "false");
                    break;
                }
                case 7: {
                    float v = *reinterpret_cast<float*>(cur + kValueOffset);
                    Log("[dv] [%d] %-45s = %.4f (Float)", cat, name,
                        static_cast<double>(v));
                    break;
                }
                case 8: {
                    double v = *reinterpret_cast<double*>(cur + kValueOffset);
                    Log("[dv] [%d] %-45s = %.4f (Double)", cat, name, v);
                    break;
                }
                default:
                    Log("[dv] [%d] %-45s = <type %u>", cat, name, vtype);
                    break;
            }

            // In-order tree traversal.
            uint8_t* right = *reinterpret_cast<uint8_t**>(cur + 0x08);
            if (right[kIsnilOffset] == 0) {
                cur = right;
                uint8_t* left = *reinterpret_cast<uint8_t**>(cur);
                while (left[kIsnilOffset] == 0) {
                    cur = left;
                    left = *reinterpret_cast<uint8_t**>(cur);
                }
            } else {
                uint8_t* par = *reinterpret_cast<uint8_t**>(cur + 0x04);
                while (par[kIsnilOffset] == 0 &&
                       cur == *reinterpret_cast<uint8_t**>(par + 0x08)) {
                    cur = par;
                    par = *reinterpret_cast<uint8_t**>(cur + 0x04);
                }
                cur = par;
            }

            logged++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[dump] AV at node %p after %u entries", cur, logged);
            break;
        }
    }

    Log("[dump] ===== End (%u/%u entries) =====", logged, count);
}

}  // namespace aor
