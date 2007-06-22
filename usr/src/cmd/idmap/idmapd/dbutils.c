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
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * Database related utility routines
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <rpc/rpc.h>
#include <sys/sid.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>

#include "idmapd.h"
#include "adutils.h"
#include "string.h"
#include "idmap_priv.h"

static idmap_retcode sql_compile_n_step_once(sqlite *, char *,
		sqlite_vm **, int *, int, const char ***);

#define	EMPTY_NAME(name)	(*name == 0 || strcmp(name, "\"\"") == 0)

#define	EMPTY_STRING(str)	(str == NULL || *str == 0)

#define	DO_NOT_ALLOC_NEW_ID_MAPPING(req)\
		(req->flag & IDMAP_REQ_FLG_NO_NEW_ID_ALLOC)

#define	AVOID_NAMESERVICE(req)\
		(req->flag & IDMAP_REQ_FLG_NO_NAMESERVICE)

#define	IS_EPHEMERAL(pid)	(pid > INT32_MAX)

#define	LOCALRID_MIN	1000

#define	SLEEP_TIME	20

#define	NANO_SLEEP(rqtp, nsec)\
	rqtp.tv_sec = 0;\
	rqtp.tv_nsec = nsec * (NANOSEC / MILLISEC);\
	(void) nanosleep(&rqtp, NULL);


typedef enum init_db_option {
	FAIL_IF_CORRUPT = 0,
	REMOVE_IF_CORRUPT = 1
} init_db_option_t;


/*
 * Initialize 'dbname' using 'sql'
 */
static int
init_db_instance(const char *dbname, const char *sql, init_db_option_t opt,
		int *new_db_created)
{
	int rc = 0;
	int tries = 0;
	sqlite *db = NULL;
	char *str = NULL;

	if (new_db_created != NULL)
		*new_db_created = 0;

	db = sqlite_open(dbname, 0600, &str);
	while (db == NULL) {
		idmapdlog(LOG_ERR,
		    "Error creating database %s (%s)",
		    dbname, CHECK_NULL(str));
		sqlite_freemem(str);
		if (opt == FAIL_IF_CORRUPT || opt != REMOVE_IF_CORRUPT ||
		    tries > 0)
			return (-1);

		tries++;
		(void) unlink(dbname);
		db = sqlite_open(dbname, 0600, &str);
	}

	sqlite_busy_timeout(db, 3000);
	rc = sqlite_exec(db, "BEGIN TRANSACTION;", NULL, NULL, &str);
	if (SQLITE_OK != rc) {
		idmapdlog(LOG_ERR, "Cannot begin database transaction (%s)",
		    str);
		sqlite_freemem(str);
		sqlite_close(db);
		return (1);
	}

	switch (sqlite_exec(db, sql, NULL, NULL, &str)) {
	case SQLITE_ERROR:
/*
 * This is the normal situation: CREATE probably failed because tables
 * already exist. It may indicate an error in SQL as well, but we cannot
 * tell.
 */
		sqlite_freemem(str);
		rc =  sqlite_exec(db, "ROLLBACK TRANSACTION",
		    NULL, NULL, &str);
		break;
	case SQLITE_OK:
		rc =  sqlite_exec(db, "COMMIT TRANSACTION",
		    NULL, NULL, &str);
		idmapdlog(LOG_INFO,
		    "Database created at %s", dbname);

		if (new_db_created != NULL)
			*new_db_created = 1;
		break;
	default:
		idmapdlog(LOG_ERR,
		    "Error initializing database %s (%s)",
		    dbname, str);
		sqlite_freemem(str);
		rc =  sqlite_exec(db, "ROLLBACK TRANSACTION",
		    NULL, NULL, &str);
		break;
	}

	if (SQLITE_OK != rc) {
		/* this is bad - database may be left in a locked state */
		idmapdlog(LOG_ERR,
		    "Error closing transaction (%s)", str);
		sqlite_freemem(str);
	}

	(void) sqlite_close(db);
	return (rc);
}

/*
 * Get the database handle
 */
idmap_retcode
get_db_handle(sqlite **db) {
	char	*errmsg;

	/*
	 * TBD RFE: Retrieve the db handle from thread-specific storage
	 * If none exists, open and store in thread-specific storage.
	 */

	*db = sqlite_open(IDMAP_DBNAME, 0, &errmsg);
	if (*db == NULL) {
		idmapdlog(LOG_ERR,
			"Error opening database %s (%s)",
			IDMAP_DBNAME, CHECK_NULL(errmsg));
		sqlite_freemem(errmsg);
		return (IDMAP_ERR_INTERNAL);
	}
	return (IDMAP_SUCCESS);
}

/*
 * Get the cache handle
 */
idmap_retcode
get_cache_handle(sqlite **db) {
	char	*errmsg;

	/*
	 * TBD RFE: Retrieve the db handle from thread-specific storage
	 * If none exists, open and store in thread-specific storage.
	 */

	*db = sqlite_open(IDMAP_CACHENAME, 0, &errmsg);
	if (*db == NULL) {
		idmapdlog(LOG_ERR,
			"Error opening database %s (%s)",
			IDMAP_CACHENAME, CHECK_NULL(errmsg));
		sqlite_freemem(errmsg);
		return (IDMAP_ERR_INTERNAL);
	}
	return (IDMAP_SUCCESS);
}

#define	CACHE_SQL\
	"CREATE TABLE idmap_cache ("\
	"	sidprefix TEXT,"\
	"	rid INTEGER,"\
	"	windomain TEXT,"\
	"	winname TEXT,"\
	"	pid INTEGER,"\
	"	unixname TEXT,"\
	"	is_user INTEGER,"\
	"	w2u INTEGER,"\
	"	u2w INTEGER,"\
	"	expiration INTEGER"\
	");"\
	"CREATE UNIQUE INDEX idmap_cache_sid_w2u ON idmap_cache"\
	"		(sidprefix, rid, w2u);"\
	"CREATE UNIQUE INDEX idmap_cache_pid_u2w ON idmap_cache"\
	"		(pid, is_user, u2w);"\
	"CREATE TABLE name_cache ("\
	"	sidprefix TEXT,"\
	"	rid INTEGER,"\
	"	name TEXT,"\
	"	domain TEXT,"\
	"	type INTEGER,"\
	"	expiration INTEGER"\
	");"\
	"CREATE UNIQUE INDEX name_cache_sid ON name_cache"\
	"		(sidprefix, rid);"

#define	DB_SQL\
	"CREATE TABLE namerules ("\
	"	is_user INTEGER NOT NULL,"\
	"	windomain TEXT,"\
	"	winname TEXT NOT NULL,"\
	"	is_nt4 INTEGER NOT NULL,"\
	"	unixname NOT NULL,"\
	"	w2u_order INTEGER,"\
	"	u2w_order INTEGER"\
	");"\
	"CREATE UNIQUE INDEX namerules_w2u ON namerules"\
	"		(winname, windomain, is_user, w2u_order);"\
	"CREATE UNIQUE INDEX namerules_u2w ON namerules"\
	"		(unixname, is_user, u2w_order);"

/*
 * Initialize cache and db
 */
int
init_dbs() {
	/* name-based mappings; probably OK to blow away in a pinch(?) */
	if (init_db_instance(IDMAP_DBNAME, DB_SQL, FAIL_IF_CORRUPT, NULL) < 0)
		return (-1);

	/* mappings, name/SID lookup cache + ephemeral IDs; OK to blow away */
	if (init_db_instance(IDMAP_CACHENAME, CACHE_SQL, REMOVE_IF_CORRUPT,
			&_idmapdstate.new_eph_db) < 0)
		return (-1);

	return (0);
}

/*
 * Finalize databases
 */
void
fini_dbs() {
}

/*
 * This table is a listing of status codes that will returned to the
 * client when a SQL command fails with the corresponding error message.
 */
static msg_table_t sqlmsgtable[] = {
	{IDMAP_ERR_U2W_NAMERULE,
	"columns unixname, is_user, u2w_order are not unique"},
	{IDMAP_ERR_W2U_NAMERULE,
	"columns winname, windomain, is_user, w2u_order are not unique"},
	{-1, NULL}
};

/*
 * idmapd's version of string2stat to map SQLite messages to
 * status codes
 */
idmap_retcode
idmapd_string2stat(const char *msg) {
	int i;
	for (i = 0; sqlmsgtable[i].msg; i++) {
		if (strcasecmp(sqlmsgtable[i].msg, msg) == 0)
			return (sqlmsgtable[i].retcode);
	}
	return (IDMAP_ERR_OTHER);
}

/*
 * Execute the given SQL statment without using any callbacks
 */
idmap_retcode
sql_exec_no_cb(sqlite *db, char *sql) {
	char		*errmsg = NULL;
	int		r, i, s;
	struct timespec	rqtp;
	idmap_retcode	retcode;

	for (i = 0, s = SLEEP_TIME; i < MAX_TRIES; i++, s *= 2) {
		if (errmsg) {
			sqlite_freemem(errmsg);
			errmsg = NULL;
		}
		r = sqlite_exec(db, sql, NULL, NULL, &errmsg);
		if (r != SQLITE_BUSY)
			break;
		NANO_SLEEP(rqtp, s);
	}

	if (r != SQLITE_OK) {
		idmapdlog(LOG_ERR, "Database error during %s (%s)",
			sql, CHECK_NULL(errmsg));
		if (r == SQLITE_BUSY) {
			retcode = IDMAP_ERR_BUSY;
		} else {
			retcode = idmap_string2stat(errmsg);
			if (retcode == IDMAP_ERR_OTHER)
				retcode = idmapd_string2stat(errmsg);
		}
		if (errmsg)
			sqlite_freemem(errmsg);
		return (retcode);
	}

	return (IDMAP_SUCCESS);
}

/*
 * Generate expression that can be used in WHERE statements.
 * Examples:
 * <prefix> <col>      <op> <value>   <suffix>
 * ""       "unixuser" "="  "foo" "AND"
 */
idmap_retcode
gen_sql_expr_from_utf8str(const char *prefix, const char *col,
		const char *op, idmap_utf8str *value,
		const char *suffix, char **out) {
	char		*str;
	idmap_stat	retcode;

	if (out == NULL)
		return (IDMAP_ERR_ARG);

	if (value == NULL)
		return (IDMAP_SUCCESS);

	retcode = idmap_utf82str(&str, 0, value);
	if (retcode != IDMAP_SUCCESS)
		return (retcode);

	if (prefix == NULL)
		prefix = "";
	if (suffix == NULL)
		suffix = "";

	*out = sqlite_mprintf("%s %s %s %Q %s",
			prefix, col, op, str, suffix);
	idmap_free(str);
	if (*out == NULL)
		return (IDMAP_ERR_MEMORY);
	return (IDMAP_SUCCESS);
}

