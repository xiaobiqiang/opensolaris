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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Processing of SHF_ORDERED sections.
 */
#include	<stdio.h>
#include	<fcntl.h>
#include	<link.h>
#include	<debug.h>
#include	"msg.h"
#include	"_libld.h"

/*
 * Part 1, Input processing.
 */
/*
 * Get the head section number
 */
inline static Word
is_keylink_ok(Ifl_desc *ifl, Word keylink, Word limit)
{
	if ((keylink == SHN_BEFORE) || (keylink == SHN_AFTER))
		return (0);

	/*
	 * Validate the key range.
	 */
	if ((keylink == 0) || (keylink >= limit))
		return (DBG_ORDER_LINK_OUTRANGE);

	/*
	 * The section pointed to by keylink should not be an ordered section.
	 */
	if (ifl->ifl_isdesc[keylink]->is_shdr->sh_flags & ALL_SHF_ORDER)
		return (DBG_ORDER_INFO_ORDER);

	return (0);
}

/*
 * Get the head section number.
 */
static Word
get_shfordered_dest(Ofl_desc *ofl, Ifl_desc *ifl, Word ndx, Word limit)
{
	Word	t1_link = ndx, t2_link, ret_link;
	Is_desc *isp, *isp1, *isp2;
	int	error = 0;

	/*
	 * Check the sh_info of myself.
	 */
	isp = ifl->ifl_isdesc[ndx];

	isp1 = isp;
	ret_link = t2_link = isp1->is_shdr->sh_link;
	t1_link = ndx;
	do {
		/*
		 * Check the validitiy of the link
		 */
		if (t2_link == 0 || t2_link >= limit) {
			error = DBG_ORDER_LINK_OUTRANGE;
			break;
		}
		isp2 = ifl->ifl_isdesc[t2_link];

		/*
		 * Pointing to a bad ordered section ?
		 */
		if ((isp2->is_flags & FLG_IS_ORDERED) == 0) {
			error = DBG_ORDER_LINK_ERROR;
			break;
		}

		/*
		 * Check sh_flag
		 */
		if (isp1->is_shdr->sh_flags != isp2->is_shdr->sh_flags) {
			error = DBG_ORDER_FLAGS;
			break;
		}

		/*
		 * Check the validity of sh_info field.
		 */
		if ((error = is_keylink_ok(ifl,
		    isp->is_shdr->sh_info, limit)) != 0) {
			break;
		}

		/*
		 * Can I break ?
		 */
		if (t1_link == t2_link)
			break;

		/*
		 * Get the next link
		 */
		t1_link = t2_link;
		isp1 = ifl->ifl_isdesc[t1_link];
		ret_link = t2_link = isp1->is_shdr->sh_link;

		/*
		 * Cyclic ?
		 */
		if (t2_link == ndx) {
			error = DBG_ORDER_CYCLIC;
			break;
		}
	/* CONSTANTCONDITION */
	} while (1);

	if (error != 0) {
		ret_link = 0;
		DBG_CALL(Dbg_sec_order_error(ofl->ofl_lml, ifl, ndx, error));
	}
	return (ret_link);
}

/*
 * Called from process_elf().
 * This routine does the input processing of the ordered sections.
 */
