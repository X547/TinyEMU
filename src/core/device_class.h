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
#pragma once

#include "json.h"

class Device;
struct DeviceContext;


/* One device object of the configuration file. Only the device's class reads
   its properties; this merely gives it typed access to them. */
class DeviceConfig {
private:
    JSONValue fProps;
    const char *fType;
    /* the configuration file, which relative file names are taken from */
    const char *fBaseFile;

public:
    DeviceConfig(JSONValue props, const char *type, const char *base_file):
        fProps(props), fType(type), fBaseFile(base_file) {}

    const char *Type() const {return fType;}
    JSONValue Get(const char *name) const;
    const char *IdOr(const char *def) const;

    /* Report and return false when the property is missing (where it is
       required) or of the wrong kind. */
    bool GetInt(const char *name, int *pval) const;
    bool GetInt(const char *name, int *pval, int def_val) const;
    bool GetStr(const char *name, const char **pstr) const;
    /* nullptr when absent */
    bool GetStrOpt(const char *name, const char **pstr) const;

    /* 'filename' relative to the configuration file; free() the result. */
    char *ResolvePath(const char *filename) const;

    /* Whether the device declares a bus with at least one device on it. */
    bool HasChildren() const;
};


/* A kind of device the configuration can declare, under the name its "type"
   gives. Each class registers itself with the roster as it is constructed, so
   a class is defined as a static object next to the device it creates. */
class DeviceClass {
private:
    const char *fName;
    const DeviceClass *fNext = nullptr;

    friend class DeviceRoster;

public:
    explicit DeviceClass(const char *name);
    virtual ~DeviceClass() = default;

    const char *Name() const {return fName;}

    /* Reports and returns nullptr when the configuration asks for something
       the device cannot be. */
    virtual Device *Create(const DeviceConfig &cfg,
                           DeviceContext *ctx) const = 0;
};


class DeviceRoster {
private:
    const DeviceClass *fFirst = nullptr;

public:
    static DeviceRoster &Default();

    /* Two classes of one name are a build error, reported at startup. */
    void Register(DeviceClass *cls);
    const DeviceClass *Find(const char *type) const;
};