/*
 * Generate and execute SQL statement for LIST RPC calls
 */
idmap_retcode
process_list_svc_sql(sqlite *db, char *sql, uint64_t limit,
		list_svc_cb cb, void *result) {
	list_cb_data_t	cb_data;
	char		*errmsg = NULL;
	int		r, i, s;
	struct timespec	rqtp;
	idmap_retcode	retcode = IDMAP_ERR_INTERNAL;

	(void) memset(&cb_data, 0, sizeof (cb_data));
	cb_data.result = result;
	cb_data.limit = limit;

	for (i = 0, s = SLEEP_TIME; i < MAX_TRIES; i++, s *= 2) {
		r = sqlite_exec(db, sql, cb, &cb_data, &errmsg);
		switch (r) {
		case SQLITE_OK:
			retcode = IDMAP_SUCCESS;
			goto out;
		case SQLITE_BUSY:
			if (errmsg) {
				sqlite_freemem(errmsg);
				errmsg = NULL;
			}
			retcode = IDMAP_ERR_BUSY;
			idmapdlog(LOG_DEBUG,
			"Database busy, %d retries remaining",
				MAX_TRIES - i - 1);
			NANO_SLEEP(rqtp, s);
			continue;
		default:
			retcode = IDMAP_ERR_INTERNAL;
			idmapdlog(LOG_ERR,
				"Database error during %s (%s)",
				sql, CHECK_NULL(errmsg));
			goto out;
		};
	}
out:
	if (errmsg)
		sqlite_freemem(errmsg);
	return (retcode);
}

/*
 * This routine is called by callbacks that process the results of
 * LIST RPC calls to validate data and to allocate memory for
 * the result array.
 */
idmap_retcode
validate_list_cb_data(list_cb_data_t *cb_data, int argc, char **argv,
		int ncol, uchar_t **list, size_t valsize) {
	size_t	nsize;
	void	*tmplist;

	if (cb_data->limit > 0 && cb_data->next == cb_data->limit)
		return (IDMAP_NEXT);

	if (argc < ncol || argv == NULL) {
		idmapdlog(LOG_ERR, "Invalid data");
		return (IDMAP_ERR_INTERNAL);
	}

	/* alloc in bulk to reduce number of reallocs */
	if (cb_data->next >= cb_data->len) {
		nsize = (cb_data->len + SIZE_INCR) * valsize;
		tmplist = realloc(*list, nsize);
		if (tmplist == NULL) {
			idmapdlog(LOG_ERR, "Out of memory");
			return (IDMAP_ERR_MEMORY);
		}
		*list = tmplist;
		(void) memset(*list + (cb_data->len * valsize), 0,
			SIZE_INCR * valsize);
		cb_data->len += SIZE_INCR;
	}
	return (IDMAP_SUCCESS);
}

static idmap_retcode
get_namerule_order(char *winname, char *windomain, char *unixname,
		int direction, int *w2u_order, int *u2w_order) {

	*w2u_order = 0;
	*u2w_order = 0;

	/*
	 * Windows to UNIX lookup order:
	 *  1. winname@domain (or winname) to ""
	 *  2. winname@domain (or winname) to unixname
	 *  3. winname@* to ""
	 *  4. winname@* to unixname
	 *  5. *@domain (or *) to *
	 *  6. *@domain (or *) to ""
	 *  7. *@domain (or *) to unixname
	 *  8. *@* to *
	 *  9. *@* to ""
	 * 10. *@* to unixname
	 *
	 * winname is a special case of winname@domain when domain is the
	 * default domain. Similarly * is a special case of *@domain when
	 * domain is the default domain.
	 *
	 * Note that "" has priority over specific names because "" inhibits
	 * mappings and traditionally deny rules always had higher priority.
	 */
	if (direction == 0 || direction == 1) {
		if (winname == NULL)
			return (IDMAP_ERR_W2U_NAMERULE);
		else if (unixname == NULL)
			return (IDMAP_ERR_W2U_NAMERULE);
		else if (EMPTY_NAME(winname))
			return (IDMAP_ERR_W2U_NAMERULE);
		else if (*winname == '*' && windomain && *windomain == '*') {
			if (*unixname == '*')
				*w2u_order = 8;
			else if (EMPTY_NAME(unixname))
				*w2u_order = 9;
			else /* unixname == name */
				*w2u_order = 10;
		} else if (*winname == '*') {
			if (*unixname == '*')
				*w2u_order = 5;
			else if (EMPTY_NAME(unixname))
				*w2u_order = 6;
			else /* name */
				*w2u_order = 7;
		} else if (windomain && *windomain == '*') {
			/* winname == name */
			if (*unixname == '*')
				return (IDMAP_ERR_W2U_NAMERULE);
			else if (EMPTY_NAME(unixname))
				*w2u_order = 3;
			else /* name */
				*w2u_order = 4;
		} else  {
			/* winname == name && windomain == null or name */
			if (*unixname == '*')
				return (IDMAP_ERR_W2U_NAMERULE);
			else if (EMPTY_NAME(unixname))
				*w2u_order = 1;
			else /* name */
				*w2u_order = 2;
		}
	}

	/*
	 * 1. unixname to ""
	 * 2. unixname to winname@domain (or winname)
	 * 3. * to *@domain (or *)
	 * 4. * to ""
	 * 5. * to winname@domain (or winname)
	 */
	if (direction == 0 || direction == 2) {
		if (unixname == NULL || EMPTY_NAME(unixname))
			return (IDMAP_ERR_U2W_NAMERULE);
		else if (winname == NULL)
			return (IDMAP_ERR_U2W_NAMERULE);
		else if (*unixname == '*') {
			if (*winname == '*')
				*u2w_order = 3;
			else if (EMPTY_NAME(winname))
				*u2w_order = 4;
			else
				*u2w_order = 5;
		} else {
			if (*winname == '*')
				return (IDMAP_ERR_U2W_NAMERULE);
			else if (EMPTY_NAME(winname))
				*u2w_order = 1;
			else
				*u2w_order = 2;
		}
	}
	return (IDMAP_SUCCESS);
}

/*
 * Generate and execute SQL statement to add name-based mapping rule
 */
idmap_retcode
add_namerule(sqlite *db, idmap_namerule *rule) {
	char		*sql = NULL;
	idmap_stat	retcode;
	char		*windomain = NULL, *winname = NULL, *dom;
	char		*unixname = NULL;
	int		w2u_order, u2w_order;
	char		w2ubuf[11], u2wbuf[11];

	retcode = idmap_utf82str(&windomain, 0, &rule->windomain);
	if (retcode != IDMAP_SUCCESS)
		goto out;
	retcode = idmap_utf82str(&winname, 0, &rule->winname);
	if (retcode != IDMAP_SUCCESS)
		goto out;
	retcode = idmap_utf82str(&unixname, 0, &rule->unixname);
	if (retcode != IDMAP_SUCCESS)
		goto out;

	retcode = get_namerule_order(winname, windomain, unixname,
			rule->direction, &w2u_order, &u2w_order);
	if (retcode != IDMAP_SUCCESS)
		goto out;

	if (w2u_order)
		(void) snprintf(w2ubuf, sizeof (w2ubuf), "%d", w2u_order);
	if (u2w_order)
		(void) snprintf(u2wbuf, sizeof (u2wbuf), "%d", u2w_order);

	/* Use NULL instead of 0 for w2u_order and u2w_order */

	RDLOCK_CONFIG();
	if (windomain)
		dom = windomain;
	else if (_idmapdstate.cfg->pgcfg.mapping_domain)
		dom = _idmapdstate.cfg->pgcfg.mapping_domain;
	else
		dom = "";
	sql = sqlite_mprintf("INSERT OR ROLLBACK into namerules "
		"(is_user, windomain, winname, is_nt4, "
		"unixname, w2u_order, u2w_order) "
		"VALUES(%d, %Q, %Q, %d, %Q, %q, %q);",
		rule->is_user?1:0,
		dom,
		winname, rule->is_nt4?1:0,
		unixname,
		w2u_order?w2ubuf:NULL,
		u2w_order?u2wbuf:NULL);
	UNLOCK_CONFIG();

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(db, sql);

	if (retcode == IDMAP_ERR_OTHER)
		retcode = IDMAP_ERR_CFG;

out:
	if (windomain)
		idmap_free(windomain);
	if (winname)
		idmap_free(winname);
	if (unixname)
		idmap_free(unixname);
	if (sql)
		sqlite_freemem(sql);
	return (retcode);
}

/*
 * Flush name-based mapping rules
 */
idmap_retcode
flush_namerules(sqlite *db, bool_t is_user) {
	char		*sql = NULL;
	idmap_stat	retcode;

	sql = sqlite_mprintf("DELETE FROM namerules WHERE "
		"is_user = %d;", is_user?1:0);

	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		return (IDMAP_ERR_MEMORY);
	}

	retcode = sql_exec_no_cb(db, sql);

	sqlite_freemem(sql);
	return (retcode);
}

/*
 * Generate and execute SQL statement to remove a name-based mapping rule
 */
idmap_retcode
rm_namerule(sqlite *db, idmap_namerule *rule) {
	char		*sql = NULL;
	idmap_stat	retcode;
	char		*s_windomain = NULL, *s_winname = NULL;
	char		*s_unixname = NULL;
	char		buf[80];

	if (rule->direction < 0 &&
			rule->windomain.idmap_utf8str_len < 1 &&
			rule->winname.idmap_utf8str_len < 1 &&
			rule->unixname.idmap_utf8str_len < 1)
		return (IDMAP_SUCCESS);

	if (rule->direction < 0) {
		buf[0] = 0;
	} else if (rule->direction == 0) {
		(void) snprintf(buf, sizeof (buf), "AND w2u_order > 0"
				" AND u2w_order > 0");
	} else if (rule->direction == 1) {
		(void) snprintf(buf, sizeof (buf), "AND w2u_order > 0"
				" AND (u2w_order = 0 OR u2w_order ISNULL)");
	} else if (rule->direction == 2) {
		(void) snprintf(buf, sizeof (buf), "AND u2w_order > 0"
				" AND (w2u_order = 0 OR w2u_order ISNULL)");
	}

	retcode = IDMAP_ERR_INTERNAL;
	if (rule->windomain.idmap_utf8str_len > 0) {
		if (gen_sql_expr_from_utf8str("AND", "windomain", "=",
				&rule->windomain,
				"", &s_windomain) != IDMAP_SUCCESS)
			goto out;
	}

	if (rule->winname.idmap_utf8str_len > 0) {
		if (gen_sql_expr_from_utf8str("AND", "winname", "=",
				&rule->winname,
				"", &s_winname) != IDMAP_SUCCESS)
			goto out;
	}

	if (rule->unixname.idmap_utf8str_len > 0) {
		if (gen_sql_expr_from_utf8str("AND", "unixname", "=",
				&rule->unixname,
				"", &s_unixname) != IDMAP_SUCCESS)
			goto out;
	}

	sql = sqlite_mprintf("DELETE FROM namerules WHERE "
		"is_user = %d %s %s %s %s;",
		rule->is_user?1:0,
		s_windomain?s_windomain:"",
		s_winname?s_winname:"",
		s_unixname?s_unixname:"",
		buf);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(db, sql);

out:
	if (s_windomain)
		sqlite_freemem(s_windomain);
	if (s_winname)
		sqlite_freemem(s_winname);
	if (s_unixname)
		sqlite_freemem(s_unixname);
	if (sql)
		sqlite_freemem(sql);
	return (retcode);
}

