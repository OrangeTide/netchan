/* microser.h : tagged message serialisation with no heap and no dependencies */
/* PUBLIC DOMAIN (CC0-1.0) */

/*
 * A field is a tag byte followed by its value.  The tag packs a field number
 * and a wire type, which is what lets a reader skip a field it has never
 * heard of and carry on:
 *
 *   tag = (field << 3) | wire
 *
 *   wire 0   1 byte      uint8, int8
 *   wire 1   2 bytes     uint16, int16, little-endian
 *   wire 2   4 bytes     uint32, int32, little-endian
 *   wire 3   8 bytes     uint64, int64, little-endian
 *   wire 4   uint16le length, then that many bytes    bytes, string
 *
 * Field numbers run from 1 to 31, since the tag byte spends three bits on
 * the wire type.  A message may therefore carry 31 distinct fields, and
 * adding a new one to a later version of a message costs an older reader
 * nothing: it skips what it does not recognise.
 *
 * Every function takes an explicit buffer and length and returns the new
 * position, or -1 when the buffer is too small.  Nothing here allocates,
 * and a decoded bytes or string field points into the caller's buffer
 * rather than a copy of it, so it stays valid only as long as that buffer.
 */

#ifndef MICROSER_H
#define MICROSER_H

#include <stdint.h>
#include <string.h>

#define MS_WIRE_8     0
#define MS_WIRE_16    1
#define MS_WIRE_32    2
#define MS_WIRE_64    3
#define MS_WIRE_BYTES 4

/****************************************************************
 * Writing: each returns the new position, or -1 if it would overrun
 ****************************************************************/

static inline int
ms_write_tag_u8(uint8_t *buf, int pos, int len, uint8_t field, uint8_t val)
{
    if (pos + 2 > len)
        return -1;
    buf[pos] = (uint8_t)((field << 3) | MS_WIRE_8);
    buf[pos + 1] = val;
    return pos + 2;
}

static inline int
ms_write_tag_i8(uint8_t *buf, int pos, int len, uint8_t field, int8_t val)
{
    return ms_write_tag_u8(buf, pos, len, field, (uint8_t)val);
}

static inline int
ms_write_tag_u16(uint8_t *buf, int pos, int len, uint8_t field, uint16_t val)
{
    if (pos + 3 > len)
        return -1;
    buf[pos] = (uint8_t)((field << 3) | MS_WIRE_16);
    buf[pos + 1] = (uint8_t)(val & 0xff);
    buf[pos + 2] = (uint8_t)((val >> 8) & 0xff);
    return pos + 3;
}

static inline int
ms_write_tag_i16(uint8_t *buf, int pos, int len, uint8_t field, int16_t val)
{
    return ms_write_tag_u16(buf, pos, len, field, (uint16_t)val);
}

static inline int
ms_write_tag_u32(uint8_t *buf, int pos, int len, uint8_t field, uint32_t val)
{
    if (pos + 5 > len)
        return -1;
    buf[pos] = (uint8_t)((field << 3) | MS_WIRE_32);
    buf[pos + 1] = (uint8_t)(val & 0xff);
    buf[pos + 2] = (uint8_t)((val >> 8) & 0xff);
    buf[pos + 3] = (uint8_t)((val >> 16) & 0xff);
    buf[pos + 4] = (uint8_t)((val >> 24) & 0xff);
    return pos + 5;
}

static inline int
ms_write_tag_i32(uint8_t *buf, int pos, int len, uint8_t field, int32_t val)
{
    return ms_write_tag_u32(buf, pos, len, field, (uint32_t)val);
}

static inline int
ms_write_tag_u64(uint8_t *buf, int pos, int len, uint8_t field, uint64_t val)
{
    int i;

    if (pos + 9 > len)
        return -1;
    buf[pos] = (uint8_t)((field << 3) | MS_WIRE_64);
    for (i = 0; i < 8; i++)
        buf[pos + 1 + i] = (uint8_t)((val >> (i * 8)) & 0xff);
    return pos + 9;
}

static inline int
ms_write_tag_i64(uint8_t *buf, int pos, int len, uint8_t field, int64_t val)
{
    return ms_write_tag_u64(buf, pos, len, field, (uint64_t)val);
}

