/*
 * VM utilities
 * 
 * Copyright (c) 2017 Fabrice Bellard
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "machine.h"
#include "fs_utils.h"
#ifdef CONFIG_FS_NET
#include "fs_wget.h"
#endif

void __attribute__((format(printf, 1, 2))) vm_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

int vm_get_int(JSONValue obj, const char *name, int *pval)
{ 
    JSONValue val;
    val = json_object_get(obj, name);
    if (json_is_undefined(val)) {
        vm_error("expecting '%s' property\n", name);
        return -1;
    }
    if (val.type != JSON_INT) {
        vm_error("%s: integer expected\n", name);
        return -1;
    }
    *pval = val.u.int32;
    return 0;
}

int vm_get_int_opt(JSONValue obj, const char *name, int *pval, int def_val)
{ 
    JSONValue val;
    val = json_object_get(obj, name);
    if (json_is_undefined(val)) {
        *pval = def_val;
        return 0;
    }
    if (val.type != JSON_INT) {
        vm_error("%s: integer expected\n", name);
        return -1;
    }
    *pval = val.u.int32;
    return 0;
}

static int vm_get_str2(JSONValue obj, const char *name, const char **pstr,
                       bool is_opt);

int vm_get_str(JSONValue obj, const char *name, const char **pstr)
{
    return vm_get_str2(obj, name, pstr, false);
}

int vm_get_str_opt(JSONValue obj, const char *name, const char **pstr)
{
    return vm_get_str2(obj, name, pstr, true);
}

static int vm_get_str2(JSONValue obj, const char *name, const char **pstr,
                      bool is_opt)
{ 
    JSONValue val;
    val = json_object_get(obj, name);
    if (json_is_undefined(val)) {
        if (is_opt) {
            *pstr = NULL;
            return 0;
        } else {
            vm_error("expecting '%s' property\n", name);
            return -1;
        }
    }
    if (val.type != JSON_STR) {
        vm_error("%s: string expected\n", name);
        return -1;
    }
    *pstr = val.u.str->data;
    return 0;
}

static char *strdup_null(const char *str)
{
    if (!str)
        return NULL;
    else
        return strdup(str);
}

/* currently only for "TZ" */
static char *cmdline_subst(const char *cmdline)
{
    DynBuf dbuf;
    const char *p;
    char var_name[32], *q, buf[32];
    
    dbuf_init(&dbuf);
    p = cmdline;
    while (*p != '\0') {
        if (p[0] == '$' && p[1] == '{') {
            p += 2;
            q = var_name;
            while (*p != '\0' && *p != '}') {
                if ((q - var_name) < sizeof(var_name) - 1)
                    *q++ = *p;
                p++;
            }
            *q = '\0';
            if (*p == '}')
                p++;
            if (!strcmp(var_name, "TZ")) {
                time_t ti;
                struct tm tm;
                int n, sg;
                /* get the offset to UTC */
                time(&ti);
                localtime_r(&ti, &tm);
                n = tm.tm_gmtoff / 60;
                sg = '-';
                if (n < 0) {
                    sg = '+';
                    n = -n;
                }
                snprintf(buf, sizeof(buf), "UTC%c%02d:%02d",
                         sg, n / 60, n % 60);
                dbuf_putstr(&dbuf, buf);
            }
        } else {
            dbuf_putc(&dbuf, *p++);
        }
    }
    dbuf_putc(&dbuf, 0);
    return (char *)dbuf.buf;
}

static bool find_name(const char *name, const char *name_list)
{
    size_t len;
    const char *p, *r;
    
    p = name_list;
    for(;;) {
        r = strchr(p, ',');
        if (!r) {
            if (!strcmp(name, p))
                return true;
            break;
        } else {
            len = r - p;
            if (len == strlen(name) && !memcmp(name, p, len))
                return true;
            p = r + 1;
        }
    }
    return false;
}

static const VirtMachineClass *virt_machine_list[] = {
    &gRiscvMachineClass,
#ifdef CONFIG_X86EMU
    &gPcMachineClass,
#endif
    NULL,
};