/*
 * Compile the given SQL query and step just once.
 *
 * Input:
 * db  - db handle
 * sql - SQL statement
 *
 * Output:
 * vm     -  virtual SQL machine
 * ncol   - number of columns in the result
 * values - column values
 *
 * Return values:
 * IDMAP_SUCCESS
 * IDMAP_ERR_BUSY
 * IDMAP_ERR_NOTFOUND
 * IDMAP_ERR_INTERNAL
 */

static idmap_retcode
sql_compile_n_step_once(sqlite *db, char *sql, sqlite_vm **vm, int *ncol,
		int reqcol, const char ***values) {
	char		*errmsg = NULL;
	struct timespec	rqtp;
	int		i, r, s;

	if (sqlite_compile(db, sql, NULL, vm, &errmsg) != SQLITE_OK) {
		idmapdlog(LOG_ERR,
			"Database error during %s (%s)",
			sql, CHECK_NULL(errmsg));
		sqlite_freemem(errmsg);
		return (IDMAP_ERR_INTERNAL);
	}

	for (i = 0, s = SLEEP_TIME; i < MAX_TRIES; i++, s *= 2) {
		r = sqlite_step(*vm, ncol, values, NULL);
		if (r != SQLITE_BUSY)
			break;
		NANO_SLEEP(rqtp, s);
	}

	if (r == SQLITE_BUSY) {
		(void) sqlite_finalize(*vm, NULL);
		*vm = NULL;
		return (IDMAP_ERR_BUSY);
	} else if (r == SQLITE_ROW) {
		if (ncol && *ncol < reqcol) {
			(void) sqlite_finalize(*vm, NULL);
			*vm = NULL;
			return (IDMAP_ERR_INTERNAL);
		}
		/* Caller will call finalize after using the results */
		return (IDMAP_SUCCESS);
	} else if (r == SQLITE_DONE) {
		(void) sqlite_finalize(*vm, NULL);
		*vm = NULL;
		return (IDMAP_ERR_NOTFOUND);
	}

	(void) sqlite_finalize(*vm, &errmsg);
	*vm = NULL;
	idmapdlog(LOG_ERR, "Database error during %s (%s)",
		sql, CHECK_NULL(errmsg));
	sqlite_freemem(errmsg);
	return (IDMAP_ERR_INTERNAL);
}

static wksids_table_t wksidstable[] = {
	{"S-1-5", 18, 0, IDMAP_WK_LOCAL_SYSTEM_GID, 0},
	{"S-1-5", 7, 0, GID_NOBODY, 0},
	{"S-1-3", 0, 1, IDMAP_WK_CREATOR_OWNER_UID, 0},
	{"S-1-3", 1, 0, IDMAP_WK_CREATOR_GROUP_GID, 0},
	{NULL, UINT32_MAX, -1, UINT32_MAX, -1}
};

static idmap_retcode
lookup_wksids_sid2pid(idmap_mapping *req, idmap_id_res *res) {
	int i;
	for (i = 0; wksidstable[i].sidprefix; i++) {
		if ((strcasecmp(wksidstable[i].sidprefix,
				req->id1.idmap_id_u.sid.prefix) == 0) &&
				req->id1.idmap_id_u.sid.rid ==
				wksidstable[i].rid &&
				wksidstable[i].direction != 2) {
			switch (req->id2.idtype) {
			case IDMAP_UID:
				if (wksidstable[i].is_user == 0)
					return (IDMAP_ERR_NOTUSER);
				res->id.idmap_id_u.uid = wksidstable[i].pid;
				res->direction = wksidstable[i].direction;
				return (IDMAP_SUCCESS);
			case IDMAP_GID:
				if (wksidstable[i].is_user == 1)
					return (IDMAP_ERR_NOTGROUP);
				res->id.idmap_id_u.gid = wksidstable[i].pid;
				res->direction = wksidstable[i].direction;
				return (IDMAP_SUCCESS);
			case IDMAP_POSIXID:
				res->id.idmap_id_u.uid = wksidstable[i].pid;
				res->id.idtype = (!wksidstable[i].is_user)?
						IDMAP_GID:IDMAP_UID;
				res->direction = wksidstable[i].direction;
				return (IDMAP_SUCCESS);
			default:
				return (IDMAP_ERR_NOTSUPPORTED);
			}
		}
	}
	return (IDMAP_ERR_NOTFOUND);
}

static idmap_retcode
lookup_wksids_pid2sid(idmap_mapping *req, idmap_id_res *res, int is_user) {
	int i;
	for (i = 0; wksidstable[i].sidprefix; i++) {
		if (req->id1.idmap_id_u.uid == wksidstable[i].pid &&
				wksidstable[i].direction != 1 &&
				(wksidstable[i].is_user < 0 ||
				is_user == wksidstable[i].is_user)) {
			switch (req->id2.idtype) {
			case IDMAP_SID:
				res->id.idmap_id_u.sid.rid =
					wksidstable[i].rid;
				res->id.idmap_id_u.sid.prefix =
					strdup(wksidstable[i].sidprefix);
				if (res->id.idmap_id_u.sid.prefix == NULL) {
					idmapdlog(LOG_ERR, "Out of memory");
					return (IDMAP_ERR_MEMORY);
				}
				res->direction = wksidstable[i].direction;
				return (IDMAP_SUCCESS);
			default:
				return (IDMAP_ERR_NOTSUPPORTED);
			}
		}
	}
	return (IDMAP_ERR_NOTFOUND);
}

static idmap_retcode
lookup_cache_sid2pid(sqlite *cache, idmap_mapping *req, idmap_id_res *res) {
	char		*end;
	char		*sql = NULL;
	const char	**values;
	sqlite_vm	*vm = NULL;
	int		ncol, is_user;
	uid_t		pid;
	idmap_utf8str	*str;
	time_t		curtime, exp;
	idmap_retcode	retcode;

	/* Current time */
	errno = 0;
	if ((curtime = time(NULL)) == (time_t)-1) {
		idmapdlog(LOG_ERR,
			"Failed to get current time (%s)",
			strerror(errno));
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	/* SQL to lookup the cache */
	sql = sqlite_mprintf("SELECT pid, is_user, expiration, unixname, u2w "
			"FROM idmap_cache WHERE "
			"sidprefix = %Q AND rid = %u AND w2u = 1 AND "
			"(pid >= 2147483648 OR "
			"(expiration = 0 OR expiration ISNULL OR "
			"expiration > %d));",
			req->id1.idmap_id_u.sid.prefix,
			req->id1.idmap_id_u.sid.rid,
			curtime);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}
	retcode = sql_compile_n_step_once(cache, sql, &vm, &ncol, 5, &values);
	sqlite_freemem(sql);

	if (retcode == IDMAP_ERR_NOTFOUND) {
		goto out;
	} else if (retcode == IDMAP_SUCCESS) {
		/* sanity checks */
		if (values[0] == NULL || values[1] == NULL) {
			retcode = IDMAP_ERR_CACHE;
			goto out;
		}

		pid = strtoul(values[0], &end, 10);
		is_user = strncmp(values[1], "0", 2)?1:0;

		/*
		 * We may have an expired ephemeral mapping. Consider
		 * the expired entry as valid if we are not going to
		 * perform name-based mapping. But do not renew the
		 * expiration.
		 * If we will be doing name-based mapping then store the
		 * ephemeral pid in the result so that we can use it
		 * if we end up doing dynamic mapping again.
		 */
		if (!DO_NOT_ALLOC_NEW_ID_MAPPING(req) &&
				!AVOID_NAMESERVICE(req)) {
			if (IS_EPHEMERAL(pid) && values[2]) {
				exp = strtoll(values[2], &end, 10);
				if (exp && exp <= curtime) {
					/* Store the ephemeral pid */
					res->id.idmap_id_u.uid = pid;
					res->id.idtype = is_user?
						IDMAP_UID:IDMAP_GID;
					res->direction = 0;
					req->direction |= is_user?
						_IDMAP_F_EXP_EPH_UID:
						_IDMAP_F_EXP_EPH_GID;
					retcode = IDMAP_ERR_NOTFOUND;
					goto out;
				}
			}
		}

		switch (req->id2.idtype) {
		case IDMAP_UID:
			if (!is_user)
				retcode = IDMAP_ERR_NOTUSER;
			else
				res->id.idmap_id_u.uid = pid;
			break;
		case IDMAP_GID:
			if (is_user)
				retcode = IDMAP_ERR_NOTGROUP;
			else
				res->id.idmap_id_u.gid = pid;
			break;
		case IDMAP_POSIXID:
			res->id.idmap_id_u.uid = pid;
			res->id.idtype = (is_user)?IDMAP_UID:IDMAP_GID;
			break;
		default:
			retcode = IDMAP_ERR_NOTSUPPORTED;
			break;
		}
	}

out:
	if (retcode == IDMAP_SUCCESS) {
		if (values[4])
			res->direction =
				(strtol(values[4], &end, 10) == 0)?1:0;
		else
			res->direction = 1;

		if (values[3]) {
			str = &req->id2name;
			retcode = idmap_str2utf8(&str, values[3], 0);
			if (retcode != IDMAP_SUCCESS) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
			}
		}
	}
	if (vm)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static idmap_retcode
