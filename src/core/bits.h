/*
 * Bit fields of integers
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

#include <type_traits>
#include <utility>


/* A value's own type gives the width its fields are taken from, so a uint64_t
   operand yields a 64 bit result. Narrower types are promoted to unsigned,
   which keeps a shift out of the sign bit of an int. */
template <typename T>
using bits_type = std::make_unsigned_t<decltype(+std::declval<T>())>;

/* A bare mask has no value to take a width from, so those makers default to
   32 bits: bit_at<uint64_t>(63) for a wider one. */

/* The low 'len' bits, up to the whole width. */
template <typename T = uint32_t>
static constexpr T bit_mask(int len)
{
    return len >= (int)(8 * sizeof(T)) ? (T)~(T)0 : ((T)1 << len) - 1;
}

template <typename T = uint32_t>
static constexpr T bit_at(int pos)
{
    return (T)1 << pos;
}

/* 'len' bits at 'pos'. */
template <typename T = uint32_t>
static constexpr T field_mask(int pos, int len)
{
    return bit_mask<T>(len) << pos;
}

template <typename T>
static constexpr bits_type<T> get_bits(T val, int pos, int len)
{
    typedef bits_type<T> U;
    return ((U)val >> pos) & bit_mask<U>(len);
}

template <typename T>
static constexpr bool get_bit(T val, int pos)
{
    return ((bits_type<T>)val >> pos) & 1;
}

/* 'val' with the 'len' bits at 'pos' taken from the low bits of 'field'. */
template <typename T>
static constexpr bits_type<T> set_bits(T val, int pos, int len,
                                       bits_type<T> field)
{
    typedef bits_type<T> U;
    U mask = field_mask<U>(pos, len);
    return ((U)val & ~mask) | ((field << pos) & mask);
}

template <typename T>
static constexpr bits_type<T> set_bit(T val, int pos, bool on)
{
    return set_bits(val, pos, 1, (bits_type<T>)on);
}

/* 'high' placed above the low 'low_len' bits of 'low'. */
template <typename T = uint64_t>
static constexpr T concat_bits(std::type_identity_t<T> high,
                               std::type_identity_t<T> low, int low_len)
{
    return ((T)high << low_len) | get_bits((T)low, 0, low_len);
}

/* The low 'len' bits of 'val' as a signed value of the value's width. */
template <typename T>
static constexpr std::make_signed_t<bits_type<T>> sign_extend(T val, int len)
{
    typedef bits_type<T> U;
    typedef std::make_signed_t<U> S;
    int shift = (int)(8 * sizeof(U)) - len;
    return (S)((U)val << shift) >> shift;
}