static const VirtMachineClass *virt_machine_find_class(const char *machine_name)
{
    for (const VirtMachineClass *vmc : virt_machine_list) {
        if (vmc == NULL)
            break;
        if (find_name(machine_name, vmc->MachineNames()))
            return vmc;
    }
    return NULL;
}

//#pragma mark - device tree

static void free_device_list(VMDeviceNode *node)
{
    while (node != NULL) {
        VMDeviceNode *next = node->next;
        free_device_list(node->children);
        free(node->type);
        free(node->id);
        free(node->filename);
        free(node->child_bus_type);
        free(node);
        node = next;
    }
}

void vm_walk_devices(VMDeviceNode *node, VMDeviceNodeVisitor visit,
                     void *opaque)
{
    for (; node != NULL; node = node->next) {
        visit(node, opaque);
        vm_walk_devices(node->children, visit, opaque);
    }
}

static int parse_bus(JSONValue bus_obj, VMDeviceNode *owner,
                     VMDeviceNode **list_out);

/* A device is { type: "...", id: "...", <device properties> }, plus an
   optional "bus" object when the device provides one. */
static VMDeviceNode *parse_device(JSONValue obj, VMDeviceNode *parent)
{
    const char *str;
    VMDeviceNode *node;
    JSONValue bus;

    if (obj.type != JSON_OBJ) {
        vm_error("device: object expected\n");
        return NULL;
    }

    node = mallocz_t<VMDeviceNode>();
    node->props = obj;
    node->parent = parent;

    if (vm_get_str(obj, "type", &str) < 0)
        goto fail;
    node->type = strdup(str);

    if (vm_get_str_opt(obj, "id", &str) < 0)
        goto fail;
    node->id = strdup_null(str);

    if (vm_get_str_opt(obj, "file", &str) < 0)
        goto fail;
    node->filename = strdup_null(str);

    bus = json_object_get(obj, "bus");
    if (!json_is_undefined(bus)) {
        if (parse_bus(bus, node, &node->children) < 0)
            goto fail;
    }
    return node;

 fail:
    free_device_list(node);
    return NULL;
}

/* A bus is { type: "...", devices: [ ... ] }. */
static int parse_bus(JSONValue bus_obj, VMDeviceNode *owner,
                     VMDeviceNode **list_out)
{
    const char *bus_type;
    JSONValue devices;
    VMDeviceNode *first = NULL;
    VMDeviceNode **tail = &first;
    int count = 0;

    *list_out = NULL;

    if (bus_obj.type != JSON_OBJ) {
        vm_error("bus: object expected\n");
        return -1;
    }
    if (vm_get_str(bus_obj, "type", &bus_type) < 0)
        return -1;
    /* Kept so that the machine can check it against the bus the owning device
       really provides, rather than accepting any name at all. */
    if (owner != NULL)
        owner->child_bus_type = strdup(bus_type);

    devices = json_object_get(bus_obj, "devices");
    if (json_is_undefined(devices))
        return 0;
    if (devices.type != JSON_ARRAY) {
        vm_error("%s bus: 'devices' must be an array\n", bus_type);
        return -1;
    }

    for (int i = 0; i < devices.u.array->len; i++) {
        VMDeviceNode *node = parse_device(json_array_get(devices, i), owner);
        if (node == NULL) {
            free_device_list(first);
            return -1;
        }
        *tail = node;
        tail = &node->next;
        count++;
    }
    if (owner != NULL)
        owner->child_count = count;
    *list_out = first;
    return 0;
}

/* The display window is opened before the machine is built, so its size has
   to be known ahead of the device that provides it. Nothing else is taken out
   of the tree here: every device is instantiated from the tree itself. */
