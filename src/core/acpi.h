/*
 * ACPI Machine Language encoding
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

#include <stddef.h>
#include <stdint.h>

#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

typedef std::vector<uint8_t> AmlBytes;


/* An ECAM window as the MCFG lists it: the configuration space of buses 0
   to 'bus_last' of PCI segment 'segment'. */
struct AcpiEcam {
    uint64_t base;
    uint16_t segment;
    uint8_t bus_last;
};


/* Carried down the tree while the DSDT is built, as FDTContext is while a
   device tree is. A device appends its Device() to 'aml'; one whose children
   are described too builds them in a context of its own, from Nested(). */
class AcpiContext {
private:
    /* the full paths handed out so far, and the _UIDs by _HID */
    std::shared_ptr<std::set<std::string>> fNames;
    std::shared_ptr<std::map<std::string, int>> fUids;
    std::shared_ptr<std::vector<AcpiEcam>> fEcams;

public:
    AmlBytes *aml;
    /* this scope's path, as a resource source names it: "\\_SB" */
    std::string scope;

    AcpiContext(AmlBytes *aml, const char *scope);

    /* A name of its own in this scope: 'prefix' with a digit after it when
       it is three characters, or else 'prefix' itself. */
    std::string NewName(const char *prefix);

    /* A _UID no other device of this _HID has. */
    int NewUid(const char *hid);

    /* Add the descriptor of the interrupt line a resource was assigned,
       level triggered and active high. Kept in one place so that every
       device describes the line it got; on the PC a line is its GSI. */
    void AddInterrupt(AmlBytes &r, uint64_t line) const;

    /* Have the MCFG list the ECAM window of 'bus_count' buses at 'base',
       as a PCI segment of its own, whose number is returned. */
    int AddEcam(uint64_t base, int bus_count);
    const std::vector<AcpiEcam> &Ecams() const {return *fEcams;}

    /* The context of the devices inside device 'name' of this scope. */
    AcpiContext Nested(const std::string &name, AmlBytes *aml) const;
};


/* Little endian values, appended. */
void aml_put8(AmlBytes &b, uint8_t v);
void aml_put16(AmlBytes &b, uint16_t v);
void aml_put32(AmlBytes &b, uint32_t v);
void aml_put64(AmlBytes &b, uint64_t v);
void aml_put_str(AmlBytes &b, const char *s, size_t len);
void aml_append(AmlBytes &b, const AmlBytes &more);


/* Terms. A name is one four character segment. */
AmlBytes aml_with_length(std::initializer_list<uint8_t> op,
                         const AmlBytes &body);
AmlBytes aml_int(uint32_t v);
AmlBytes aml_string(const char *s);
AmlBytes aml_name(const char *seg, const AmlBytes &value);
AmlBytes aml_package(const std::vector<AmlBytes> &elements);
AmlBytes aml_buffer(const AmlBytes &data);
AmlBytes aml_device(const char *seg, const AmlBytes &contents);
/* A scope given by its path as AML spells it, such as "\\_SB_". */
AmlBytes aml_scope(const char *path, const AmlBytes &contents);
/* EISAID("PNP0A03") */
AmlBytes aml_eisaid(const char *id);
/* ToUUID("3cdff6f7-4267-4555-ad05-b30a3d8938de") */
AmlBytes aml_uuid(const char *uuid);

/* Control methods and the little of their bodies the devices need. */
AmlBytes aml_method(const char *seg, int arg_count, const AmlBytes &body);
AmlBytes aml_if(const AmlBytes &predicate, const AmlBytes &body);
AmlBytes aml_lequal(const AmlBytes &a, const AmlBytes &b);
AmlBytes aml_return(const AmlBytes &value);
AmlBytes aml_arg(int index);


/* Resource templates: the descriptors are appended to 'r', which
   aml_resource_template() then ends and wraps in a buffer. */
void aml_res_io(AmlBytes &r, uint16_t port, uint8_t len);
/* An ISA line, edge triggered and active high. */
void aml_res_irq(AmlBytes &r, int irq);
/* An extended interrupt descriptor of one GSI. */
void aml_res_interrupt(AmlBytes &r, uint32_t gsi, bool level,
                       bool active_low);
void aml_res_memory32_fixed(AmlBytes &r, uint32_t base, uint32_t size);
/* I2cSerialBusV2: a 7 bit target of the controller at path 'source'. */
void aml_res_i2c_serial_bus(AmlBytes &r, uint16_t address, uint32_t speed,
                            const char *source);
/* Address space descriptors that are fixed at both ends; 'type' is 0 for
   memory, 1 for I/O and 2 for bus numbers. */
void aml_res_word_space(AmlBytes &r, int type, int type_flags, uint16_t min,
                        uint16_t max);
void aml_res_dword_memory(AmlBytes &r, uint32_t min, uint32_t max);
void aml_res_qword_memory(AmlBytes &r, uint64_t min, uint64_t max);
AmlBytes aml_resource_template(AmlBytes r);
