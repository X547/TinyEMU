/*
 * Flattened Device Tree builder
 *
 * Copyright (c) 2016-2017 Fabrice Bellard
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

#include <stdint.h>


/* Accumulates the structure and string blocks, then serialises them into the
   flat blob. Node names carrying a unit address are built with BeginNodeNum()
   so that the address in the name always comes from the same value that went
   into "reg". */
class FDTBuilder {
private:
    uint32_t *fTab = nullptr;
    int fTabLen = 0;
    int fTabSize = 0;
    int fOpenNodeCount = 0;

    char *fStringTable = nullptr;
    int fStringTableLen = 0;
    int fStringTableSize = 0;

    uint32_t fNextPhandle = 1;

    void AllocLen(int len);
    void Put32(uint32_t v);
    void PutData(const uint8_t *data, int len);
    int StringOffset(const char *name);

public:
    FDTBuilder() = default;
    ~FDTBuilder();

    /* Phandles are handed out by the builder so that no two nodes can pick
       the same value. */
    uint32_t AllocPhandle() {return fNextPhandle++;}

    void BeginNode(const char *name);
    void BeginNodeNum(const char *name, uint64_t n);
    void EndNode();

    void Prop(const char *prop_name, const void *data, int data_len);
    void PropEmpty(const char *prop_name) {Prop(prop_name, nullptr, 0);}
    void PropTabU32(const char *prop_name, const uint32_t *tab, int tab_len);
    void PropU32(const char *prop_name, uint32_t val);
    void PropU64(const char *prop_name, uint64_t v0);
    void PropU64Range(const char *prop_name, uint64_t addr, uint64_t size);
    void PropStr(const char *prop_name, const char *str);
    /* nullptr terminated string list */
    void PropStrList(const char *prop_name, ...);

    /* Write the blob to 'dst'; returns its size in bytes. */
    int Output(uint8_t *dst);
};