static void flatten_visit(VMDeviceNode *node, void *opaque)
{
    VirtMachineParams *p = static_cast<VirtMachineParams *>(opaque);

    if (strcmp(node->type, "simplefb") != 0 &&
        strcmp(node->type, "vga") != 0)
        return;

    free(p->display_device);
    p->display_device = strdup(node->type);
    vm_get_int_opt(node->props, "width", &p->width, 800);
    vm_get_int_opt(node->props, "height", &p->height, 600);
}

static int flatten_device_tree(VirtMachineParams *p)
{
    vm_walk_devices(p->root_devices, flatten_visit, p);
    return 0;
}

//#pragma mark - configuration

static int virt_machine_parse_config(VirtMachineParams *p,
                                     char *config_file_str, int len)
{
    int version, val;
    const char *tag_name, *str;
    JSONValue cfg, obj, el;

    cfg = json_parse_value_len(config_file_str, len);
    if (json_is_error(cfg)) {
        vm_error("error: %s\n", json_get_error(cfg));
        json_free(cfg);
        return -1;
    }

    if (vm_get_int(cfg, "version", &version) < 0)
        goto tag_fail;
    if (version != VM_CONFIG_VERSION) {
        if (version > VM_CONFIG_VERSION) {
            vm_error("The emulator is too old to run this VM: please upgrade\n");
        } else {
            vm_error("This configuration file uses format version %d. Version "
                     "%d declares devices hierarchically inside buses; see "
                     "the sample configuration.\n",
                     version, VM_CONFIG_VERSION);
        }
        goto tag_fail;
    }
    
    if (vm_get_str(cfg, "machine", &str) < 0)
        goto tag_fail;
    p->machine_name = strdup(str);
    p->vmc = virt_machine_find_class(p->machine_name);
    if (!p->vmc) {
        vm_error("Unknown machine name: %s\n", p->machine_name);
        goto tag_fail;
    }
    p->vmc->SetDefaults(p);

    tag_name = "memory_size";
    if (vm_get_int(cfg, tag_name, &val) < 0)
        goto tag_fail;
    p->ram_size = (uint64_t)val << 20;

    if (vm_get_int_opt(cfg, "cpus", &p->cpu_count, 1) < 0)
        goto tag_fail;

    tag_name = "bios";
    if (vm_get_str_opt(cfg, tag_name, &str) < 0)
        goto tag_fail;
    if (str) {
        p->files[VM_FILE_BIOS].filename = strdup(str);
    }

    tag_name = "kernel";
    if (vm_get_str_opt(cfg, tag_name, &str) < 0)
        goto tag_fail;
    if (str) {
        p->files[VM_FILE_KERNEL].filename = strdup(str);
    }

    tag_name = "initrd";
    if (vm_get_str_opt(cfg, tag_name, &str) < 0)
        goto tag_fail;
    if (str) {
        p->files[VM_FILE_INITRD].filename = strdup(str);
    }

    if (vm_get_str_opt(cfg, "cmdline", &str) < 0)
        goto tag_fail;
    if (str) {
        p->cmdline = cmdline_subst(str);
    }
    
    obj = json_object_get(cfg, "bus");
    if (json_is_undefined(obj)) {
        vm_error("expecting a 'bus' property describing the root bus\n");
        goto tag_fail;
    }
    if (vm_get_str(obj, "type", &str) < 0)
        goto tag_fail;
    p->root_bus_type = strdup(str);
    if (parse_bus(obj, NULL, &p->root_devices) < 0)
        goto tag_fail;
    if (flatten_device_tree(p) < 0)
        goto tag_fail;

    if (vm_get_str_opt(cfg, "vga_bios", &str) < 0)
        goto tag_fail;
    if (str) {
        p->files[VM_FILE_VGA_BIOS].filename = strdup(str);
    }

    if (vm_get_str_opt(cfg, "accel", &str) < 0)
        goto tag_fail;
    if (str) {
        if (!strcmp(str, "none")) {
            p->accel_enable = false;
        } else if (!strcmp(str, "auto")) {
            p->accel_enable = true;
        } else {
            vm_error("unsupported 'accel' config: %s\n", str);
            return -1;
        }
    }

    tag_name = "rtc_local_time";
    el = json_object_get(cfg, tag_name);
    if (!json_is_undefined(el)) {
        if (el.type != JSON_BOOL) {
            vm_error("%s: boolean expected\n", tag_name);
            goto tag_fail;
        }
        p->rtc_local_time = el.u.b;
    }

    /* The device nodes hold JSONValues pointing into this tree, so it stays
       alive until virt_machine_free_config(). */
    p->cfg_json = cfg;
    return 0;
 tag_fail:
    free_device_list(p->root_devices);
    p->root_devices = NULL;
    json_free(cfg);
    return -1;
}