_lookup_cache_sid2name(sqlite *cache, const char *sidprefix, idmap_rid_t rid,
		char **name, char **domain, int *type) {
	char		*end;
	char		*sql = NULL;
	const char	**values;
	sqlite_vm	*vm = NULL;
	int		ncol;
	time_t		curtime;
	idmap_retcode	retcode = IDMAP_SUCCESS;

	/* Get current time */
	errno = 0;
	if ((curtime = time(NULL)) == (time_t)-1) {
		idmapdlog(LOG_ERR,
			"Failed to get current time (%s)",
			strerror(errno));
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	/* SQL to lookup the cache */
	sql = sqlite_mprintf("SELECT name, domain, type FROM name_cache WHERE "
			"sidprefix = %Q AND rid = %u AND "
			"(expiration = 0 OR expiration ISNULL OR "
			"expiration > %d);",
			sidprefix, rid, curtime);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}
	retcode = sql_compile_n_step_once(cache, sql, &vm, &ncol, 3, &values);
	sqlite_freemem(sql);

	if (retcode == IDMAP_SUCCESS) {
		if (type) {
			if (values[2] == NULL) {
				retcode = IDMAP_ERR_CACHE;
				goto out;
			}
			*type = strtol(values[2], &end, 10);
		}

		if (name && values[0]) {
			if ((*name = strdup(values[0])) == NULL) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}
		}

		if (domain && values[1]) {
			if ((*domain = strdup(values[1])) == NULL) {
				if (name && *name) {
					free(*name);
					*name = NULL;
				}
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}
		}
	}

out:
	if (vm)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static idmap_retcode
verify_type(idmap_id_type idtype, int type, idmap_id_res *res) {
	switch (idtype) {
	case IDMAP_UID:
		if (type != _IDMAP_T_USER)
			return (IDMAP_ERR_NOTUSER);
		res->id.idtype = IDMAP_UID;
		break;
	case IDMAP_GID:
		if (type != _IDMAP_T_GROUP)
			return (IDMAP_ERR_NOTGROUP);
		res->id.idtype = IDMAP_GID;
		break;
	case IDMAP_POSIXID:
		if (type == _IDMAP_T_USER)
			res->id.idtype = IDMAP_UID;
		else if (type == _IDMAP_T_GROUP)
			res->id.idtype = IDMAP_GID;
		else
			return (IDMAP_ERR_SID);
		break;
	default:
		return (IDMAP_ERR_NOTSUPPORTED);
	}
	return (IDMAP_SUCCESS);
}

/*
 * Lookup cache for sid to name
 */
static idmap_retcode
lookup_cache_sid2name(sqlite *cache, idmap_mapping *req, idmap_id_res *res) {
	int		type = -1;
	idmap_retcode	retcode;
	char		*sidprefix;
	idmap_rid_t	rid;
	char		*name = NULL, *domain = NULL;
	idmap_utf8str	*str;

	sidprefix = req->id1.idmap_id_u.sid.prefix;
	rid = req->id1.idmap_id_u.sid.rid;

	/* Lookup sid to name in cache */
	retcode = _lookup_cache_sid2name(cache, sidprefix, rid, &name,
		&domain, &type);
	if (retcode != IDMAP_SUCCESS)
		goto out;

	/* Verify that the sid type matches the request */
	retcode = verify_type(req->id2.idtype, type, res);

out:
	if (retcode == IDMAP_SUCCESS) {
		/* update state in 'req' */
		if (name) {
			str = &req->id1name;
			(void) idmap_str2utf8(&str, name, 1);
		}
		if (domain) {
			str = &req->id1domain;
			(void) idmap_str2utf8(&str, domain, 1);
		}
	} else {
		if (name)
			free(name);
		if (domain)
			free(domain);
	}
	return (retcode);
}

idmap_retcode
lookup_win_batch_sid2name(lookup_state_t *state, idmap_mapping_batch *batch,
		idmap_ids_res *result) {
	idmap_retcode	retcode;
	int		ret, i;
	int		retries = 0;
	idmap_mapping	*req;
	idmap_id_res	*res;

	if (state->ad_nqueries == 0)
		return (IDMAP_SUCCESS);

retry:
	ret = idmap_lookup_batch_start(_idmapdstate.ad, state->ad_nqueries,
		&state->ad_lookup);
	if (ret != 0) {
		idmapdlog(LOG_ERR,
		"Failed to create sid2name batch for AD lookup");
		return (IDMAP_ERR_INTERNAL);
	}

	for (i = 0; i < batch->idmap_mapping_batch_len; i++) {
		req = &batch->idmap_mapping_batch_val[i];
		res = &result->ids.ids_val[i];

		if (retries == 0)
			res->retcode = IDMAP_ERR_RETRIABLE_NET_ERR;
		else if (res->retcode != IDMAP_ERR_RETRIABLE_NET_ERR)
			continue;

		if (req->id1.idtype == IDMAP_SID &&
				req->direction & _IDMAP_F_S2N_AD) {
			retcode = idmap_sid2name_batch_add1(
					state->ad_lookup,
					req->id1.idmap_id_u.sid.prefix,
					&req->id1.idmap_id_u.sid.rid,
					&req->id1name.idmap_utf8str_val,
					&req->id1domain.idmap_utf8str_val,
					(int *)&res->id.idtype,
					&res->retcode);

			if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR)
				break;

			if (retcode != IDMAP_SUCCESS)
				goto out;
		}
	}

	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR)
		idmap_lookup_free_batch(&state->ad_lookup);
	else
		retcode = idmap_lookup_batch_end(&state->ad_lookup, NULL);

	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR && retries++ < 2)
		goto retry;

	return (retcode);

out:
	idmapdlog(LOG_NOTICE, "Windows SID to user/group name lookup failed");
	idmap_lookup_free_batch(&state->ad_lookup);
	return (retcode);
}

idmap_retcode
sid2pid_first_pass(lookup_state_t *state, sqlite *cache, idmap_mapping *req,
		idmap_id_res *res) {
	idmap_retcode	retcode;

	/*
	 * The req->direction field is used to maintain state of the
	 * sid2pid request.
	 */
	req->direction = _IDMAP_F_DONE;

	if (req->id1.idmap_id_u.sid.prefix == NULL) {
		retcode = IDMAP_ERR_SID;
		goto out;
	}
	res->id.idtype = req->id2.idtype;
	res->id.idmap_id_u.uid = UID_NOBODY;

	/* Lookup well-known SIDs */
	retcode = lookup_wksids_sid2pid(req, res);
	if (retcode != IDMAP_ERR_NOTFOUND)
		goto out;

	/* Lookup sid to pid in cache */
	retcode = lookup_cache_sid2pid(cache, req, res);
	if (retcode != IDMAP_ERR_NOTFOUND)
		goto out;

	if (DO_NOT_ALLOC_NEW_ID_MAPPING(req) || AVOID_NAMESERVICE(req)) {
		res->id.idmap_id_u.uid = SENTINEL_PID;
		goto out;
	}

	/*
	 * Failed to find non-expired entry in cache. Tell the caller
	 * that we are not done yet.
	 */
	state->sid2pid_done = FALSE;

	/*
	 * Our next step is name-based mapping. To lookup name-based
	 * mapping rules, we need the windows name and domain-name
	 * associated with the SID.
	 */

	/*
	 * Check if we already have the name (i.e name2pid lookups)
	 */
	if (req->id1name.idmap_utf8str_val &&
	    req->id1domain.idmap_utf8str_val) {
		retcode = IDMAP_SUCCESS;
		req->direction |= _IDMAP_F_S2N_CACHE;
		goto out;
	}

	/*
	 * Lookup sid to winname@domainname in cache.
	 * Note that since libldap caches LDAP results, we may remove
	 * the idmapd's cache eventually
	 */
	retcode = lookup_cache_sid2name(cache, req, res);
	if (retcode == IDMAP_ERR_NOTFOUND) {
		/* Batch sid to name AD lookup request */
		retcode = IDMAP_SUCCESS;
		req->direction |= _IDMAP_F_S2N_AD;
		state->ad_nqueries++;
		goto out;
	} else if (retcode != IDMAP_SUCCESS) {
		goto out;
	}

	retcode = IDMAP_SUCCESS;
	req->direction |= _IDMAP_F_S2N_CACHE;

out:
	res->retcode = idmap_stat4prot(retcode);
	return (retcode);
}

/*
 * Generate SID using the following convention
 * 	<machine-sid-prefix>-<1000 + uid>
 * 	<machine-sid-prefix>-<2^31 + gid>
 */
static idmap_retcode
generate_localsid(idmap_mapping *req, idmap_id_res *res, int is_user) {

	if (_idmapdstate.cfg->pgcfg.machine_sid) {
		/* Skip 1000 UIDs */
		if (is_user && req->id1.idmap_id_u.uid >
				(INT32_MAX - LOCALRID_MIN))
			return (IDMAP_ERR_NOTFOUND);

		RDLOCK_CONFIG();
		res->id.idmap_id_u.sid.prefix =
			strdup(_idmapdstate.cfg->pgcfg.machine_sid);
		if (res->id.idmap_id_u.sid.prefix == NULL) {
			UNLOCK_CONFIG();
			idmapdlog(LOG_ERR, "Out of memory");
			return (IDMAP_ERR_MEMORY);
		}
		UNLOCK_CONFIG();
		res->id.idmap_id_u.sid.rid =
			(is_user)?req->id1.idmap_id_u.uid + LOCALRID_MIN:
			req->id1.idmap_id_u.gid + INT32_MAX + 1;
		res->direction = 0;

		/*
		 * Don't update name_cache because local sids don't have
		 * valid windows names.
		 * We mark the entry as being found in the namecache so that
		 * the cache update routine doesn't update namecache.
		 */
		req->direction = _IDMAP_F_S2N_CACHE;
	}

	return (IDMAP_SUCCESS);
}

static idmap_retcode
lookup_localsid2pid(idmap_mapping *req, idmap_id_res *res) {
	char		*sidprefix;
	uint32_t	rid;
	int		s;

	/*
	 * If the sidprefix == localsid then UID = last RID - 1000 or
	 * GID = last RID - 2^31.
	 */
	sidprefix = req->id1.idmap_id_u.sid.prefix;
	rid = req->id1.idmap_id_u.sid.rid;

	RDLOCK_CONFIG();
	s = (_idmapdstate.cfg->pgcfg.machine_sid)?
		strcasecmp(sidprefix,
		_idmapdstate.cfg->pgcfg.machine_sid):1;
	UNLOCK_CONFIG();

	if (s == 0) {
		switch (req->id2.idtype) {
		case IDMAP_UID:
			if (rid > INT32_MAX) {
				return (IDMAP_ERR_NOTUSER);
			} else if (rid < LOCALRID_MIN) {
				return (IDMAP_ERR_NOTFOUND);
			}
			res->id.idmap_id_u.uid = rid - LOCALRID_MIN;
			res->id.idtype = IDMAP_UID;
			break;
		case IDMAP_GID:
			if (rid <= INT32_MAX) {
				return (IDMAP_ERR_NOTGROUP);
			}
			res->id.idmap_id_u.gid = rid - INT32_MAX - 1;
			res->id.idtype = IDMAP_GID;
			break;
		case IDMAP_POSIXID:
			if (rid > INT32_MAX) {
				res->id.idmap_id_u.gid =
					rid - INT32_MAX - 1;
				res->id.idtype = IDMAP_GID;
			} else if (rid < LOCALRID_MIN) {
				return (IDMAP_ERR_NOTFOUND);
			} else {
				res->id.idmap_id_u.uid = rid - LOCALRID_MIN;
				res->id.idtype = IDMAP_UID;
			}
			break;
		default:
			return (IDMAP_ERR_NOTSUPPORTED);
		}
		return (IDMAP_SUCCESS);
	}

	return (IDMAP_ERR_NOTFOUND);
}

