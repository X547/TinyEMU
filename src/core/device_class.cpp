/*
 * Device classes and the roster that finds them by name
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
#include "device_class.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"


//#pragma mark - DeviceConfig

JSONValue DeviceConfig::Get(const char *name) const
{
    return json_object_get(fProps, name);
}


const char *DeviceConfig::IdOr(const char *def) const
{
    JSONValue id = Get("id");
    return id.type == JSON_STR ? id.u.str->data : def;
}


bool DeviceConfig::GetInt(const char *name, int *pval) const
{
    return vm_get_int(fProps, name, pval) >= 0;
}


bool DeviceConfig::GetInt(const char *name, int *pval, int def_val) const
{
    return vm_get_int_opt(fProps, name, pval, def_val) >= 0;
}


bool DeviceConfig::GetStr(const char *name, const char **pstr) const
{
    return vm_get_str(fProps, name, pstr) >= 0;
}


bool DeviceConfig::GetStrOpt(const char *name, const char **pstr) const
{
    return vm_get_str_opt(fProps, name, pstr) >= 0;
}


char *DeviceConfig::ResolvePath(const char *filename) const
{
    return get_file_path(fBaseFile, filename);
}


bool DeviceConfig::HasChildren() const
{
    JSONValue bus = Get("bus");
    if (bus.type != JSON_OBJ) {
        return false;
    }
    JSONValue devices = json_object_get(bus, "devices");
    return devices.type == JSON_ARRAY && devices.u.array->Length() > 0;
}


//#pragma mark - DeviceClass

DeviceClass::DeviceClass(const char *name):
    fName(name)
{
    DeviceRoster::Default().Register(this);
}


//#pragma mark - DeviceRoster

DeviceRoster &DeviceRoster::Default()
{
    static DeviceRoster roster;
    return roster;
}


void DeviceRoster::Register(DeviceClass *cls)
{
    if (Find(cls->Name()) != nullptr) {
        fprintf(stderr, "device type '%s' is defined twice\n", cls->Name());
        abort();
    }
    cls->fNext = fFirst;
    fFirst = cls;
}


const DeviceClass *DeviceRoster::Find(const char *type) const
{
    for (const DeviceClass *cls = fFirst; cls != nullptr; cls = cls->fNext) {
        if (strcmp(cls->Name(), type) == 0) {
            return cls;
        }
    }
    return nullptr;
}