/* Receives a config or auxiliary file once it has been read, whether from
   disk or over HTTP. */
class FSLoadFileHandler {
public:
    virtual ~FSLoadFileHandler() = default;

    virtual void FileLoaded(uint8_t *buf, int buf_len) = 0;
};

typedef struct VMConfigLoadState VMConfigLoadState;

#ifdef CONFIG_FS_NET
/* Bridges the HTTP transfer of a config file back into the loader. */
class ConfigLoadWriteHandler final: public WGetWriteHandler {
private:
    VMConfigLoadState &fState;

public:
    ConfigLoadWriteHandler(VMConfigLoadState &state): fState(state) {}

    void WGetWrite(int err, void *data, size_t size) override;
};
#endif


/* The two stages of loading a config: the file itself, then each auxiliary
   file it names. */
class ConfigFileLoaded final: public FSLoadFileHandler {
private:
    VMConfigLoadState &fState;

public:
    ConfigFileLoaded(VMConfigLoadState &state): fState(state) {}

    void FileLoaded(uint8_t *buf, int buf_len) override;
};

class AdditionalFileLoaded final: public FSLoadFileHandler {
private:
    VMConfigLoadState &fState;

public:
    AdditionalFileLoaded(VMConfigLoadState &state): fState(state) {}

    void FileLoaded(uint8_t *buf, int buf_len) override;
};

struct VMConfigLoadState {
    VirtMachineParams *vm_params;
    StartCallback *start;

    FSLoadFileHandler *file_load_handler;
    int file_index;
#ifdef CONFIG_FS_NET
    ConfigLoadWriteHandler write_handler {*this};
#endif
    ConfigFileLoaded config_file_loaded {*this};
    AdditionalFileLoaded additional_file_loaded {*this};
};

static void config_additional_file_load(VMConfigLoadState *s);

/* XXX: win32, URL */
char *get_file_path(const char *base_filename, const char *filename)
{
    int len, len1;
    char *fname;
    const char *p;
    
    if (!base_filename)
        goto done;
    if (strchr(filename, ':'))
        goto done; /* full URL */
    if (filename[0] == '/')
        goto done;
    p = strrchr(base_filename, '/');
    if (!p) {
    done:
        return strdup(filename);
    }
    len = p + 1 - base_filename;
    len1 = strlen(filename);
    fname = static_cast<char *>(malloc(len + len1 + 1));
    memcpy(fname, base_filename, len);
    memcpy(fname + len, filename, len1 + 1);
    return fname;
}


/* return -1 if error. */
static int load_file(uint8_t **pbuf, const char *filename)
{
    FILE *f;
    int size;
    uint8_t *buf;
    
    f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = static_cast<uint8_t *>(malloc(size));
    if (fread(buf, 1, size, f) != size) {
        fprintf(stderr, "%s: read error\n", filename);
        exit(1);
    }
    fclose(f);
    *pbuf = buf;
    return size;
}

#ifdef CONFIG_FS_NET
void ConfigLoadWriteHandler::WGetWrite(int err, void *data, size_t size)
{
    VMConfigLoadState *s = &fState;
    
    //    printf("err=%d data=%p size=%ld\n", err, data, size);
    if (err < 0) {
        vm_error("Error %d while loading file\n", -err);
        exit(1);
    }
    s->file_load_handler->FileLoaded(static_cast<uint8_t *>(data), size);
}
#endif