static idmap_retcode
ns_lookup_byname(int is_user, const char *name, idmap_id_res *res) {
	struct passwd	pwd;
	struct group	grp;
	char		buf[1024];
	int		errnum;
	const char	*me = "ns_lookup_byname";

	if (is_user) {
		if (getpwnam_r(name, &pwd, buf, sizeof (buf)) == NULL) {
			errnum = errno;
			idmapdlog(LOG_WARNING,
			"%s: getpwnam_r(%s) failed (%s).",
				me, name,
				errnum?strerror(errnum):"not found");
			if (errnum == 0)
				return (IDMAP_ERR_NOTFOUND);
			else
				return (IDMAP_ERR_INTERNAL);
		}
		res->id.idmap_id_u.uid = pwd.pw_uid;
		res->id.idtype = IDMAP_UID;
	} else {
		if (getgrnam_r(name, &grp, buf, sizeof (buf)) == NULL) {
			errnum = errno;
			idmapdlog(LOG_WARNING,
			"%s: getgrnam_r(%s) failed (%s).",
				me, name,
				errnum?strerror(errnum):"not found");
			if (errnum == 0)
				return (IDMAP_ERR_NOTFOUND);
			else
				return (IDMAP_ERR_INTERNAL);
		}
		res->id.idmap_id_u.gid = grp.gr_gid;
		res->id.idtype = IDMAP_GID;
	}
	return (IDMAP_SUCCESS);
}

/*
 * Name-based mapping
 *
 * Case 1: If no rule matches do ephemeral
 *
 * Case 2: If rule matches and unixname is "" then return no mapping.
 *
 * Case 3: If rule matches and unixname is specified then lookup name
 *  service using the unixname. If unixname not found then return no mapping.
 *
 * Case 4: If rule matches and unixname is * then lookup name service
 *  using winname as the unixname. If unixname not found then process
 *  other rules using the lookup order. If no other rule matches then do
 *  ephemeral. Otherwise, based on the matched rule do Case 2 or 3 or 4.
 *  This allows us to specify a fallback unixname per _domain_ or no mapping
 *  instead of the default behaviour of doing ephemeral mapping.
 *
 * Example 1:
 * *@sfbay == *
 * If looking up windows users foo@sfbay and foo does not exists in
 * the name service then foo@sfbay will be mapped to an ephemeral id.
 *
 * Example 2:
 * *@sfbay == *
 * *@sfbay => guest
 * If looking up windows users foo@sfbay and foo does not exists in
 * the name service then foo@sfbay will be mapped to guest.
 *
 * Example 3:
 * *@sfbay == *
 * *@sfbay => ""
 * If looking up windows users foo@sfbay and foo does not exists in
 * the name service then we will return no mapping for foo@sfbay.
 *
 */
static idmap_retcode
name_based_mapping_sid2pid(sqlite *db, idmap_mapping *req, idmap_id_res *res) {
	const char	*unixname, *winname, *windomain;
	char		*sql = NULL, *errmsg = NULL;
	idmap_retcode	retcode;
	char		*end;
	const char	**values;
	sqlite_vm	*vm = NULL;
	struct timespec rqtp;
	idmap_utf8str	*str;
	int		ncol, r, i, s, is_user;
	const char	*me = "name_based_mapping_sid2pid";

	winname = req->id1name.idmap_utf8str_val;
	windomain = req->id1domain.idmap_utf8str_val;
	is_user = (res->id.idtype == IDMAP_UID)?1:0;

	RDLOCK_CONFIG();
	i = 0;
	if (_idmapdstate.cfg->pgcfg.mapping_domain) {
		if (strcasecmp(_idmapdstate.cfg->pgcfg.mapping_domain,
				windomain) == 0)
			i = 1;
	}
	UNLOCK_CONFIG();

	sql = sqlite_mprintf(
		"SELECT unixname, u2w_order FROM namerules WHERE "
		"w2u_order > 0 AND is_user = %d AND "
		"(winname = %Q OR winname = '*') AND "
		"(windomain = %Q OR windomain = '*' %s) "
		"ORDER BY w2u_order ASC;",
		is_user, winname, windomain, i?"OR windomain ISNULL":"");
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}

	if (sqlite_compile(db, sql, NULL, &vm, &errmsg) != SQLITE_OK) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR,
			"%s: database error (%s)",
			me, CHECK_NULL(errmsg));
		sqlite_freemem(errmsg);
		goto out;
	}

	for (i = 0, s = SLEEP_TIME; ; ) {
		r = sqlite_step(vm, &ncol, &values, NULL);

		if (r == SQLITE_BUSY) {
			if (++i < MAX_TRIES) {
				NANO_SLEEP(rqtp, s);
				s *= 2;
				continue;
			}
			retcode = IDMAP_ERR_BUSY;
			goto out;
		} else if (r == SQLITE_ROW) {
			if (ncol < 2) {
				retcode = IDMAP_ERR_INTERNAL;
				goto out;
			}
			if (values[0] == NULL) {
				retcode = IDMAP_ERR_INTERNAL;
				goto out;
			}

			if (EMPTY_NAME(values[0])) {
				retcode = IDMAP_ERR_NOMAPPING;
				goto out;
			}
			unixname = (values[0][0] == '*')?winname:values[0];
			retcode = ns_lookup_byname(is_user, unixname, res);
			if (retcode == IDMAP_ERR_NOTFOUND) {
				if (unixname == winname)
					/* Case 4 */
					continue;
				else
					/* Case 3 */
					retcode = IDMAP_ERR_NOMAPPING;
			}
			goto out;
		} else if (r == SQLITE_DONE) {
			retcode = IDMAP_ERR_NOTFOUND;
			goto out;
		} else {
			(void) sqlite_finalize(vm, &errmsg);
			vm = NULL;
			idmapdlog(LOG_ERR,
				"%s: database error (%s)",
				me, CHECK_NULL(errmsg));
			sqlite_freemem(errmsg);
			retcode = IDMAP_ERR_INTERNAL;
			goto out;
		}
	}

out:
	if (sql)
		sqlite_freemem(sql);
	if (retcode == IDMAP_SUCCESS) {
		if (values[1])
			res->direction =
				(strtol(values[1], &end, 10) == 0)?1:0;
		else
			res->direction = 1;
		str = &req->id2name;
		retcode = idmap_str2utf8(&str, unixname, 0);
	}
	if (vm)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static
int
get_next_eph_uid(uid_t *next_uid)
{
	uid_t uid;
	gid_t gid;
	int err;

	*next_uid = (uid_t)-1;
	uid = _idmapdstate.next_uid++;
	if (uid >= _idmapdstate.limit_uid) {
		if ((err = allocids(0, 8192, &uid, 0, &gid)) != 0)
			return (err);

		_idmapdstate.limit_uid = uid + 8192;
		_idmapdstate.next_uid = uid;
	}
	*next_uid = uid;

	return (0);
}

static
int
get_next_eph_gid(gid_t *next_gid)
{
	uid_t uid;
	gid_t gid;
	int err;

	*next_gid = (uid_t)-1;
	gid = _idmapdstate.next_gid++;
	if (gid >= _idmapdstate.limit_gid) {
		if ((err = allocids(0, 0, &uid, 8192, &gid)) != 0)
			return (err);

		_idmapdstate.limit_gid = gid + 8192;
		_idmapdstate.next_gid = gid;
	}
	*next_gid = gid;

	return (0);
}


/* ARGSUSED */
static
idmap_retcode
dynamic_ephemeral_mapping(sqlite *cache, idmap_mapping *req,
		idmap_id_res *res) {

	uid_t		next_pid;

	if (IS_EPHEMERAL(res->id.idmap_id_u.uid)) {
		res->direction = 0;
	} else if (res->id.idtype == IDMAP_UID) {
		if (get_next_eph_uid(&next_pid) != 0)
			return (IDMAP_ERR_INTERNAL);
		res->id.idmap_id_u.uid = next_pid;
		res->id.idtype = IDMAP_UID;
		res->direction = 0;
	} else {
		if (get_next_eph_gid(&next_pid) != 0)
			return (IDMAP_ERR_INTERNAL);
		res->id.idmap_id_u.gid = next_pid;
		res->id.idtype = IDMAP_GID;
		res->direction = 0;
	}

	return (IDMAP_SUCCESS);
}

idmap_retcode
sid2pid_second_pass(lookup_state_t *state, sqlite *cache, sqlite *db,
		idmap_mapping *req, idmap_id_res *res) {
	idmap_retcode	retcode;

	/*
	 * The req->direction field is used to maintain state of the
	 * sid2pid request.
	 */

	/* Check if second pass is needed */
	if (req->direction == _IDMAP_F_DONE)
		return (res->retcode);

	/* Get status from previous pass */
	retcode = (res->retcode == IDMAP_NEXT)?IDMAP_SUCCESS:res->retcode;

	if (retcode != IDMAP_SUCCESS) {
		/* Reset return type */
		res->id.idtype = req->id2.idtype;
		res->id.idmap_id_u.uid = UID_NOBODY;

		/* Check if this is a localsid */
		if (retcode == IDMAP_ERR_NOTFOUND &&
				_idmapdstate.cfg->pgcfg.machine_sid) {
			retcode = lookup_localsid2pid(req, res);
			if (retcode == IDMAP_SUCCESS) {
				state->sid2pid_done = FALSE;
				req->direction = _IDMAP_F_S2N_CACHE;
			}
			goto out;
		}
		goto out;
	}

	/*
	 * Verify that the sid type matches the request if the
	 * SID was validated by an AD lookup.
	 */
	if (req->direction & _IDMAP_F_S2N_AD) {
		retcode = verify_type(req->id2.idtype,
			(int)res->id.idtype, res);
		if (retcode != IDMAP_SUCCESS) {
			res->id.idtype = req->id2.idtype;
			res->id.idmap_id_u.uid = UID_NOBODY;
			goto out;
		}
	}

	/* Name-based mapping */
	retcode = name_based_mapping_sid2pid(db, req, res);
	if (retcode == IDMAP_ERR_NOTFOUND)
		/* If not found, do ephemeral mapping */
		goto ephemeral;
	else if (retcode != IDMAP_SUCCESS)
		goto out;

	state->sid2pid_done = FALSE;
	goto out;


ephemeral:
	retcode = dynamic_ephemeral_mapping(cache, req, res);
	if (retcode == IDMAP_SUCCESS)
		state->sid2pid_done = FALSE;

out:
	res->retcode = idmap_stat4prot(retcode);
	return (retcode);
}

