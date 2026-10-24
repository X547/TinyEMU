/*
 * Device tree reader
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "fdt.h"

#include "heap.h"

#define FDT_MAGIC      0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

namespace {

const uint8_t *sBlob;
FdtNode *sRoot;

/* The length of a node name's first component in 'path', up to '/'. */
size_t component_len(const char *path)
{
    size_t n = 0;
    while (path[n] && path[n] != '/')
        n++;
    return n;
}

/* A component with a unit address names one node; one without names the
   node of that name whatever its address. */
bool name_matches(const char *name, const char *comp, size_t len)
{
    if (strncmp(name, comp, len) != 0)
        return false;
    if (name[len] == '\0')
        return true;
    for (size_t i = 0; i < len; i++) {
        if (comp[i] == '@')
            return false;
    }
    return name[len] == '@';
}

}


void fdt_load(const void *blob)
{
    const uint8_t *b = (const uint8_t *)blob;
    if (be32(b) != FDT_MAGIC)
        panic("no device tree at %p", blob);
    sBlob = b;

    const uint8_t *p = b + be32(b + 8);
    const char *strings = (const char *)b + be32(b + 12);
    FdtNode *cur = nullptr;
    FdtNode *last_child[64] = {};
    FdtProp *last_prop = nullptr;
    int depth = 0;

    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        if (tok == FDT_BEGIN_NODE) {
            FdtNode *n = (FdtNode *)boot_alloc(sizeof(FdtNode));
            n->name = (const char *)p;
            p += align_up(strlen(n->name) + 1, 4);
            n->parent = cur;
            if (cur == nullptr) {
                sRoot = n;
            } else if (last_child[depth]) {
                last_child[depth]->sibling = n;
            } else {
                cur->child = n;
            }
            if (cur != nullptr)
                last_child[depth] = n;
            if (++depth >= (int)ARRAY_SIZE(last_child))
                panic("device tree nested too deep");
            last_child[depth] = nullptr;
            last_prop = nullptr;
            cur = n;
        } else if (tok == FDT_END_NODE) {
            if (cur == nullptr)
                panic("device tree: unbalanced nodes");
            cur = cur->parent;
            depth--;
            last_prop = nullptr;
        } else if (tok == FDT_PROP) {
            uint32_t len = be32(p);
            const char *name = strings + be32(p + 4);
            p += 8;
            if (cur == nullptr)
                panic("device tree: property outside a node");
            FdtProp *prop = (FdtProp *)boot_alloc(sizeof(FdtProp));
            prop->name = name;
            prop->data = p;
            prop->len = len;
            /* kept in blob order */
            if (last_prop)
                last_prop->next = prop;
            else
                cur->props = prop;
            last_prop = prop;
            if (strcmp(name, "phandle") == 0 && len == 4)
                cur->phandle = be32(p);
            p += align_up(len, 4);
        } else if (tok == FDT_NOP) {
            continue;
        } else if (tok == FDT_END) {
            break;
        } else {
            panic("device tree: bad token %#x", tok);
        }
    }
    if (sRoot == nullptr)
        panic("device tree without a root node");
}


uint32_t fdt_blob_size()
{
    return be32(sBlob + 4);
}


int fdt_reservation_count()
{
    const uint8_t *p = sBlob + be32(sBlob + 16);
    int n = 0;
    while (be64(p + 16 * n) || be64(p + 16 * n + 8))
        n++;
    return n;
}


void fdt_reservation(int index, uint64_t *addr, uint64_t *size)
{
    const uint8_t *p = sBlob + be32(sBlob + 16) + 16 * index;
    *addr = be64(p);
    *size = be64(p + 8);
}


const FdtNode *fdt_root()
{
    return sRoot;
}


const FdtNode *fdt_next(const FdtNode *node)
{
    if (node->child)
        return node->child;
    while (node) {
        if (node->sibling)
            return node->sibling;
        node = node->parent;
    }
    return nullptr;
}


const FdtNode *fdt_find_path(const char *path)
{
    if (path[0] != '/') {
        /* an alias, possibly followed by more of the path */
        const FdtNode *aliases = fdt_find_path("/aliases");
        if (aliases == nullptr)
            return nullptr;
        size_t len = component_len(path);
        for (const FdtProp *p = aliases->props; p; p = p->next) {
            if (strlen(p->name) == len && strncmp(p->name, path, len) == 0) {
                const FdtNode *n = fdt_find_path((const char *)p->data);
                if (n == nullptr || path[len] == '\0')
                    return n;
                /* the rest is relative to the alias's node */
                path += len;
                while (*path == '/')
                    path++;
                for (; n && *path;) {
                    size_t l = component_len(path);
                    const FdtNode *c = n->child;
                    while (c && !name_matches(c->name, path, l))
                        c = c->sibling;
                    n = c;
                    path += l;
                    while (*path == '/')
                        path++;
                }
                return n;
            }
        }
        return nullptr;
    }

    const FdtNode *n = sRoot;
    while (*path == '/')
        path++;
    while (n && *path) {
        size_t len = component_len(path);
        const FdtNode *c = n->child;
        while (c && !name_matches(c->name, path, len))
            c = c->sibling;
        n = c;
        path += len;
        while (*path == '/')
            path++;
    }
    return n;
}


