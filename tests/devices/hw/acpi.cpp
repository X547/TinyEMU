/*
 * ACPI tables and namespace
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
#include "acpi.h"

#include "heap.h"

#define MAX_TABLES 64

namespace {

const uint8_t *sTables[MAX_TABLES];
int sTableCount;
const uint8_t *sFadt;
AcpiNode *sRoot;

const uint8_t *ptr(uint64_t addr)
{
    return (const uint8_t *)(uintptr_t)addr;
}

void add_table(const uint8_t *t)
{
    if (t == nullptr || sTableCount == MAX_TABLES)
        return;
    sTables[sTableCount++] = t;
}


//#pragma mark - AML

struct NameString {
    bool rooted = false;
    int parents = 0;
    const uint8_t *segs = nullptr;
    int count = 0;
};

/* A PkgLength at *p: the end of what it covers. */
const uint8_t *pkg_end(const uint8_t *&p)
{
    const uint8_t *start = p;
    uint8_t lead = *p++;
    int follow = lead >> 6;
    uint32_t len;
    if (follow == 0) {
        len = lead & 0x3f;
    } else {
        len = lead & 0x0f;
        for (int i = 0; i < follow; i++)
            len |= (uint32_t)*p++ << (4 + 8 * i);
    }
    return start + len;
}

NameString parse_name(const uint8_t *&p)
{
    NameString n;
    if (*p == '\\') {
        n.rooted = true;
        p++;
    }
    while (*p == '^') {
        n.parents++;
        p++;
    }
    if (*p == 0x00) {           /* NullName */
        p++;
    } else if (*p == 0x2e) {    /* DualNamePrefix */
        n.segs = p + 1;
        n.count = 2;
        p += 1 + 8;
    } else if (*p == 0x2f) {    /* MultiNamePrefix */
        n.count = p[1];
        n.segs = p + 2;
        p += 2 + 4 * n.count;
    } else {
        n.segs = p;
        n.count = 1;
        p += 4;
    }
    return n;
}

bool is_name_start(uint8_t c)
{
    return c == '\\' || c == '^' || c == '_' || (c >= 'A' && c <= 'Z') ||
           c == 0x2e || c == 0x2f;
}

/* A data object; false for anything else, which is left unread. */
bool parse_data(const uint8_t *&p, const uint8_t *end, AmlValue *v)
{
    if (p >= end)
        return false;
    *v = AmlValue();
    switch (*p) {
    case 0x00:
        v->type = AML_INTEGER;
        v->integer = 0;
        p++;
        return true;
    case 0x01:
        v->type = AML_INTEGER;
        v->integer = 1;
        p++;
        return true;
    case 0xff:
        v->type = AML_INTEGER;
        v->integer = UINT64_MAX;
        p++;
        return true;
    case 0x0a:
        v->type = AML_INTEGER;
        v->integer = p[1];
        p += 2;
        return true;
    case 0x0b:
        v->type = AML_INTEGER;
        v->integer = le16(p + 1);
        p += 3;
        return true;
    case 0x0c:
        v->type = AML_INTEGER;
        v->integer = le32(p + 1);
        p += 5;
        return true;
    case 0x0e:
        v->type = AML_INTEGER;
        v->integer = le64(p + 1);
        p += 9;
        return true;
    case 0x0d:
        v->type = AML_STRING;
        v->data = p + 1;
        v->len = strnlen((const char *)p + 1, end - p - 1);
        p += 1 + v->len + 1;
        return true;
    case 0x11: {
        p++;
        const uint8_t *e = pkg_end(p);
        AmlValue size;
        if (!parse_data(p, e, &size) || size.type != AML_INTEGER)
            return false;
        v->type = AML_BUFFER;
        v->data = p;
        v->len = e - p;
        p = e;
        return true;
    }
    case 0x12:
    case 0x13: {
        bool var = *p == 0x13;
        p++;
        const uint8_t *e = pkg_end(p);
        if (var) {
            AmlValue n;
            if (!parse_data(p, e, &n))
                return false;
            v->count = n.integer;
        } else {
            v->count = *p++;
        }
        v->type = AML_PACKAGE;
        v->data = p;
        v->len = e - p;
        p = e;
        return true;
    }
    default:
        return false;
    }
}

AcpiNode *new_node(AcpiNode *parent, const uint8_t *seg, AcpiNodeKind kind)
{
    AcpiNode *n = (AcpiNode *)boot_alloc(sizeof(AcpiNode));
    memcpy(n->seg, seg, 4);
    n->kind = kind;
    n->parent = parent;
    /* appended, so the namespace keeps the tables' order */
    AcpiNode **link = &parent->child;
    while (*link)
        link = &(*link)->sibling;
    *link = n;
    return n;
}

