/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/vfs.h>
#include <rpc/types.h>
#include <rpc/xdr.h>
#include <nfs/nfs4.h>
#include <nfs/nfs4_kprot.h>
#include <nfs/ds_prot.h>
#include <nfs/ds_filehandle.h>

#include <sys/sdt.h>

bool_t
xdr_mds_ppid_content(XDR *xdrs, mds_ppid_content *objp)
{
	if (!xdr_uint64_t(xdrs, &objp->id))
		return (FALSE);
	if (!xdr_uint32_t(xdrs, &objp->aun))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_mds_ppid(XDR *xdrs, mds_ppid *objp)
{
	if (!xdr_bytes(xdrs, (char **)&objp->mds_ppid_val,
	    (uint_t *)&objp->mds_ppid_len, ~0))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_ds_fh_v1(XDR *xdrs, ds_fh_v1 *objp)
{
	char *ptr;

	if (!xdr_uint32_t(xdrs, &objp->flags))
		return (FALSE);
	if (!xdr_uint32_t(xdrs, &objp->gen))
		return (FALSE);
	if (!xdr_uint64_t(xdrs, &objp->mds_id))
		return (FALSE);
	if (!xdr_mds_ppid(xdrs, &objp->mds_ppid))
		return (FALSE);
	if (!xdr_uint64_t(xdrs, &objp->mds_dataset_id))
		return (FALSE);
	if (!xdr_uint64_t(xdrs, &objp->fsid.major))
		return (FALSE);
	if (!xdr_uint64_t(xdrs, &objp->fsid.minor))
		return (FALSE);

	ptr = &objp->mds_fid.mds_fid_val[0];
	if (!xdr_bytes(xdrs, &ptr,
	    (uint_t *)&objp->mds_fid.mds_fid_len, DS_MAXFIDSZ))
		return (FALSE);

	return (TRUE);
}

bool_t
xdr_ds_fh(XDR *xdrs, mds_ds_fh *objp)
{
	if (!xdr_enum(xdrs, (enum_t *)&objp->vers))
		return (FALSE);

	switch (objp->vers) {
	case DS_FH_v1:
		if (!xdr_ds_fh_v1(xdrs, &objp->fh.v1))
			return (FALSE);
		break;
	default:
		DTRACE_PROBE(xdr__e__unsuported_fh_vers);
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_ds_fh_fmt(XDR *xdrs, mds_ds_fh *objp)
{
	if (!xdr_enum(xdrs, (enum_t *)&objp->type))
		return (FALSE);

	switch (objp->type) {
	case FH41_TYPE_DMU_DS:
		if (!xdr_ds_fh(xdrs, objp))
			return (FALSE);
		break;
	default:
		DTRACE_PROBE(xdr__e__unsuported_fh_type);
		return (FALSE);
	}
	return (TRUE);
}

bool_t
xdr_encode_ds_fh(mds_ds_fh *fhp, nfs_fh4 *objp)
{
	XDR xdr;
	char *xdr_ptr;
	uint_t otw_len;

	objp->nfs_fh4_val = NULL;
	objp->nfs_fh4_len = 0;

	otw_len = xdr_sizeof(xdr_ds_fh_fmt, fhp);

	if (otw_len == 0)
		return (FALSE);

	objp->nfs_fh4_val = xdr_ptr = kmem_zalloc(otw_len, KM_SLEEP);
	objp->nfs_fh4_len = otw_len;
	xdrmem_create(&xdr, xdr_ptr, otw_len, XDR_ENCODE);

	return (xdr_ds_fh_fmt(&xdr, fhp));
}