idmap_retcode
update_cache_pid2sid(lookup_state_t *state, sqlite *cache,
		idmap_mapping *req, idmap_id_res *res) {
	char		*sql = NULL;
	idmap_retcode	retcode;

	/* Check if we need to cache anything */
	if (req->direction == _IDMAP_F_DONE)
		return (IDMAP_SUCCESS);

	/* We don't cache negative entries */
	if (res->retcode != IDMAP_SUCCESS)
		return (IDMAP_SUCCESS);

	/*
	 * Using NULL for u2w instead of 0 so that our trigger allows
	 * the same pid to be the destination in multiple entries
	 */
	sql = sqlite_mprintf("INSERT OR REPLACE into idmap_cache "
		"(sidprefix, rid, windomain, winname, pid, unixname, "
		"is_user, expiration, w2u, u2w) "
		"VALUES(%Q, %u, %Q, %Q, %u, %Q, %d, "
		"strftime('%%s','now') + 600, %q, 1); ",
		res->id.idmap_id_u.sid.prefix,
		res->id.idmap_id_u.sid.rid,
		req->id2domain.idmap_utf8str_val,
		req->id2name.idmap_utf8str_val,
		req->id1.idmap_id_u.uid,
		req->id1name.idmap_utf8str_val,
		(req->id1.idtype == IDMAP_UID)?1:0,
		(res->direction == 0)?"1":NULL);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(cache, sql);
	if (retcode != IDMAP_SUCCESS)
		goto out;

	state->pid2sid_done = FALSE;
	sqlite_freemem(sql);
	sql = NULL;

	/* If sid2name was found in the cache, no need to update namecache */
	if (req->direction & _IDMAP_F_S2N_CACHE)
		goto out;

	if (req->id2name.idmap_utf8str_val == NULL)
		goto out;

	sql = sqlite_mprintf("INSERT OR REPLACE into name_cache "
		"(sidprefix, rid, name, domain, type, expiration) "
		"VALUES(%Q, %u, %Q, %Q, %d, strftime('%%s','now') + 3600); ",
		res->id.idmap_id_u.sid.prefix,
		res->id.idmap_id_u.sid.rid,
		req->id2name.idmap_utf8str_val,
		req->id2domain.idmap_utf8str_val,
		(req->id1.idtype == IDMAP_UID)?_IDMAP_T_USER:_IDMAP_T_GROUP);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(cache, sql);

out:
	if (sql)
		sqlite_freemem(sql);
	return (retcode);
}

idmap_retcode
update_cache_sid2pid(lookup_state_t *state, sqlite *cache,
		idmap_mapping *req, idmap_id_res *res) {
	char		*sql = NULL;
	idmap_retcode	retcode;
	int		is_eph_user;

	/* Check if we need to cache anything */
	if (req->direction == _IDMAP_F_DONE)
		return (IDMAP_SUCCESS);

	/* We don't cache negative entries */
	if (res->retcode != IDMAP_SUCCESS)
		return (IDMAP_SUCCESS);

	if (req->direction & _IDMAP_F_EXP_EPH_UID)
		is_eph_user = 1;
	else if (req->direction & _IDMAP_F_EXP_EPH_GID)
		is_eph_user = 0;
	else
		is_eph_user = -1;

	if (is_eph_user >= 0 && !IS_EPHEMERAL(res->id.idmap_id_u.uid)) {
		sql = sqlite_mprintf("UPDATE idmap_cache "
			"SET w2u = 0 WHERE "
			"sidprefix = %Q AND rid = %u AND w2u = 1 AND "
			"pid >= 2147483648 AND is_user = %d;",
			req->id1.idmap_id_u.sid.prefix,
			req->id1.idmap_id_u.sid.rid,
			is_eph_user);
		if (sql == NULL) {
			retcode = IDMAP_ERR_INTERNAL;
			idmapdlog(LOG_ERR, "Out of memory");
			goto out;
		}

		retcode = sql_exec_no_cb(cache, sql);
		if (retcode != IDMAP_SUCCESS)
			goto out;

		sqlite_freemem(sql);
		sql = NULL;
	}

	sql = sqlite_mprintf("INSERT OR REPLACE into idmap_cache "
		"(sidprefix, rid, windomain, winname, pid, unixname, "
		"is_user, expiration, w2u, u2w) "
		"VALUES(%Q, %u, %Q, %Q, %u, %Q, %d, "
		"strftime('%%s','now') + 600, 1, %q); ",
		req->id1.idmap_id_u.sid.prefix,
		req->id1.idmap_id_u.sid.rid,
		req->id1domain.idmap_utf8str_val,
		req->id1name.idmap_utf8str_val,
		res->id.idmap_id_u.uid,
		req->id2name.idmap_utf8str_val,
		(res->id.idtype == IDMAP_UID)?1:0,
		(res->direction == 0)?"1":NULL);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(cache, sql);
	if (retcode != IDMAP_SUCCESS)
		goto out;

	state->sid2pid_done = FALSE;
	sqlite_freemem(sql);
	sql = NULL;

	/* If name2sid was found in the cache, no need to update namecache */
	if (req->direction & _IDMAP_F_S2N_CACHE)
		goto out;

	if (req->id1name.idmap_utf8str_val == NULL)
		goto out;

	sql = sqlite_mprintf("INSERT OR REPLACE into name_cache "
		"(sidprefix, rid, name, domain, type, expiration) "
		"VALUES(%Q, %u, %Q, %Q, %d, strftime('%%s','now') + 3600); ",
		req->id1.idmap_id_u.sid.prefix,
		req->id1.idmap_id_u.sid.rid,
		req->id1name.idmap_utf8str_val,
		req->id1domain.idmap_utf8str_val,
		(res->id.idtype == IDMAP_UID)?_IDMAP_T_USER:_IDMAP_T_GROUP);

	if (sql == NULL) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR, "Out of memory");
		goto out;
	}

	retcode = sql_exec_no_cb(cache, sql);

out:
	if (sql)
		sqlite_freemem(sql);
	return (retcode);
}

static idmap_retcode
lookup_cache_pid2sid(sqlite *cache, idmap_mapping *req, idmap_id_res *res,
		int is_user, int getname) {
	char		*end;
	char		*sql = NULL;
	const char	**values;
	sqlite_vm	*vm = NULL;
	int		ncol;
	idmap_retcode	retcode = IDMAP_SUCCESS;
	idmap_utf8str	*str;
	time_t		curtime;

	/* Current time */
	errno = 0;
	if ((curtime = time(NULL)) == (time_t)-1) {
		idmapdlog(LOG_ERR,
			"Failed to get current time (%s)",
			strerror(errno));
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	/* SQL to lookup the cache */
	sql = sqlite_mprintf("SELECT sidprefix, rid, winname, windomain, w2u "
			"FROM idmap_cache WHERE "
			"pid = %u AND u2w = 1 AND is_user = %d AND "
			"(pid >= 2147483648 OR "
			"(expiration = 0 OR expiration ISNULL OR "
			"expiration > %d));",
			req->id1.idmap_id_u.uid, is_user, curtime);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}
	retcode = sql_compile_n_step_once(cache, sql, &vm, &ncol, 5, &values);
	sqlite_freemem(sql);

	if (retcode == IDMAP_ERR_NOTFOUND)
		goto out;
	else if (retcode == IDMAP_SUCCESS) {
		/* sanity checks */
		if (values[0] == NULL || values[1] == NULL) {
			retcode = IDMAP_ERR_CACHE;
			goto out;
		}

		switch (req->id2.idtype) {
		case IDMAP_SID:
			res->id.idmap_id_u.sid.rid =
				strtoul(values[1], &end, 10);
			res->id.idmap_id_u.sid.prefix = strdup(values[0]);
			if (res->id.idmap_id_u.sid.prefix == NULL) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}

			if (values[4])
				res->direction =
				    (strtol(values[4], &end, 10) == 0)?2:0;
			else
				res->direction = 2;

			if (getname == 0 || values[2] == NULL)
				break;
			str = &req->id2name;
			retcode = idmap_str2utf8(&str, values[2], 0);
			if (retcode != IDMAP_SUCCESS) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}

			if (values[3] == NULL)
				break;
			str = &req->id2domain;
			retcode = idmap_str2utf8(&str, values[3], 0);
			if (retcode != IDMAP_SUCCESS) {
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}
			break;
		default:
			retcode = IDMAP_ERR_NOTSUPPORTED;
			break;
		}
	}

out:
	if (vm)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static idmap_retcode
lookup_cache_name2sid(sqlite *cache, const char *name, const char *domain,
		char **sidprefix, idmap_rid_t *rid, int *type) {
	char		*end;
	char		*sql = NULL;
	const char	**values;
	sqlite_vm	*vm = NULL;
	int		ncol;
	time_t		curtime;
	idmap_retcode	retcode = IDMAP_SUCCESS;

	/* Get current time */
	errno = 0;
	if ((curtime = time(NULL)) == (time_t)-1) {
		idmapdlog(LOG_ERR,
			"Failed to get current time (%s)",
			strerror(errno));
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	/* SQL to lookup the cache */
	sql = sqlite_mprintf("SELECT sidprefix, rid, type FROM name_cache "
			"WHERE name = %Q AND domain = %Q AND "
			"(expiration = 0 OR expiration ISNULL OR "
			"expiration > %d);",
			name, domain, curtime);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}
	retcode = sql_compile_n_step_once(cache, sql, &vm, &ncol, 3, &values);
	sqlite_freemem(sql);

	if (retcode == IDMAP_SUCCESS) {
		if (type) {
			if (values[2] == NULL) {
				retcode = IDMAP_ERR_CACHE;
				goto out;
			}
			*type = strtol(values[2], &end, 10);
		}

		if (values[0] == NULL || values[1] == NULL) {
			retcode = IDMAP_ERR_CACHE;
			goto out;
		}
		if ((*sidprefix = strdup(values[0])) == NULL) {
			idmapdlog(LOG_ERR, "Out of memory");
			retcode = IDMAP_ERR_MEMORY;
			goto out;
		}
		*rid = strtoul(values[1], &end, 10);
	}

out:
	if (vm)
		(void) sqlite_finalize(vm, NULL);
	return (retcode);
}

