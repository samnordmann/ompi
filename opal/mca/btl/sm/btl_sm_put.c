/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010-2014 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2019      Google, Inc. All rights reserved.
 * Copyright (c) 2020      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "opal_config.h"

#include "btl_sm.h"
#include "btl_sm_frag.h"
#include "btl_sm_endpoint.h"
#include "btl_sm_xpmem.h"

#if OPAL_BTL_SM_HAVE_CMA
#include <sys/uio.h>

#if OPAL_CMA_NEED_SYSCALL_DEFS
#include "opal/sys/cma.h"
#endif /* OPAL_CMA_NEED_SYSCALL_DEFS */

#endif

/**
 * Initiate an synchronous put.
 *
 * @param btl (IN)         BTL module
 * @param endpoint (IN)    BTL addressing information
 * @param descriptor (IN)  Description of the data to be transferred
 */
#if OPAL_BTL_SM_HAVE_XPMEM
int mca_btl_sm_put_xpmem (mca_btl_base_module_t *btl, mca_btl_base_endpoint_t *endpoint, void *local_address,
                             uint64_t remote_address, mca_btl_base_registration_handle_t *local_handle,
                             mca_btl_base_registration_handle_t *remote_handle, size_t size, int flags,
                             int order, mca_btl_base_rdma_completion_fn_t cbfunc, void *cbcontext, void *cbdata)
{
    mca_rcache_base_registration_t *reg;
    void *rem_ptr;

    reg = sm_get_registation (endpoint, (void *)(intptr_t) remote_address, size, 0, &rem_ptr);
    if (OPAL_UNLIKELY(NULL == reg)) {
        return OPAL_ERROR;
    }

    sm_memmove (rem_ptr, local_address, size);

    sm_return_registration (reg, endpoint);

    /* always call the callback function */
    cbfunc (btl, endpoint, local_address, local_handle, cbcontext, cbdata, OPAL_SUCCESS);

    return OPAL_SUCCESS;
}
#endif

#if OPAL_BTL_SM_HAVE_CMA
int mca_btl_sm_put_cma (mca_btl_base_module_t *btl, mca_btl_base_endpoint_t *endpoint, void *local_address,
                           uint64_t remote_address, mca_btl_base_registration_handle_t *local_handle,
                           mca_btl_base_registration_handle_t *remote_handle, size_t size, int flags,
                           int order, mca_btl_base_rdma_completion_fn_t cbfunc, void *cbcontext, void *cbdata)
{
    struct iovec src_iov = {.iov_base = local_address, .iov_len = size};
    struct iovec dst_iov = {.iov_base = (void *)(intptr_t) remote_address, .iov_len = size};
    ssize_t ret;

    /* This should not be needed, see the rationale in mca_btl_sm_get_cma() */
    do {
        ret = process_vm_writev (endpoint->segment_data.other.seg_ds->seg_cpid, &src_iov, 1, &dst_iov, 1, 0);
        if (0 > ret) {
            if (ESRCH == errno) {
                BTL_PEER_ERROR(NULL,
                        ("CMA wrote %ld, expected %lu, errno = %d\n", (long)ret, (unsigned long)size, errno));
                return OPAL_ERROR;
            }
            BTL_ERROR(("CMA wrote %ld, expected %lu, errno = %d\n", (long)ret, (unsigned long)size, errno));
            return OPAL_ERROR;
        }
        src_iov.iov_base = (void *)((char *)src_iov.iov_base + ret);
        src_iov.iov_len -= ret;
        dst_iov.iov_base = (void *)((char *)dst_iov.iov_base + ret);
        dst_iov.iov_len -= ret;
    } while (0 < src_iov.iov_len);

    /* always call the callback function */
    cbfunc (btl, endpoint, local_address, local_handle, cbcontext, cbdata, OPAL_SUCCESS);

    return OPAL_SUCCESS;
}
#endif

#if OPAL_BTL_SM_HAVE_KNEM
int mca_btl_sm_put_knem (mca_btl_base_module_t *btl, mca_btl_base_endpoint_t *endpoint, void *local_address,
                            uint64_t remote_address, mca_btl_base_registration_handle_t *local_handle,
                            mca_btl_base_registration_handle_t *remote_handle, size_t size, int flags,
                            int order, mca_btl_base_rdma_completion_fn_t cbfunc, void *cbcontext, void *cbdata)
{
    struct knem_cmd_param_iovec send_iovec;
    struct knem_cmd_inline_copy icopy;

    /* Fill in the ioctl data fields.  There's no async completion, so
       we don't need to worry about getting a slot, etc. */
    send_iovec.base = (uintptr_t) local_address;
    send_iovec.len = size;
    icopy.local_iovec_array = (uintptr_t) &send_iovec;
    icopy.local_iovec_nr    = 1;
    icopy.remote_cookie     = remote_handle->cookie;
    icopy.remote_offset     = remote_address - remote_handle->base_addr;
    icopy.write             = 1;
    icopy.flags             = 0;

    /* Use the DMA flag if knem supports it *and* the segment length
     * is greater than the cutoff. Not that if DMA is not supported
     * or the user specified 0 for knem_dma_min the knem_dma_min was
     * set to UINT_MAX in mca_btl_sm_knem_init. */
    if (mca_btl_sm_component.knem_dma_min <= size) {
        icopy.flags = KNEM_FLAG_DMA;
    }
    /* synchronous flags only, no need to specify icopy.async_status_index */

    /* When the ioctl returns, the transfer is done and we can invoke
       the btl callback and return the frag */
    if (OPAL_UNLIKELY(0 != ioctl (mca_btl_sm.knem_fd, KNEM_CMD_INLINE_COPY, &icopy))) {
        return OPAL_ERROR;
    }

    if (KNEM_STATUS_FAILED == icopy.current_status) {
        return OPAL_ERROR;
    }

    /* always call the callback function */
    cbfunc (btl, endpoint, local_address, local_handle, cbcontext, cbdata, OPAL_SUCCESS);

    return OPAL_SUCCESS;
}
#endif