uintptr_t
ld_process_ordered(Ifl_desc *ifl, Ofl_desc *ofl, Word ndx)
{
	Is_desc		*isp2, *isp = ifl->ifl_isdesc[ndx];
	Xword		shflags = isp->is_shdr->sh_flags;
	Word		keylink, dest_ndx, limit = ifl->ifl_shnum;
	Os_desc		*osp2, *osp;
	Sort_desc	*st;
	Aliste		idx;
	int		error = 0;

	/*
	 * This section might have been checked and marked in error already.
	 */
	if ((isp->is_flags & FLG_IS_ORDERED) == 0)
		return (0);

	if (shflags & SHF_ORDERED)
		keylink = isp->is_shdr->sh_info;
	else if (shflags & SHF_LINK_ORDER)
		keylink = isp->is_shdr->sh_link;
	else
		keylink = 0;

	if ((error = is_keylink_ok(ifl, keylink, limit)) != 0) {
		DBG_CALL(Dbg_sec_order_error(ofl->ofl_lml, ifl, ndx, error));
		isp->is_flags &= ~FLG_IS_ORDERED;
		if (isp->is_osdesc == NULL) {
			return ((uintptr_t)ld_place_section(ofl, isp,
			    isp->is_keyident, 0));
		}
		return ((uintptr_t)isp->is_osdesc);
	}

	/*
	 * If SHF_ORDERED is in effect, search for our destination section based
	 * off of sh_link, otherwise follow the default rules for the
	 * destination section.
	 */
	if (shflags & SHF_ORDERED) {
		if ((dest_ndx = get_shfordered_dest(ofl, ifl,
		    ndx, limit)) == 0) {
			isp->is_flags &= ~FLG_IS_ORDERED;
			if (isp->is_osdesc == NULL) {
				return ((uintptr_t)ld_place_section(ofl, isp,
				    isp->is_keyident, 0));
			}
			return ((uintptr_t)isp->is_osdesc);
		}
	} else {
		/*
		 * SHF_LINK_ORDER coalesces into default sections, set dest_ndx
		 * to NULL to trigger this.
		 */
		dest_ndx = 0;
	}

	/*
	 * Place the section into its output section. It's possible that this
	 * section is discarded (possibly because it's defined COMDAT), in
	 * which case we're done.
	 */
	if ((osp = isp->is_osdesc) == NULL) {
		if ((osp = ld_place_section(ofl, isp, isp->is_keyident,
		    dest_ndx)) == (Os_desc *)S_ERROR)
			return ((uintptr_t)S_ERROR);
		if (!osp)
			return (0);
	}

	/*
	 * If the output section is not yet on the ordered list, place it on
	 * the list.
	 */
	osp2 = NULL;
	for (APLIST_TRAVERSE(ofl->ofl_ordered, idx, osp2)) {
		if (osp2 == osp)
			break;
	}

	if ((osp != osp2) && (aplist_append(&(ofl->ofl_ordered),
	    osp, AL_CNT_OFL_ORDERED) == NULL))
		return ((uintptr_t)S_ERROR);

	/*
	 * Output section has been found - set up it's sorting information.
	 */
	if ((osp->os_sort == NULL) &&
	    ((osp->os_sort = libld_calloc(1, sizeof (Sort_desc))) == NULL))
		return (S_ERROR);

	st = osp->os_sort;

	if (keylink == SHN_BEFORE) {
		st->st_beforecnt++;
	} else if (keylink == SHN_AFTER) {
		st->st_aftercnt++;
	} else {
		st->st_ordercnt++;
		isp2 = ifl->ifl_isdesc[keylink];
		if (isp2->is_flags & FLG_IS_DISCARD) {
			eprintf(ofl->ofl_lml, ERR_FATAL,
			    MSG_INTL(MSG_FIL_BADORDREF), ifl->ifl_name,
			    isp->is_name, isp->is_scnndx, isp2->is_name,
			    isp2->is_scnndx);
			return (S_ERROR);
		}

		/*
		 * Indicate that this ordered input section will require a sort
		 * key created.  Propagate the key requirement through to the
		 * associated output section, segment and file, to trigger the
		 * sort key creation.  See ld_sec_validate();
		 */
		isp2->is_flags |= FLG_IS_KEY;

		osp2 = isp2->is_osdesc;
		osp2->os_flags |= FLG_OS_KEY;
		osp2->os_sgdesc->sg_flags |= FLG_SG_KEY;

		ofl->ofl_flags |= FLG_OF_KEY;
	}

	return ((uintptr_t)osp);
}

/*
 * Part 2, Sorting processing
 */

/*
 * Traverse all segments looking for section ordering information that hasn't
 * been used.  If found give a warning message to the user.  Also, check if
 * there are any SHF_ORDERED key sections, and if so set up sort key values.
 */
void
ld_sec_validate(Ofl_desc *ofl)
{
	Aliste		idx1;
	Sg_desc		*sgp;
	Word 		key = 1;

	for (APLIST_TRAVERSE(ofl->ofl_segs, idx1, sgp)) {
		Sec_order	*scop;
		Os_desc		*osp;
		Aliste		idx2;

		for (APLIST_TRAVERSE(sgp->sg_secorder, idx2, scop)) {
			if ((scop->sco_flags & FLG_SGO_USED) == 0) {
				eprintf(ofl->ofl_lml, ERR_WARNING,
				    MSG_INTL(MSG_MAP_SECORDER),
				    sgp->sg_name, scop->sco_secname);
			}
		}
		if ((sgp->sg_flags & FLG_SG_KEY) == 0)
			continue;

		for (APLIST_TRAVERSE(sgp->sg_osdescs, idx2, osp)) {
			Aliste	idx3;
			Is_desc	*isp;

			if ((osp->os_flags & FLG_OS_KEY) == 0)
				continue;

			for (APLIST_TRAVERSE(osp->os_isdescs, idx3, isp)) {
				if (isp->is_flags & FLG_IS_KEY)
					isp->is_keyident = key++;
			}
		}
	}
}