static idmap_retcode
lookup_win_name2sid(const char *name, const char *domain, char **sidprefix,
		idmap_rid_t *rid, int *type) {
	int			ret;
	int			retries = 0;
	idmap_query_state_t	*qs = NULL;
	idmap_retcode		rc, retcode;

	retcode = IDMAP_ERR_NOTFOUND;

retry:
	ret = idmap_lookup_batch_start(_idmapdstate.ad, 1, &qs);
	if (ret != 0) {
		idmapdlog(LOG_ERR,
		"Failed to create name2sid batch for AD lookup");
		return (IDMAP_ERR_INTERNAL);
	}

	retcode = idmap_name2sid_batch_add1(qs, name, domain, sidprefix,
					rid, type, &rc);
	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR)
		goto out;

	if (retcode != IDMAP_SUCCESS) {
		idmapdlog(LOG_ERR,
		"Failed to batch name2sid for AD lookup");
		idmap_lookup_free_batch(&qs);
		return (IDMAP_ERR_INTERNAL);
	}

out:
	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR)
		idmap_lookup_free_batch(&qs);
	else
		retcode = idmap_lookup_batch_end(&qs, NULL);

	if (retcode == IDMAP_ERR_RETRIABLE_NET_ERR && retries++ < 2)
		goto retry;

	if (retcode != IDMAP_SUCCESS) {
		idmapdlog(LOG_NOTICE, "Windows user/group name to SID lookup "
		    "failed");
		return (retcode);
	} else
		return (rc);
	/* NOTREACHED */
}

static idmap_retcode
lookup_name2sid(sqlite *cache, const char *name, const char *domain,
		int *is_user, char **sidprefix, idmap_rid_t *rid,
		idmap_mapping *req) {
	int		type;
	idmap_retcode	retcode;

	/* Lookup sid to name@domain in cache */
	retcode = lookup_cache_name2sid(cache, name, domain, sidprefix,
		rid, &type);
	if (retcode == IDMAP_ERR_NOTFOUND) {
		/* Lookup Windows NT/AD to map name@domain to sid */
		retcode = lookup_win_name2sid(name, domain, sidprefix, rid,
			&type);
		if (retcode != IDMAP_SUCCESS)
			return (retcode);
		req->direction |= _IDMAP_F_S2N_AD;
	} else if (retcode != IDMAP_SUCCESS) {
		return (retcode);
	} else {
		/* Set flag */
		req->direction |= _IDMAP_F_S2N_CACHE;
	}

	/*
	 * Entry found (cache or Windows lookup)
	 * is_user is both input as well as output parameter
	 */
	if (*is_user == 1) {
		if (type != _IDMAP_T_USER)
			return (IDMAP_ERR_NOTUSER);
	} else if (*is_user == 0) {
		if (type != _IDMAP_T_GROUP)
			return (IDMAP_ERR_NOTGROUP);
	} else if (*is_user == -1) {
		/* Caller wants to know if its user or group */
		if (type == _IDMAP_T_USER)
			*is_user = 1;
		else if (type == _IDMAP_T_GROUP)
			*is_user = 0;
		else
			return (IDMAP_ERR_SID);
	}

	return (retcode);
}

static idmap_retcode
name_based_mapping_pid2sid(sqlite *db, sqlite *cache, const char *unixname,
		int is_user, idmap_mapping *req, idmap_id_res *res) {
	const char	*winname, *windomain;
	char		*mapping_domain = NULL;
	char		*sql = NULL, *errmsg = NULL;
	idmap_retcode	retcode;
	char		*end;
	const char	**values;
	sqlite_vm	*vm = NULL;
	idmap_utf8str	*str;
	struct timespec	rqtp;
	int		ncol, r, i, s;
	const char	*me = "name_based_mapping_pid2sid";

	RDLOCK_CONFIG();
	if (_idmapdstate.cfg->pgcfg.mapping_domain) {
		mapping_domain =
			strdup(_idmapdstate.cfg->pgcfg.mapping_domain);
		if (mapping_domain == NULL) {
			UNLOCK_CONFIG();
			idmapdlog(LOG_ERR, "Out of memory");
			retcode = IDMAP_ERR_MEMORY;
			goto out;
		}
	}
	UNLOCK_CONFIG();

	sql = sqlite_mprintf(
		"SELECT winname, windomain, w2u_order FROM namerules WHERE "
		"u2w_order > 0 AND is_user = %d AND "
		"(unixname = %Q OR unixname = '*') "
		"ORDER BY u2w_order ASC;",
		is_user, unixname);
	if (sql == NULL) {
		idmapdlog(LOG_ERR, "Out of memory");
		retcode = IDMAP_ERR_MEMORY;
		goto out;
	}

	if (sqlite_compile(db, sql, NULL, &vm, &errmsg) != SQLITE_OK) {
		retcode = IDMAP_ERR_INTERNAL;
		idmapdlog(LOG_ERR,
			"%s: database error (%s)",
			me, CHECK_NULL(errmsg));
		sqlite_freemem(errmsg);
		goto out;
	}

	for (i = 0, s = SLEEP_TIME; ; ) {
		r = sqlite_step(vm, &ncol, &values, NULL);
		if (r == SQLITE_BUSY) {
			if (++i < MAX_TRIES) {
				NANO_SLEEP(rqtp, s);
				s *= 2;
				continue;
			}
			retcode = IDMAP_ERR_BUSY;
			goto out;
		} else if (r == SQLITE_ROW) {
			if (ncol < 3) {
				retcode = IDMAP_ERR_INTERNAL;
				goto out;
			}
			if (values[0] == NULL) {
				/* values [1] and [2] can be null */
				retcode = IDMAP_ERR_INTERNAL;
				goto out;
			}
			if (EMPTY_NAME(values[0])) {
				retcode = IDMAP_ERR_NOMAPPING;
				goto out;
			}
			winname = (values[0][0] == '*')?unixname:values[0];
			if (values[1])
				windomain = values[1];
			else if (mapping_domain)
				windomain = mapping_domain;
			else {
				idmapdlog(LOG_ERR,
					"%s: no domain", me);
				retcode = IDMAP_ERR_DOMAIN_NOTFOUND;
				goto out;
			}
			/* Lookup winname@domain to sid */
			retcode = lookup_name2sid(cache, winname, windomain,
				&is_user, &res->id.idmap_id_u.sid.prefix,
				&res->id.idmap_id_u.sid.rid, req);
			if (retcode == IDMAP_ERR_NOTFOUND) {
				if (winname == unixname)
					continue;
				else
					retcode = IDMAP_ERR_NOMAPPING;
			}
			goto out;
		} else if (r == SQLITE_DONE) {
			retcode = IDMAP_ERR_NOTFOUND;
			goto out;
		} else {
			(void) sqlite_finalize(vm, &errmsg);
			vm = NULL;
			idmapdlog(LOG_ERR,
				"%s: database error (%s)",
				me, CHECK_NULL(errmsg));
			sqlite_freemem(errmsg);
			retcode = IDMAP_ERR_INTERNAL;
			goto out;
		}
	}

out:
	if (sql)
		sqlite_freemem(sql);
	if (retcode == IDMAP_SUCCESS) {
		if (values[2])
			res->direction =
				(strtol(values[2], &end, 10) == 0)?2:0;
		else
			res->direction = 2;
		str = &req->id2name;
		retcode = idmap_str2utf8(&str, winname, 0);
		if (retcode == IDMAP_SUCCESS) {
			str = &req->id2domain;
			if (windomain == mapping_domain) {
				(void) idmap_str2utf8(&str, windomain, 1);
				mapping_domain = NULL;
			} else
				retcode = idmap_str2utf8(&str, windomain, 0);
		}
	}
	if (vm)
		(void) sqlite_finalize(vm, NULL);
	if (mapping_domain)
		free(mapping_domain);
	return (retcode);
}

idmap_retcode
pid2sid_first_pass(lookup_state_t *state, sqlite *cache, sqlite *db,
		idmap_mapping *req, idmap_id_res *res, int is_user,
		int getname) {
	char		*unixname = NULL;
	struct passwd	pwd;
	struct group	grp;
	idmap_utf8str	*str;
	char		buf[1024];
	int		errnum;
	idmap_retcode	retcode = IDMAP_SUCCESS;
	const char	*me = "pid2sid";

	req->direction = _IDMAP_F_DONE;
	res->id.idtype = req->id2.idtype;

	/* Lookup well-known SIDs */
	retcode = lookup_wksids_pid2sid(req, res, is_user);
	if (retcode != IDMAP_ERR_NOTFOUND)
		goto out;

	/* Lookup pid to sid in cache */
	retcode = lookup_cache_pid2sid(cache, req, res, is_user, getname);
	if (retcode != IDMAP_ERR_NOTFOUND)
		goto out;

	/* Ephemeral ids cannot be allocated during pid2sid */
	if (IS_EPHEMERAL(req->id1.idmap_id_u.uid)) {
		retcode = IDMAP_ERR_NOTFOUND;
		goto out;
	}

	if (DO_NOT_ALLOC_NEW_ID_MAPPING(req) || AVOID_NAMESERVICE(req)) {
		retcode = IDMAP_ERR_NOTFOUND;
		goto out;
	}

	/* uid/gid to name */
	if (req->id1name.idmap_utf8str_val) {
		unixname = req->id1name.idmap_utf8str_val;
	} if (is_user) {
		errno = 0;
		if (getpwuid_r(req->id1.idmap_id_u.uid, &pwd, buf,
				sizeof (buf)) == NULL) {
			errnum = errno;
			idmapdlog(LOG_WARNING,
			"%s: getpwuid_r(%u) failed (%s).",
				me, req->id1.idmap_id_u.uid,
				errnum?strerror(errnum):"not found");
			retcode = (errnum == 0)?IDMAP_ERR_NOTFOUND:
					IDMAP_ERR_INTERNAL;
			goto fallback_localsid;
		}
		unixname = pwd.pw_name;
	} else {
		errno = 0;
		if (getgrgid_r(req->id1.idmap_id_u.gid, &grp, buf,
				sizeof (buf)) == NULL) {
			errnum = errno;
			idmapdlog(LOG_WARNING,
			"%s: getgrgid_r(%u) failed (%s).",
				me, req->id1.idmap_id_u.gid,
				errnum?strerror(errnum):"not found");
			retcode = (errnum == 0)?IDMAP_ERR_NOTFOUND:
					IDMAP_ERR_INTERNAL;
			goto fallback_localsid;
		}
		unixname = grp.gr_name;
	}

	/* Name-based mapping */
	retcode = name_based_mapping_pid2sid(db, cache, unixname, is_user,
		req, res);
	if (retcode == IDMAP_ERR_NOTFOUND) {
		retcode = generate_localsid(req, res, is_user);
		goto out;
	} else if (retcode == IDMAP_SUCCESS)
		goto out;

fallback_localsid:
	/*
	 * Here we generate localsid as fallback id on errors. Our
	 * return status is the error that's been previously assigned.
	 */
	(void) generate_localsid(req, res, is_user);

out:
	if (retcode == IDMAP_SUCCESS) {
		if (req->id1name.idmap_utf8str_val == NULL && unixname) {
			str = &req->id1name;
			retcode = idmap_str2utf8(&str, unixname, 0);
		}
	}
	if (req->direction != _IDMAP_F_DONE)
		state->pid2sid_done = FALSE;
	res->retcode = idmap_stat4prot(retcode);
	return (retcode);
}