const FdtNode *fdt_find_phandle(uint32_t phandle)
{
    for (const FdtNode *n = sRoot; n; n = fdt_next(n)) {
        if (n->phandle == phandle)
            return n;
    }
    return nullptr;
}


const FdtNode *fdt_find_compatible(const char *compat, const FdtNode *after)
{
    const FdtNode *n = after ? fdt_next(after) : sRoot;
    for (; n; n = fdt_next(n)) {
        if (fdt_is_compatible(n, compat) && fdt_is_enabled(n))
            return n;
    }
    return nullptr;
}


const FdtProp *fdt_prop(const FdtNode *node, const char *name)
{
    for (const FdtProp *p = node->props; p; p = p->next) {
        if (strcmp(p->name, name) == 0)
            return p;
    }
    return nullptr;
}


bool fdt_prop_u32(const FdtNode *node, const char *name, uint32_t *out)
{
    const FdtProp *p = fdt_prop(node, name);
    if (p == nullptr || p->len < 4)
        return false;
    *out = be32(p->data);
    return true;
}


const char *fdt_prop_str(const FdtNode *node, const char *name)
{
    const FdtProp *p = fdt_prop(node, name);
    if (p == nullptr || p->len == 0 || p->data[p->len - 1] != '\0')
        return nullptr;
    return (const char *)p->data;
}


bool fdt_is_compatible(const FdtNode *node, const char *compat)
{
    const FdtProp *p = fdt_prop(node, "compatible");
    if (p == nullptr)
        return false;
    for (uint32_t i = 0; i < p->len;) {
        const char *s = (const char *)p->data + i;
        if (strcmp(s, compat) == 0)
            return true;
        i += strnlen(s, p->len - i) + 1;
    }
    return false;
}


bool fdt_is_enabled(const FdtNode *node)
{
    const char *status = fdt_prop_str(node, "status");
    return status == nullptr || strcmp(status, "okay") == 0 ||
           strcmp(status, "ok") == 0;
}


int fdt_address_cells(const FdtNode *node)
{
    uint32_t v;
    return node && fdt_prop_u32(node, "#address-cells", &v) ? (int)v : 2;
}


int fdt_size_cells(const FdtNode *node)
{
    uint32_t v;
    return node && fdt_prop_u32(node, "#size-cells", &v) ? (int)v : 1;
}


uint64_t fdt_read_cells(const uint8_t *p, int cells)
{
    uint64_t v = 0;
    for (int i = 0; i < cells; i++)
        v = v << 32 | be32(p + 4 * i);
    return v;
}


int fdt_reg_count(const FdtNode *node)
{
    const FdtProp *reg = fdt_prop(node, "reg");
    if (reg == nullptr || node->parent == nullptr)
        return 0;
    int cells = fdt_address_cells(node->parent) + fdt_size_cells(node->parent);
    return cells ? reg->len / (4 * cells) : 0;
}


bool fdt_reg(const FdtNode *node, int index, uint64_t *addr, uint64_t *size)
{
    const FdtNode *bus = node->parent;
    const FdtProp *reg = fdt_prop(node, "reg");
    if (reg == nullptr || bus == nullptr)
        return false;
    int ac = fdt_address_cells(bus), sc = fdt_size_cells(bus);
    uint32_t stride = 4 * (ac + sc);
    if (stride == 0 || (uint32_t)(index + 1) * stride > reg->len)
        return false;
    const uint8_t *p = reg->data + index * stride;
    uint64_t a = fdt_read_cells(p, ac);
    *size = fdt_read_cells(p + 4 * ac, sc);

    /* up through every bus to the root */
    for (; bus->parent; bus = bus->parent) {
        const FdtProp *ranges = fdt_prop(bus, "ranges");
        if (ranges == nullptr)
            return false;
        if (ranges->len == 0)
            continue;
        int child_ac = fdt_address_cells(bus);
        int parent_ac = fdt_address_cells(bus->parent);
        int child_sc = fdt_size_cells(bus);
        uint32_t entry = 4 * (child_ac + parent_ac + child_sc);
        bool mapped = false;
        for (uint32_t off = 0; off + entry <= ranges->len; off += entry) {
            const uint8_t *r = ranges->data + off;
            uint64_t child = fdt_read_cells(r, child_ac);
            uint64_t parent = fdt_read_cells(r + 4 * child_ac, parent_ac);
            uint64_t len = fdt_read_cells(r + 4 * (child_ac + parent_ac),
                                          child_sc);
            if (a >= child && a - child < len) {
                a = a - child + parent;
                mapped = true;
                break;
            }
        }
        if (!mapped)
            return false;
    }
    *addr = a;
    return true;
}
