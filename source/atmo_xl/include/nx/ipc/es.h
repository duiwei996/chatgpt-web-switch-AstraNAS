/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

#include <switch/types.h>
#include <switch/services/fs.h>

typedef union {
    struct {
        u64 application_id_be;
        u64 key_generation_be;
    } id;
    FsRightsId fs_id;
    u8 bytes[0x10];
} EsRightsId;

#ifdef __cplusplus
static_assert(sizeof(EsRightsId) == 0x10, "EsRightsId must be 16 bytes");
#else
_Static_assert(sizeof(EsRightsId) == 0x10, "EsRightsId must be 16 bytes");
#endif

Result esInitialize(void);
void esExit(void);

Result esImportTicket(void const *tikBuf, size_t tikSize, void const *certBuf, size_t certSize);
Result esDeleteTicket(const EsRightsId *rights_id);
Result esCountCommonTicket(u32 *out_count);
Result esCountPersonalizedTicket(u32 *out_count);
Result esListCommonTicket(u32 *out_written, EsRightsId *out_rights_id_buf, size_t out_right_id_buf_size);
Result esListPersonalizedTicket(u32 *out_written, EsRightsId *out_rights_id_buf, size_t out_right_id_buf_size);