static int
setup_sortbuf(Os_desc *osp)
{
	Sort_desc	*st = osp->os_sort;
	Word		num_after = 0, num_before = 0, num_order = 0;
	Aliste		idx;
	Is_desc		*isp;

	if ((st == NULL) ||
	    ((st->st_ordercnt + st->st_beforecnt + st->st_aftercnt) == 0))
		return (0);

	/*
	 * Get memory
	 */
	if (st->st_beforecnt && ((st->st_before = libld_calloc(st->st_beforecnt,
	    sizeof (Is_desc *))) == NULL))
		return (0);

	if (st->st_ordercnt && ((st->st_order = libld_calloc(st->st_ordercnt,
	    sizeof (Is_desc *))) == NULL))
		return (0);

	if (st->st_aftercnt && ((st->st_after = libld_calloc(st->st_aftercnt,
	    sizeof (Is_desc *))) == NULL))
		return (0);

	/*
	 * Set info.
	 */
	for (APLIST_TRAVERSE(osp->os_isdescs, idx, isp)) {
		Word	keylink = 0;

		if ((isp->is_flags & FLG_IS_ORDERED) == 0)
			continue;

		if (isp->is_shdr->sh_flags & SHF_ORDERED)
			keylink = isp->is_shdr->sh_info;
		else if (isp->is_shdr->sh_flags & SHF_LINK_ORDER)
			keylink = isp->is_shdr->sh_link;

		if (keylink == SHN_BEFORE)
			st->st_before[num_before++] = isp;
		else if (keylink == SHN_AFTER)
			st->st_after[num_after++] = isp;
		else
			st->st_order[num_order++] = isp;
	}
	return (1);
}

static int
comp(const void *ss1, const void *ss2)
{
	Is_desc		*s1 = *((Is_desc **)ss1);
	Is_desc		*s2 = *((Is_desc **)ss2);
	Is_desc		*i1, *i2;
	Word		ndx1, ndx2;

	if (s1->is_shdr->sh_flags & SHF_ORDERED)
		ndx1 = s1->is_shdr->sh_info;
	else
		ndx1 = s1->is_shdr->sh_link;

	if (s2->is_shdr->sh_flags & SHF_ORDERED)
		ndx2 = s2->is_shdr->sh_info;
	else
		ndx2 = s2->is_shdr->sh_link;

	i1 = s1->is_file->ifl_isdesc[ndx1];
	i2 = s2->is_file->ifl_isdesc[ndx2];

	if (i1->is_keyident > i2->is_keyident)
		return (1);
	if (i1->is_keyident < i2->is_keyident)
		return (-1);
	return (0);
}

uintptr_t
ld_sort_ordered(Ofl_desc *ofl)
{
	Aliste	idx1;
	Os_desc *osp;

	DBG_CALL(Dbg_sec_order_list(ofl, 0));

	/*
	 * Sort Sections
	 */
	for (APLIST_TRAVERSE(ofl->ofl_ordered, idx1, osp)) {
		int		i;
		APlist		*islist = NULL;
		Aliste		idx2;
		Is_desc		*isp;
		Sort_desc	*st = osp->os_sort;

		if (setup_sortbuf(osp) == 0)
			return (S_ERROR);

		islist = osp->os_isdescs;
		osp->os_isdescs = NULL;

		/*
		 * Sorting.
		 * First Sort the ordered sections.
		 */
		if (st->st_ordercnt != 0)
			qsort((char *)st->st_order, st->st_ordercnt,
			    sizeof (Is_desc *), comp);

		/*
		 * Place SHN_BEFORE at head of list
		 */
		for (i = 0; i < st->st_beforecnt; i++) {
			if (ld_append_isp(ofl, osp, st->st_before[i], 0) == 0)
				return (S_ERROR);
		}

		/*
		 * Next come 'linked' ordered sections
		 */
		for (i = 0; i < st->st_ordercnt; i++) {
			if (ld_append_isp(ofl, osp, st->st_order[i], 0) == 0)
				return (S_ERROR);
		}

		/*
		 * Now we list any sections which have no sorting
		 * specifications - in the order they were input.
		 *
		 * We use aplist_append() here instead of ld_append_isp(),
		 * because these items have already been inserted once, and
		 * we don't want any duplicate entries in osp->os_mstridescs.
		 */
		for (APLIST_TRAVERSE(islist, idx2, isp)) {
			if (isp->is_flags & FLG_IS_ORDERED)
				continue;
			if (aplist_append(&(osp->os_isdescs),
			    isp, AL_CNT_OS_ISDESCS) == NULL)
				return (S_ERROR);
		}

		/*
		 * And the end of the list are the SHN_AFTER sections.
		 */
		for (i = 0; i < st->st_aftercnt; i++) {
			if (ld_append_isp(ofl, osp, st->st_after[i], 0) == 0)
				return (S_ERROR);
		}

		if (islist)
			free(islist);
	}
	DBG_CALL(Dbg_sec_order_list(ofl, 1));
	return (0);
}