static void config_load_file(VMConfigLoadState *s, const char *filename,
                             FSLoadFileHandler *handler)
{
    //    printf("loading %s\n", filename);
#ifdef CONFIG_FS_NET
    if (is_url(filename)) {
        s->file_load_handler = handler;
        fs_wget(filename, NULL, NULL, &s->write_handler, true);
    } else
#endif
    {
        uint8_t *buf;
        int size;
        size = load_file(&buf, filename);
        handler->FileLoaded(buf, size);
        free(buf);
    }
}

void virt_machine_load_config_file(VirtMachineParams *p,
                                   const char *filename,
                                   StartCallback *start)
{
    VMConfigLoadState *s;
    
    s = new VMConfigLoadState();
    s->vm_params = p;
    s->start = start;
    p->cfg_filename = strdup(filename);

    config_load_file(s, filename, &s->config_file_loaded);
}

void ConfigFileLoaded::FileLoaded(uint8_t *buf, int buf_len)
{
    VMConfigLoadState *s = &fState;
    VirtMachineParams *p = s->vm_params;

    if (virt_machine_parse_config(p, (char *)buf, buf_len) < 0)
        exit(1);
    
    /* load the additional files */
    s->file_index = 0;
    config_additional_file_load(s);
}

static void config_additional_file_load(VMConfigLoadState *s)
{
    VirtMachineParams *p = s->vm_params;
    while (s->file_index < VM_FILE_COUNT &&
           p->files[s->file_index].filename == NULL) {
        s->file_index++;
    }
    if (s->file_index == VM_FILE_COUNT) {
        if (s->start != nullptr) {
            s->start->Start();
        }
        delete s;
    } else {
        char *fname;
        
        fname = get_file_path(p->cfg_filename,
                              p->files[s->file_index].filename);
        config_load_file(s, fname, &s->additional_file_loaded);
        free(fname);
    }
}

void AdditionalFileLoaded::FileLoaded(uint8_t *buf, int buf_len)
{
    VMConfigLoadState *s = &fState;
    VirtMachineParams *p = s->vm_params;

    p->files[s->file_index].buf = static_cast<uint8_t *>(malloc(buf_len));
    memcpy(p->files[s->file_index].buf, buf, buf_len);
    p->files[s->file_index].len = buf_len;

    /* load the next files */
    s->file_index++;
    config_additional_file_load(s);
}

void vm_add_cmdline(VirtMachineParams *p, const char *cmdline)
{
    char *new_cmdline;
    const char *old_cmdline;
    if (cmdline[0] == '!') {
        new_cmdline = strdup(cmdline + 1);
    } else {
        old_cmdline = p->cmdline;
        if (!old_cmdline)
            old_cmdline = "";
        new_cmdline = static_cast<char *>(malloc(strlen(old_cmdline) + 1 + strlen(cmdline) + 1));
        strcpy(new_cmdline, old_cmdline);
        strcat(new_cmdline, " ");
        strcat(new_cmdline, cmdline);
    }
    free(p->cmdline);
    p->cmdline = new_cmdline;
}

void virt_machine_free_config(VirtMachineParams *p)
{
    int i;

    free(p->machine_name);
    free(p->cmdline);
    for(i = 0; i < VM_FILE_COUNT; i++) {
        free(p->files[i].filename);
        free(p->files[i].buf);
    }
    free_device_list(p->root_devices);
    p->root_devices = NULL;
    free(p->root_bus_type);
    p->root_bus_type = NULL;
    json_free(p->cfg_json);
    p->cfg_json = json_undefined_new();
    free(p->display_device);
    free(p->cfg_filename);
}

VirtMachine *virt_machine_init(VirtMachineParams *p)
{
    return p->vmc->Init(p);
}

void virt_machine_set_defaults(VirtMachineParams *p)
{
    memset(p, 0, sizeof(*p));
    /* a zeroed JSONValue is a JSON_STR with a null payload, not "nothing" */
    p->cfg_json = json_undefined_new();
}