/* Write a length-prefixed field: tag byte, uint16le length, then the data. */
static inline int
ms_write_tag_bytes(uint8_t *buf, int pos, int len, uint8_t field,
                   const void *data, uint16_t dlen)
{
    if (pos + 3 + dlen > len)
        return -1;
    buf[pos] = (uint8_t)((field << 3) | MS_WIRE_BYTES);
    buf[pos + 1] = (uint8_t)(dlen & 0xff);
    buf[pos + 2] = (uint8_t)((dlen >> 8) & 0xff);
    if (dlen > 0)
        memcpy(buf + pos + 3, data, dlen);
    return pos + 3 + dlen;
}

/****************************************************************
 * Reading: each returns the new position, or -1 if it would run past end
 ****************************************************************/

static inline int
ms_read_u8(const uint8_t *buf, int pos, int end, uint8_t *val)
{
    if (pos + 1 > end)
        return -1;
    *val = buf[pos];
    return pos + 1;
}

static inline int
ms_read_i8(const uint8_t *buf, int pos, int end, int8_t *val)
{
    uint8_t tmp;
    int ret;

    ret = ms_read_u8(buf, pos, end, &tmp);
    if (ret >= 0)
        *val = (int8_t)tmp;
    return ret;
}

static inline int
ms_read_u16(const uint8_t *buf, int pos, int end, uint16_t *val)
{
    if (pos + 2 > end)
        return -1;
    *val = (uint16_t)((uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8));
    return pos + 2;
}

static inline int
ms_read_i16(const uint8_t *buf, int pos, int end, int16_t *val)
{
    uint16_t tmp;
    int ret;

    ret = ms_read_u16(buf, pos, end, &tmp);
    if (ret >= 0)
        *val = (int16_t)tmp;
    return ret;
}

static inline int
ms_read_u32(const uint8_t *buf, int pos, int end, uint32_t *val)
{
    if (pos + 4 > end)
        return -1;
    *val = (uint32_t)buf[pos]
         | ((uint32_t)buf[pos + 1] << 8)
         | ((uint32_t)buf[pos + 2] << 16)
         | ((uint32_t)buf[pos + 3] << 24);
    return pos + 4;
}

static inline int
ms_read_i32(const uint8_t *buf, int pos, int end, int32_t *val)
{
    uint32_t tmp;
    int ret;

    ret = ms_read_u32(buf, pos, end, &tmp);
    if (ret >= 0)
        *val = (int32_t)tmp;
    return ret;
}

static inline int
ms_read_u64(const uint8_t *buf, int pos, int end, uint64_t *val)
{
    uint64_t v = 0;
    int i;

    if (pos + 8 > end)
        return -1;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)buf[pos + i] << (i * 8);
    *val = v;
    return pos + 8;
}

static inline int
ms_read_i64(const uint8_t *buf, int pos, int end, int64_t *val)
{
    uint64_t tmp;
    int ret;

    ret = ms_read_u64(buf, pos, end, &tmp);
    if (ret >= 0)
        *val = (int64_t)tmp;
    return ret;
}

/*
 * Read a length-prefixed field.  The data pointer is into buf rather than a
 * copy, so it lives exactly as long as buf does.  Fails if the field runs
 * past end or is longer than dmax.
 */
static inline int
ms_read_bytes(const uint8_t *buf, int pos, int end,
              const uint8_t **data, uint16_t dmax, uint16_t *out_len)
{
    uint16_t dlen;

    if (pos + 2 > end)
        return -1;
    dlen = (uint16_t)((uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8));
    pos += 2;
    if (pos + dlen > end || dlen > dmax)
        return -1;
    *data = buf + pos;
    *out_len = dlen;
    return pos + dlen;
}

/*
 * Step over a field this reader does not know about.  This is what makes a
 * message forward compatible: the wire type in the tag says how big the
 * value is even when the field number means nothing here.
 */
static inline int
ms_skip(const uint8_t *buf, int pos, int end, uint8_t wire)
{
    static const int sizes[] = { 1, 2, 4, 8 };
    uint16_t blen;

    if (wire <= MS_WIRE_64) {
        pos += sizes[wire];
        return pos <= end ? pos : -1;
    }
    if (wire == MS_WIRE_BYTES) {
        if (pos + 2 > end)
            return -1;
        blen = (uint16_t)((uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8));
        pos += 2 + blen;
        return pos <= end ? pos : -1;
    }
    return -1;
}

#endif /* MICROSER_H */