AcpiNode *find_child(AcpiNode *parent, const uint8_t *seg)
{
    for (AcpiNode *c = parent->child; c; c = c->sibling) {
        if (memcmp(c->seg, seg, 4) == 0)
            return c;
    }
    return nullptr;
}

/* The node a declaration names, created along with any missing scope on
   the way. A Scope or Device of a name that exists already is the same
   node. */
AcpiNode *declare(AcpiNode *scope, const NameString &name, AcpiNodeKind kind)
{
    AcpiNode *n = name.rooted ? sRoot : scope;
    for (int i = 0; i < name.parents && n->parent; i++)
        n = n->parent;
    if (name.count == 0)
        return n;
    for (int i = 0; i < name.count; i++) {
        const uint8_t *seg = name.segs + 4 * i;
        bool last = i == name.count - 1;
        AcpiNode *c = find_child(n, seg);
        if (c == nullptr) {
            c = new_node(n, seg, last ? kind : ACPI_SCOPE);
        } else if (last && kind == ACPI_DEVICE) {
            c->kind = ACPI_DEVICE;
        }
        n = c;
    }
    return n;
}

void walk(AcpiNode *scope, const uint8_t *p, const uint8_t *end)
{
    while (p < end) {
        uint8_t op = *p;
        if (op == 0x10 || (op == 0x5b && p[1] == 0x82)) {
            /* Scope, Device */
            p += op == 0x10 ? 1 : 2;
            const uint8_t *e = pkg_end(p);
            NameString name = parse_name(p);
            AcpiNode *n = declare(scope, name,
                                  op == 0x10 ? ACPI_SCOPE : ACPI_DEVICE);
            walk(n, p, e);
            p = e;
        } else if (op == 0x08) {
            /* Name */
            p++;
            NameString name = parse_name(p);
            AcpiNode *n = declare(scope, name, ACPI_NAME);
            n->kind = ACPI_NAME;
            if (!parse_data(p, end, &n->value))
                return;
        } else if (op == 0x14) {
            /* Method: recorded, not run */
            p++;
            const uint8_t *e = pkg_end(p);
            NameString name = parse_name(p);
            declare(scope, name, ACPI_METHOD)->kind = ACPI_METHOD;
            p = e;
        } else if (op == 0xa0 || op == 0xa1 || op == 0xa2) {
            /* If, Else, While at the top level: skipped */
            p++;
            p = pkg_end(p);
        } else if (op == 0x5b && (p[1] == 0x81 || (p[1] >= 0x83 &&
                                                   p[1] <= 0x87))) {
            /* Field, Processor, PowerResource, ThermalZone, IndexField,
               BankField: all carry a length */
            p += 2;
            p = pkg_end(p);
        } else {
            /* nothing to skip it by: the rest of this list is lost */
            return;
        }
    }
}

void load_aml(const uint8_t *table)
{
    uint32_t len = le32(table + 4);
    walk(sRoot, table + 36, table + len);
}

}


void acpi_init(const void *rsdp_ptr)
{
    const uint8_t *rsdp = (const uint8_t *)rsdp_ptr;
    if (rsdp == nullptr || memcmp(rsdp, "RSD PTR ", 8) != 0)
        panic("no ACPI RSDP at %p", rsdp_ptr);

    const uint8_t *xsdt = rsdp[15] >= 2 ? ptr(le64(rsdp + 24)) : nullptr;
    if (xsdt && memcmp(xsdt, "XSDT", 4) == 0) {
        uint32_t n = (le32(xsdt + 4) - 36) / 8;
        for (uint32_t i = 0; i < n; i++)
            add_table(ptr(le64(xsdt + 36 + 8 * i)));
    } else {
        const uint8_t *rsdt = ptr(le32(rsdp + 16));
        if (memcmp(rsdt, "RSDT", 4) != 0)
            panic("no ACPI RSDT or XSDT");
        uint32_t n = (le32(rsdt + 4) - 36) / 4;
        for (uint32_t i = 0; i < n; i++)
            add_table(ptr(le32(rsdt + 36 + 4 * i)));
    }

    sFadt = acpi_table("FACP");
    if (sFadt) {
        uint32_t len = le32(sFadt + 4);
        uint64_t dsdt = len >= 148 ? le64(sFadt + 140) : 0;
        if (dsdt == 0)
            dsdt = le32(sFadt + 40);
        add_table(ptr(dsdt));
    }

    static const uint8_t root_seg[4] = {'\\', '_', '_', '_'};
    sRoot = (AcpiNode *)boot_alloc(sizeof(AcpiNode));
    memcpy(sRoot->seg, root_seg, 4);
    sRoot->kind = ACPI_SCOPE;
    const uint8_t *dsdt = acpi_table("DSDT");
    if (dsdt)
        load_aml(dsdt);
    for (int i = 0; const uint8_t *ssdt = acpi_table("SSDT", i); i++)
        load_aml(ssdt);
}


