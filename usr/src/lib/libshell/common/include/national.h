/***********************************************************************
*                                                                      *
*               This software is part of the ast package               *
*          Copyright (c) 1982-2009 AT&T Intellectual Property          *
*                      and is licensed under the                       *
*                  Common Public License, Version 1.0                  *
*                    by AT&T Intellectual Property                     *
*                                                                      *
*                A copy of the License is available at                 *
*            http://www.opensource.org/licenses/cpl1.0.txt             *
*         (with md5 checksum 059e8cd6165cb4c31e351f2b69388fd9)         *
*                                                                      *
*              Information and Software Systems Research               *
*                            AT&T Research                             *
*                           Florham Park NJ                            *
*                                                                      *
*                  David Korn <dgk@research.att.com>                   *
*                                                                      *
***********************************************************************/
#pragma prototyped
/*
 *  national.h -  definitions for multibyte character sets
 *
 *   David Korn
 *   AT&T Labs
 *
 */

#if SHOPT_MULTIBYTE

#   ifndef MARKER
#	define MARKER		0xdfff	/* Must be invalid character */
#   endif

    extern int sh_strchr(const char*,const char*);

#endif /* SHOPT_MULTIBYTE */
