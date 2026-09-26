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

#include "nx/ipc/es.h"

#include <string.h>

#include <switch.h>

static Service g_esSrv;

Result esInitialize(void) {
    return smGetService(&g_esSrv, "es");
}

void esExit(void) {
    serviceClose(&g_esSrv);
}

Result esImportTicket(void const *tikBuf, size_t tikSize, void const *certBuf, size_t certSize) {
    return serviceDispatch(&g_esSrv, 1,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
        },
        .buffers = {
            { tikBuf,   tikSize },
            { certBuf,  certSize },
        },
    );
}

Result esDeleteTicket(const EsRightsId *rights_id) {
    return serviceDispatch(&g_esSrv, 3,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { rights_id, sizeof(*rights_id) } },
    );
}

Result esCountCommonTicket(u32 *out_count) {
    return serviceDispatchOut(&g_esSrv, 9, *out_count);
}

Result esCountPersonalizedTicket(u32 *out_count) {
    return serviceDispatchOut(&g_esSrv, 10, *out_count);
}

Result esListCommonTicket(u32 *out_written, EsRightsId *out_rights_id_buf, size_t out_right_id_buf_size) {
    return serviceDispatchOut(&g_esSrv, 11, *out_written,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out_rights_id_buf, out_right_id_buf_size } },
    );
}

Result esListPersonalizedTicket(u32 *out_written, EsRightsId *out_rights_id_buf, size_t out_right_id_buf_size) {
    return serviceDispatchOut(&g_esSrv, 12, *out_written,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out_rights_id_buf, out_right_id_buf_size } },
    );
}