bool acpi_present()
{
    return sTableCount > 0;
}


const uint8_t *acpi_table(const char *signature, int index)
{
    for (int i = 0; i < sTableCount; i++) {
        if (memcmp(sTables[i], signature, 4) == 0 && index-- == 0)
            return sTables[i];
    }
    return nullptr;
}


int acpi_table_count()
{
    return sTableCount;
}


const uint8_t *acpi_table_at(int index)
{
    return sTables[index];
}


bool acpi_pm_timer(uint16_t *port, bool *ext32)
{
    if (sFadt == nullptr || le32(sFadt + 76) == 0)
        return false;
    *port = le32(sFadt + 76);
    *ext32 = (le32(sFadt + 112) >> 8) & 1; /* TMR_VAL_EXT */
    return true;
}


bool acpi_pm1_control(uint16_t *pm1a, uint16_t *pm1b)
{
    if (sFadt == nullptr || le32(sFadt + 64) == 0)
        return false;
    *pm1a = le32(sFadt + 64);
    *pm1b = le32(sFadt + 68);
    return true;
}


const AcpiNode *acpi_root()
{
    return sRoot;
}


const AcpiNode *acpi_next(const AcpiNode *node)
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


const AcpiNode *acpi_child(const AcpiNode *node, const char *seg)
{
    for (const AcpiNode *c = node->child; c; c = c->sibling) {
        if (memcmp(c->seg, seg, 4) == 0)
            return c;
    }
    return nullptr;
}


const AcpiNode *acpi_find(const char *path, const AcpiNode *scope)
{
    const AcpiNode *n = scope ? scope : sRoot;
    if (*path == '\\') {
        n = sRoot;
        path++;
    }
    while (*path == '^') {
        if (n->parent)
            n = n->parent;
        path++;
    }
    while (n && *path) {
        char seg[4] = {'_', '_', '_', '_'};
        int i = 0;
        while (*path && *path != '.') {
            if (i < 4)
                seg[i++] = *path;
            path++;
        }
        if (*path == '.')
            path++;
        n = acpi_child(n, seg);
    }
    return n;
}


void acpi_path(const AcpiNode *node, char *buf, size_t size)
{
    const AcpiNode *chain[32];
    int depth = 0;
    for (; node && node->parent && depth < 32; node = node->parent)
        chain[depth++] = node;
    size_t len = 0;
    auto put = [&](char c) {
        if (len + 1 < size)
            buf[len] = c;
        len++;
    };
    put('\\');
    for (int i = depth - 1; i >= 0; i--) {
        for (int j = 0; j < 4; j++)
            put(chain[i]->seg[j]);
        if (i)
            put('.');
    }
    if (size)
        buf[min(len, size - 1)] = '\0';
}


bool aml_package_element(const AmlValue &pkg, uint32_t index, AmlValue *out)
{
    if (pkg.type != AML_PACKAGE || index >= pkg.count)
        return false;
    const uint8_t *p = pkg.data, *end = pkg.data + pkg.len;
    for (uint32_t i = 0; i <= index; i++) {
        if (p >= end)
            return false;
        if (!parse_data(p, end, out)) {
            /* a name reference: not a value */
            if (!is_name_start(*p))
                return false;
            parse_name(p);
            *out = AmlValue();
        }
    }
    return true;
}


bool aml_id_string(const AmlValue &v, char *buf, size_t size)
{
    if (size == 0)
        return false;
    if (v.type == AML_STRING) {
        size_t n = min((size_t)v.len, size - 1);
        memcpy(buf, v.data, n);
        buf[n] = '\0';
        return true;
    }
    if (v.type != AML_INTEGER || size < 8)
        return false;
    /* EISAID: three 5 bit letters and four hex digits, byte swapped */
    uint32_t id = (uint32_t)v.integer;
    uint32_t swapped = (id & 0xff) << 24 | (id & 0xff00) << 8 |
                       (id >> 8 & 0xff00) | id >> 24;
    const char *hex = "0123456789ABCDEF";
    buf[0] = '@' + ((swapped >> 26) & 0x1f);
    buf[1] = '@' + ((swapped >> 21) & 0x1f);
    buf[2] = '@' + ((swapped >> 16) & 0x1f);
    buf[3] = hex[(swapped >> 12) & 0xf];
    buf[4] = hex[(swapped >> 8) & 0xf];
    buf[5] = hex[(swapped >> 4) & 0xf];
    buf[6] = hex[swapped & 0xf];
    buf[7] = '\0';
    return true;
}