static idmap_retcode
lookup_win_sid2name(const char *sidprefix, idmap_rid_t rid, char **name,
		char **domain, int *type) {
	int			ret;
	idmap_query_state_t	*qs = NULL;
	idmap_retcode		rc, retcode;

	retcode = IDMAP_ERR_NOTFOUND;

	ret = idmap_lookup_batch_start(_idmapdstate.ad, 1, &qs);
	if (ret != 0) {
		idmapdlog(LOG_ERR,
		"Failed to create sid2name batch for AD lookup");
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

	ret = idmap_sid2name_batch_add1(
			qs, sidprefix, &rid, name, domain, type, &rc);
	if (ret != 0) {
		idmapdlog(LOG_ERR,
		"Failed to batch sid2name for AD lookup");
		retcode = IDMAP_ERR_INTERNAL;
		goto out;
	}

out:
	if (qs) {
		ret = idmap_lookup_batch_end(&qs, NULL);
		if (ret != 0) {
			idmapdlog(LOG_ERR,
			"Failed to execute sid2name AD lookup");
			retcode = IDMAP_ERR_INTERNAL;
		} else
			retcode = rc;
	}

	return (retcode);
}

static void
copy_id_mapping(idmap_mapping *mapping, idmap_mapping *request)
{
	mapping->flag = request->flag;
	mapping->direction = request->direction;

	mapping->id1.idtype = request->id1.idtype;
	if (request->id1.idtype == IDMAP_SID) {
		mapping->id1.idmap_id_u.sid.rid =
		    request->id1.idmap_id_u.sid.rid;
		if (request->id1.idmap_id_u.sid.prefix)
			mapping->id1.idmap_id_u.sid.prefix =
			    strdup(request->id1.idmap_id_u.sid.prefix);
		else
			mapping->id1.idmap_id_u.sid.prefix = NULL;
	} else {
		mapping->id1.idmap_id_u.uid = request->id1.idmap_id_u.uid;
	}

	mapping->id1domain.idmap_utf8str_len =
	    request->id1domain.idmap_utf8str_len;
	if (mapping->id1domain.idmap_utf8str_len)
		mapping->id1domain.idmap_utf8str_val =
		    strdup(request->id1domain.idmap_utf8str_val);
	else
		mapping->id1domain.idmap_utf8str_val = NULL;

	mapping->id1name.idmap_utf8str_len  =
	    request->id1name.idmap_utf8str_len;
	if (mapping->id1name.idmap_utf8str_len)
		mapping->id1name.idmap_utf8str_val =
		    strdup(request->id1name.idmap_utf8str_val);
	else
		mapping->id1name.idmap_utf8str_val = NULL;

	mapping->id2.idtype = request->id2.idtype;
	if (request->id2.idtype == IDMAP_SID) {
		mapping->id2.idmap_id_u.sid.rid =
		    request->id2.idmap_id_u.sid.rid;
		if (request->id2.idmap_id_u.sid.prefix)
			mapping->id2.idmap_id_u.sid.prefix =
			    strdup(request->id2.idmap_id_u.sid.prefix);
		else
			mapping->id2.idmap_id_u.sid.prefix = NULL;
	} else {
		mapping->id2.idmap_id_u.uid = request->id2.idmap_id_u.uid;
	}

	mapping->id2domain.idmap_utf8str_len =
	    request->id2domain.idmap_utf8str_len;
	if (mapping->id2domain.idmap_utf8str_len)
		mapping->id2domain.idmap_utf8str_val =
		    strdup(request->id2domain.idmap_utf8str_val);
	else
		mapping->id2domain.idmap_utf8str_val = NULL;

	mapping->id2name.idmap_utf8str_len =
	    request->id2name.idmap_utf8str_len;
	if (mapping->id2name.idmap_utf8str_len)
		mapping->id2name.idmap_utf8str_val =
		    strdup(request->id2name.idmap_utf8str_val);
	else
		mapping->id2name.idmap_utf8str_val = NULL;
}


idmap_retcode
get_w2u_mapping(sqlite *cache, sqlite *db, idmap_mapping *request,
		idmap_mapping *mapping) {
	idmap_id_res	idres;
	lookup_state_t	state;
	idmap_utf8str	*str;
	int		is_user;
	idmap_retcode	retcode;
	const char	*winname, *windomain;

	(void) memset(&idres, 0, sizeof (idres));
	(void) memset(&state, 0, sizeof (state));

	if (request->id2.idtype == IDMAP_UID)
		is_user = 1;
	else if (request->id2.idtype == IDMAP_GID)
		is_user = 0;
	else if (request->id2.idtype == IDMAP_POSIXID)
		is_user = -1;
	else {
		retcode = IDMAP_ERR_IDTYPE;
		goto out;
	}

	/* Copy data from request to result */
	copy_id_mapping(mapping, request);

	winname = mapping->id1name.idmap_utf8str_val;
	windomain = mapping->id1domain.idmap_utf8str_val;

	if (winname == NULL && windomain) {
		retcode = IDMAP_ERR_ARG;
		goto out;
	}

	if (winname && windomain == NULL) {
		str = &mapping->id1domain;
		RDLOCK_CONFIG();
		if (_idmapdstate.cfg->pgcfg.mapping_domain) {
			retcode = idmap_str2utf8(&str,
				_idmapdstate.cfg->pgcfg.mapping_domain, 0);
			if (retcode != IDMAP_SUCCESS) {
				UNLOCK_CONFIG();
				idmapdlog(LOG_ERR, "Out of memory");
				retcode = IDMAP_ERR_MEMORY;
				goto out;
			}
		}
		UNLOCK_CONFIG();
		windomain = mapping->id1domain.idmap_utf8str_val;
	}

	if (winname && EMPTY_STRING(mapping->id1.idmap_id_u.sid.prefix)) {
		retcode = lookup_name2sid(cache, winname, windomain,
			&is_user, &mapping->id1.idmap_id_u.sid.prefix,
			&mapping->id1.idmap_id_u.sid.rid, mapping);
		if (retcode != IDMAP_SUCCESS)
			goto out;
		if (mapping->id2.idtype == IDMAP_POSIXID)
			mapping->id2.idtype = is_user?IDMAP_UID:IDMAP_GID;
	}

	state.sid2pid_done = TRUE;
	retcode = sid2pid_first_pass(&state, cache, mapping, &idres);
	if (IDMAP_ERROR(retcode) || state.sid2pid_done == TRUE)
		goto out;

	if (state.ad_nqueries) {
		/* sid2name AD lookup */
		retcode = lookup_win_sid2name(
			mapping->id1.idmap_id_u.sid.prefix,
			mapping->id1.idmap_id_u.sid.rid,
			&mapping->id1name.idmap_utf8str_val,
			&mapping->id1domain.idmap_utf8str_val,
			(int *)&idres.id.idtype);

		idres.retcode = retcode;
	}

	state.sid2pid_done = TRUE;
	retcode = sid2pid_second_pass(&state, cache, db, mapping, &idres);
	if (IDMAP_ERROR(retcode) || state.sid2pid_done == TRUE)
		goto out;

	/* Update cache */
	(void) update_cache_sid2pid(&state, cache, mapping, &idres);

out:
	if (retcode == IDMAP_SUCCESS) {
		mapping->direction = idres.direction;
		mapping->id2 = idres.id;
		(void) memset(&idres, 0, sizeof (idres));
	}
	xdr_free(xdr_idmap_id_res, (caddr_t)&idres);
	return (retcode);
}

idmap_retcode
get_u2w_mapping(sqlite *cache, sqlite *db, idmap_mapping *request,
		idmap_mapping *mapping, int is_user) {
	idmap_id_res	idres;
	lookup_state_t	state;
	struct passwd	pwd;
	struct group	grp;
	char		buf[1024];
	int		errnum;
	idmap_retcode	retcode;
	const char	*unixname;
	const char	*me = "get_u2w_mapping";

	/*
	 * In order to re-use the pid2sid code, we convert
	 * our input data into structs that are expected by
	 * pid2sid_first_pass.
	 */

	(void) memset(&idres, 0, sizeof (idres));
	(void) memset(&state, 0, sizeof (state));

	/* Copy data from request to result */
	copy_id_mapping(mapping, request);

	unixname = mapping->id1name.idmap_utf8str_val;

	if (unixname == NULL && mapping->id1.idmap_id_u.uid == SENTINEL_PID) {
		retcode = IDMAP_ERR_ARG;
		goto out;
	}

	if (unixname && mapping->id1.idmap_id_u.uid == SENTINEL_PID) {
		/* Get uid/gid by name */
		if (is_user) {
			errno = 0;
			if (getpwnam_r(unixname, &pwd, buf,
					sizeof (buf)) == NULL) {
				errnum = errno;
				idmapdlog(LOG_WARNING,
				"%s: getpwnam_r(%s) failed (%s).",
					me, unixname,
					errnum?strerror(errnum):"not found");
				retcode = (errnum == 0)?IDMAP_ERR_NOTFOUND:
						IDMAP_ERR_INTERNAL;
				goto out;
			}
			mapping->id1.idmap_id_u.uid = pwd.pw_uid;
		} else {
			errno = 0;
			if (getgrnam_r(unixname, &grp, buf,
					sizeof (buf)) == NULL) {
				errnum = errno;
				idmapdlog(LOG_WARNING,
				"%s: getgrnam_r(%s) failed (%s).",
					me, unixname,
					errnum?strerror(errnum):"not found");
				retcode = (errnum == 0)?IDMAP_ERR_NOTFOUND:
						IDMAP_ERR_INTERNAL;
				goto out;
			}
			mapping->id1.idmap_id_u.gid = grp.gr_gid;
		}
	}

	state.pid2sid_done = TRUE;
	retcode = pid2sid_first_pass(&state, cache, db, mapping, &idres,
			is_user, 1);
	if (IDMAP_ERROR(retcode) || state.pid2sid_done == TRUE)
		goto out;

	/* Update cache */
	(void) update_cache_pid2sid(&state, cache, mapping, &idres);

out:
	mapping->direction = idres.direction;
	mapping->id2 = idres.id;
	(void) memset(&idres, 0, sizeof (idres));
	xdr_free(xdr_idmap_id_res, (caddr_t)&idres);
	return (retcode);
}