bool acpi_parse_resources(const AmlValue &buffer,
                          void (*fn)(void *ctx, const AcpiResource &res),
                          void *ctx)
{
    if (buffer.type != AML_BUFFER)
        return false;
    const uint8_t *p = buffer.data, *end = buffer.data + buffer.len;
    while (p < end) {
        AcpiResource r {};
        uint8_t tag = *p;
        if (!(tag & 0x80)) {
            uint8_t type = (tag >> 3) & 0xf;
            uint32_t len = tag & 7;
            const uint8_t *d = p + 1;
            if (d + len > end)
                return false;
            r.type = type;
            r.raw = p;
            r.raw_len = 1 + len;
            if (type == 0xf) {
                return true;            /* End Tag */
            } else if (type == 0x4 && len >= 2) {
                uint16_t mask = le16(d);
                uint8_t flags = len >= 3 ? d[2] : 0x01;
                r.kind = ACPI_RES_IRQ;
                r.level = !(flags & 0x01);
                r.active_low = flags & 0x08;
                r.length = 1;
                for (int line = 0; line < 16; line++) {
                    if (mask & (1 << line)) {
                        r.base = line;
                        fn(ctx, r);
                    }
                }
            } else if (type == 0x8 && len >= 7) {
                r.kind = ACPI_RES_IO;
                r.base = le16(d + 1);
                r.length = d[6];
                fn(ctx, r);
            } else if (type == 0x9 && len >= 3) {
                r.kind = ACPI_RES_IO;
                r.base = le16(d) & 0x3ff;
                r.length = d[2];
                fn(ctx, r);
            } else {
                r.kind = ACPI_RES_OTHER;
                fn(ctx, r);
            }
            p = d + len;
        } else {
            if (p + 3 > end)
                return false;
            uint8_t type = tag & 0x7f;
            uint32_t len = le16(p + 1);
            const uint8_t *d = p + 3;
            if (d + len > end)
                return false;
            r.type = type;
            r.raw = p;
            r.raw_len = 3 + len;
            if (type == 0x06 && len >= 9) {
                r.kind = ACPI_RES_MEMORY;
                r.base = le32(d + 1);
                r.length = le32(d + 5);
                fn(ctx, r);
            } else if (type == 0x05 && len >= 17) {
                r.kind = ACPI_RES_MEMORY;
                r.base = le32(d + 1);
                r.length = le32(d + 13);
                fn(ctx, r);
            } else if (type == 0x09 && len >= 6) {
                uint8_t flags = d[0];
                uint8_t count = d[1];
                r.kind = ACPI_RES_INTERRUPT;
                r.level = !(flags & 0x02);
                r.active_low = flags & 0x04;
                r.length = 1;
                for (uint8_t i = 0; i < count && 2 + 4u * (i + 1) <= len; i++) {
                    r.base = le32(d + 2 + 4 * i);
                    fn(ctx, r);
                }
            } else if ((type == 0x07 && len >= 23) ||
                       (type == 0x08 && len >= 13) ||
                       (type == 0x0a && len >= 43)) {
                /* Word, DWord and QWord address space descriptors */
                int w = type == 0x08 ? 2 : type == 0x07 ? 4 : 8;
                uint8_t space = d[0];
                const uint8_t *min_p = d + 3 + w;
                const uint8_t *len_p = d + 3 + 4 * w;
                auto rd = [w](const uint8_t *q) -> uint64_t {
                    return w == 2 ? le16(q) : w == 4 ? le32(q) : le64(q);
                };
                r.kind = space == 0 ? ACPI_RES_MEMORY
                       : space == 1 ? ACPI_RES_IO
                       : space == 2 ? ACPI_RES_BUS : ACPI_RES_OTHER;
                r.base = rd(min_p);
                r.length = rd(len_p);
                fn(ctx, r);
            } else {
                r.kind = ACPI_RES_OTHER;
                fn(ctx, r);
            }
            p = d + len;
        }
    }
    return true;
}
