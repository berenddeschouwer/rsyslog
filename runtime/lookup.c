/* lookup.c
 * Support for lookup tables in RainerScript.
 *
 * Copyright 2013 Adiscon GmbH.
 *
 * This file is part of the rsyslog runtime library.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <json.h>
#include <assert.h>

#include "rsyslog.h"
#include "srUtils.h"
#include "errmsg.h"
#include "lookup.h"
#include "msg.h"
#include "rsconf.h"
#include "dirty.h"
#include "unicode-helper.h"

/* definitions for objects we access */
DEFobjStaticHelpers
DEFobjCurrIf(errmsg)
DEFobjCurrIf(glbl)

/* forward definitions */
static rsRetVal lookupReadFile(lookup_t *pThis, const uchar* name, const uchar* filename);
static void lookupDestruct(lookup_t *pThis);

/* static data */
/* tables for interfacing with the v6 config system (as far as we need to) */
static struct cnfparamdescr modpdescr[] = {
	{ "name", eCmdHdlrString, CNFPARAM_REQUIRED },
	{ "file", eCmdHdlrString, CNFPARAM_REQUIRED }
};
static struct cnfparamblk modpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(modpdescr)/sizeof(struct cnfparamdescr),
	  modpdescr
	};

/* internal data-types */
typedef struct uint32_index_val_s {
	uint32_t index;
	uchar *val;
} uint32_index_val_t;

/* create a new lookup table object AND include it in our list of
 * lookup tables.
 */
rsRetVal
lookupNew(lookup_ref_t **ppThis)
{
	lookup_ref_t *pThis = NULL;
	lookup_t *t = NULL;
	DEFiRet;

	CHKmalloc(pThis = calloc(1, sizeof(lookup_ref_t)));
	CHKmalloc(t = calloc(1, sizeof(lookup_t)));
	pthread_rwlock_init(&pThis->rwlock, NULL);

	if(loadConf->lu_tabs.root == NULL) {
		loadConf->lu_tabs.root = pThis;
		pThis->next = NULL;
	} else {
		pThis->next = loadConf->lu_tabs.last;
	}
	loadConf->lu_tabs.last = pThis;

	pThis->self = t;

	*ppThis = pThis;
finalize_it:
	if(iRet != RS_RET_OK) {
		free(t);
		free(pThis);
	}
	RETiRet;
}

static void
lookupRefDestruct(lookup_ref_t *pThis)
{
	pthread_rwlock_destroy(&pThis->rwlock);
	lookupDestruct(pThis->self);
	free(pThis->name);
	free(pThis->filename);
	free(pThis);
}

static void
destructTable_str(lookup_t *pThis) {
	int i = 0;
	lookup_string_tab_entry_t *entries = pThis->table.str->entries;
	for (i = 0; i < pThis->nmemb; i++) {
		free(entries[i].key);
	}
	free(entries);
	free(pThis->table.str);
}


static void
destructTable_arr(lookup_t *pThis) {
	free(pThis->table.arr->interned_val_refs);
	free(pThis->table.arr);
}

static void
destructTable_sparseArr(lookup_t *pThis) {
	free(pThis->table.sprsArr->entries);
	free(pThis->table.sprsArr);
}

static void
lookupDestruct(lookup_t *pThis) {
	int i;

	if (pThis == NULL) return;
	
	if (pThis->type == STRING_LOOKUP_TABLE) {
		destructTable_str(pThis);
	} else if (pThis->type == ARRAY_LOOKUP_TABLE) {
		destructTable_arr(pThis);
	} else if (pThis->type == SPARSE_ARRAY_LOOKUP_TABLE) {
		destructTable_sparseArr(pThis);
	} else {
		assert(0);//destructor is missing for a new lookup-table type
	}
	for (i = 0; i < pThis->interned_val_count; i++) {
		free(pThis->interned_vals[i]);
	}
	free(pThis->interned_vals);
	free(pThis->nomatch);
	free(pThis);
}

void
lookupInitCnf(lookup_tables_t *lu_tabs)
{
	lu_tabs->root = NULL;
	lu_tabs->last = NULL;
}

void
lookupDestroyCnf() {
	lookup_ref_t *luref, *luref_next;
	for(luref = loadConf->lu_tabs.root ; luref != NULL ; ) {
		luref_next = luref->next;
		lookupRefDestruct(luref);
		luref = luref_next;
	}	
}

/* comparison function for qsort() */
static int
qs_arrcmp_strtab(const void *s1, const void *s2)
{
	return ustrcmp(((lookup_string_tab_entry_t*)s1)->key, ((lookup_string_tab_entry_t*)s2)->key);
}

static int
qs_arrcmp_ustrs(const void *s1, const void *s2)
{
	return ustrcmp(*(uchar**)s1, *(uchar**)s2);
}

static int
qs_arrcmp_uint32_index_val(const void *s1, const void *s2)
{
	return ((uint32_index_val_t*)s1)->index - ((uint32_index_val_t*)s2)->index;
}

static int
qs_arrcmp_sprsArrtab(const void *s1, const void *s2)
{
	return ((lookup_sparseArray_tab_entry_t*)s1)->key - ((lookup_sparseArray_tab_entry_t*)s2)->key;
}

/* comparison function for bsearch() and string array compare
 * this is for the string lookup table type
 */
static int
bs_arrcmp_strtab(const void *s1, const void *s2)
{
	return strcmp((char*)s1, (char*)((lookup_string_tab_entry_t*)s2)->key);
}

static int
bs_arrcmp_str(const void *s1, const void *s2)
{
	return strcmp((uchar*)s1, *(uchar**)s2);
}

static int
bs_arrcmp_sprsArrtab(const void *s1, const void *s2)
{
	return *(uint32_t*)s1 - ((lookup_sparseArray_tab_entry_t*)s2)->key;
}

static inline const char*
defaultVal(lookup_t *pThis) {
	return (pThis->nomatch == NULL) ? "" : (const char*) pThis->nomatch;
}

/* lookup_fn for different types of tables */
static es_str_t*
lookupKey_str(lookup_t *pThis, lookup_key_t key) {
	lookup_string_tab_entry_t *entry;
	const char *r;
	entry = bsearch(key.k_str, pThis->table.str->entries, pThis->nmemb, sizeof(lookup_string_tab_entry_t), bs_arrcmp_strtab);
	if(entry == NULL) {
		r = defaultVal(pThis);
	} else {
		r = (const char*)entry->interned_val_ref;
	}
	return es_newStrFromCStr(r, strlen(r));
}

static es_str_t*
lookupKey_arr(lookup_t *pThis, lookup_key_t key) {
	const char *r;
	uint32_t uint_key = key.k_uint;

	if (pThis->table.arr->first_key + pThis->nmemb <= uint_key) {
		r = defaultVal(pThis);
	} else {
		r = (char*) pThis->table.arr->interned_val_refs[uint_key - pThis->table.arr->first_key];
	}
	return es_newStrFromCStr(r, strlen(r));
}

static es_str_t*
lookupKey_sprsArr(lookup_t *pThis, lookup_key_t key) {
	lookup_sparseArray_tab_entry_t *entry;
	const char *r;
	entry = bsearch(&key.k_uint, pThis->table.sprsArr->entries, pThis->nmemb, sizeof(lookup_sparseArray_tab_entry_t), bs_arrcmp_sprsArrtab);
	if(entry == NULL) {
		r = defaultVal(pThis);
	} else {
		r = (const char*)entry->interned_val_ref;
	}
	return es_newStrFromCStr(r, strlen(r));
}

/* builders for different table-types */
static inline rsRetVal
build_StringTable(lookup_t *pThis, struct json_object *jtab) {
	uint32_t i;
	struct json_object *jrow, *jindex, *jvalue;
	uchar *value, *canonicalValueRef;
	DEFiRet;
	
	pThis->table.str = NULL;
	CHKmalloc(pThis->table.str = calloc(1, sizeof(lookup_string_tab_t)));
	CHKmalloc(pThis->table.str->entries = calloc(pThis->nmemb, sizeof(lookup_string_tab_entry_t)));

	for(i = 0; i < pThis->nmemb; i++) {
		jrow = json_object_array_get_idx(jtab, i);
		jindex = json_object_object_get(jrow, "index");
		jvalue = json_object_object_get(jrow, "value");
		CHKmalloc(pThis->table.str->entries[i].key = strdup(json_object_get_string(jindex)));
		value = (uchar*) json_object_get_string(jvalue);
		canonicalValueRef = *(uchar**) bsearch(value, pThis->interned_vals, pThis->interned_val_count, sizeof(uchar*), bs_arrcmp_str);
		assert(canonicalValueRef != NULL);
		pThis->table.str->entries[i].interned_val_ref = canonicalValueRef;
	}
	qsort(pThis->table.str->entries, pThis->nmemb, sizeof(lookup_string_tab_entry_t), qs_arrcmp_strtab);
		
	pThis->lookup = lookupKey_str;
	pThis->key_type = LOOKUP_KEY_TYPE_STRING;
	
finalize_it:
	RETiRet;
}

static inline rsRetVal
build_ArrayTable(lookup_t *pThis, struct json_object *jtab, const uchar *name) {
	uint32_t i;
	struct json_object *jrow, *jindex, *jvalue;
	uchar *value, *canonicalValueRef;
	uint32_t prev_index, index;
	uint8_t prev_index_set;
	uint32_index_val_t *indexes = NULL;
	DEFiRet;

	prev_index_set = 0;
	
	pThis->table.arr = NULL;
	CHKmalloc(indexes = calloc(pThis->nmemb, sizeof(uint32_index_val_t)));
	CHKmalloc(pThis->table.arr = calloc(1, sizeof(lookup_array_tab_t)));
	CHKmalloc(pThis->table.arr->interned_val_refs = calloc(pThis->nmemb, sizeof(uchar*)));

	for(i = 0; i < pThis->nmemb; i++) {
		jrow = json_object_array_get_idx(jtab, i);
		jindex = json_object_object_get(jrow, "index");
		jvalue = json_object_object_get(jrow, "value");
		indexes[i].index = (uint32_t) json_object_get_int(jindex);
		indexes[i].val = (uchar*) json_object_get_string(jvalue);
	}
	qsort(indexes, pThis->nmemb, sizeof(uint32_index_val_t), qs_arrcmp_uint32_index_val);
	for(i = 0; i < pThis->nmemb; i++) {
		index = indexes[i].index;
		if (prev_index_set == 0) {
			prev_index = index;
			prev_index_set = 1;
		} else {
			if (index != ++prev_index) {
				errmsg.LogError(0, RS_RET_INVALID_VALUE, "'array' lookup table name: '%s' has non-contigious values between index '%d' and '%d'",
								name, prev_index, index);
				ABORT_FINALIZE(RS_RET_INVALID_VALUE);
			}
		}
		canonicalValueRef = *(uchar**) bsearch(indexes[i].val, pThis->interned_vals, pThis->interned_val_count, sizeof(uchar*), bs_arrcmp_str);
		assert(canonicalValueRef != NULL);
		pThis->table.arr->interned_val_refs[i] = canonicalValueRef;
	}
		
	pThis->lookup = lookupKey_arr;
	pThis->key_type = LOOKUP_KEY_TYPE_UINT;

finalize_it:
	free(indexes);
	RETiRet;
}

static inline rsRetVal
build_SparseArrayTable(lookup_t *pThis, struct json_object *jtab) {
	uint32_t i;
	struct json_object *jrow, *jindex, *jvalue;
	uchar *value, *canonicalValueRef;
	DEFiRet;
	
	pThis->table.str = NULL;
	CHKmalloc(pThis->table.sprsArr = calloc(1, sizeof(lookup_sparseArray_tab_t)));
	CHKmalloc(pThis->table.sprsArr->entries = calloc(pThis->nmemb, sizeof(lookup_sparseArray_tab_entry_t)));

	for(i = 0; i < pThis->nmemb; i++) {
		jrow = json_object_array_get_idx(jtab, i);
		jindex = json_object_object_get(jrow, "index");
		jvalue = json_object_object_get(jrow, "value");
		pThis->table.sprsArr->entries[i].key = (uint32_t) json_object_get_int(jindex);
		value = (uchar*) json_object_get_string(jvalue);
		canonicalValueRef = *(uchar**) bsearch(value, pThis->interned_vals, pThis->interned_val_count, sizeof(uchar*), bs_arrcmp_str);
		assert(canonicalValueRef != NULL);
		pThis->table.sprsArr->entries[i].interned_val_ref = canonicalValueRef;
	}
	qsort(pThis->table.sprsArr->entries, pThis->nmemb, sizeof(lookup_sparseArray_tab_entry_t), qs_arrcmp_sprsArrtab);
		
	pThis->lookup = lookupKey_sprsArr;
	pThis->key_type = LOOKUP_KEY_TYPE_UINT;
	
finalize_it:
	RETiRet;
}

rsRetVal
lookupBuildTable(lookup_t *pThis, struct json_object *jroot, const uchar* name)
{
	struct json_object *jversion, *jnomatch, *jtype, *jtab;
	struct json_object *jrow, *jindex, *jvalue;
	const char *table_type, *nomatch_value;
	const uchar **all_values;
	const uchar *curr, *prev;
	uint32_t i, j;
	uint32_t uniq_values;

	DEFiRet;
	all_values = NULL;

	jversion = json_object_object_get(jroot, "version");
	jnomatch = json_object_object_get(jroot, "nomatch");
	jtype = json_object_object_get(jroot, "type");
	jtab = json_object_object_get(jroot, "table");
	pThis->nmemb = json_object_array_length(jtab);
	table_type = json_object_get_string(jtype);
	if (table_type == NULL) {
		table_type = "string";
	}

	CHKmalloc(all_values = malloc(pThis->nmemb * sizeof(uchar*)));

	/* before actual table can be loaded, prepare all-value list and remove duplicates*/
	for(i = 0; i < pThis->nmemb; i++) {
		jrow = json_object_array_get_idx(jtab, i);
		jvalue = json_object_object_get(jrow, "value");
		all_values[i] = (const uchar*) json_object_get_string(jvalue);
	}
	qsort(all_values, pThis->nmemb, sizeof(uchar*), qs_arrcmp_ustrs);
	uniq_values = 1;
	for(i = 1; i < pThis->nmemb; i++) {
		curr = all_values[i];
		prev = all_values[i - 1];
		if (strcmp(prev, curr) != 0) {
			uniq_values++;
		}
	}

	CHKmalloc(pThis->interned_vals = malloc(uniq_values * sizeof(uchar*)));
	j = 0;
	CHKmalloc(pThis->interned_vals[j++] = strdup(all_values[0]));
	for(i = 1; i < pThis->nmemb ; ++i) {
		curr = all_values[i];
		prev = all_values[i - 1];
		if (strcmp(prev, curr) != 0) {
			CHKmalloc(pThis->interned_vals[j++] = strdup(all_values[i]));
		}
	}
	pThis->interned_val_count = uniq_values;
	/* uniq values captured (sorted) */

	nomatch_value = json_object_get_string(jnomatch);
	if (nomatch_value != NULL) {
		CHKmalloc(pThis->nomatch = strdup(nomatch_value));
	}

	if (strcmp(table_type, "array") == 0) {
		pThis->type = ARRAY_LOOKUP_TABLE;
		CHKiRet(build_ArrayTable(pThis, jtab, name));
	} else if (strcmp(table_type, "sparseArray") == 0) {
		pThis->type = SPARSE_ARRAY_LOOKUP_TABLE;
		CHKiRet(build_SparseArrayTable(pThis, jtab));
	} else {
		pThis->type = STRING_LOOKUP_TABLE;
		CHKiRet(build_StringTable(pThis, jtab));
	}

finalize_it:
	if (all_values != NULL) free(all_values);
	RETiRet;
}


/* find a lookup table. This is a naive O(n) algo, but this really
 * doesn't matter as it is called only a few times during config
 * load. The function returns either a pointer to the requested
 * table or NULL, if not found.
 */
lookup_ref_t *
lookupFindTable(uchar *name)
{
	lookup_ref_t *curr;

	for(curr = loadConf->lu_tabs.root ; curr != NULL ; curr = curr->next) {
		if(!ustrcmp(curr->name, name))
			break;
	}
	return curr;
}


/* this reloads a lookup table. This is done while the engine is running,
 * as such the function must ensure proper locking and proper order of
 * operations (so that nothing can interfere). If the table cannot be loaded,
 * the old table is continued to be used.
 */
static rsRetVal
lookupReload(lookup_ref_t *pThis)
{
	uint32_t i;
	lookup_t *newlu, *oldlu; /* dummy to be able to use support functions without 
						affecting current settings. */
	DEFiRet;

	oldlu = pThis->self;
	newlu = NULL;
	
	DBGPRINTF("reload requested for lookup table '%s'\n", pThis->name);
	CHKmalloc(newlu = calloc(1, sizeof(lookup_t)));
	CHKiRet(lookupReadFile(newlu, pThis->name, pThis->filename));
	/* all went well, copy over data members */
	pthread_rwlock_wrlock(&pThis->rwlock);
	pThis->self = newlu;
	pthread_rwlock_unlock(&pThis->rwlock);
finalize_it:
	if (iRet != RS_RET_OK) {
		errmsg.LogError(0, RS_RET_INTERNAL_ERROR,
						"lookup table '%s' could not be reloaded from file '%s'",
						pThis->name, pThis->filename);
		lookupDestruct(newlu);
	} else {
		errmsg.LogError(0, RS_RET_OK, "lookup table '%s' reloaded from file '%s'",
						pThis->name, pThis->filename);
		lookupDestruct(oldlu);
	}
	RETiRet;
}


/* reload all lookup tables on HUP */
void
lookupDoHUP()
{
	lookup_ref_t *luref;
	for(luref = loadConf->lu_tabs.root ; luref != NULL ; luref = luref->next) {
		lookupReload(luref);
	}
}


/* returns either a pointer to the value (read only!) or NULL
 * if either the key could not be found or an error occured.
 * Note that an estr_t object is returned. The caller is 
 * responsible for freeing it.
 */
es_str_t *
lookupKey(lookup_ref_t *pThis, lookup_key_t key)
{
	es_str_t *estr;
	lookup_t *t;
	pthread_rwlock_rdlock(&pThis->rwlock);
	t = pThis->self;
	estr = t->lookup(t, key);
	pthread_rwlock_unlock(&pThis->rwlock);
	return estr;
}


/* note: widely-deployed json_c 0.9 does NOT support incremental
 * parsing. In order to keep compatible with e.g. Ubuntu 12.04LTS,
 * we read the file into one big memory buffer and parse it at once.
 * While this is not very elegant, it will not pose any real issue
 * for "reasonable" lookup tables (and "unreasonably" large ones
 * will probably have other issues as well...).
 */
static rsRetVal
lookupReadFile(lookup_t *pThis, const uchar *name, const uchar *filename)
{
	struct json_tokener *tokener = NULL;
	struct json_object *json = NULL;
	int eno;
	char errStr[1024];
	char *iobuf = NULL;
	int fd;
	ssize_t nread;
	struct stat sb;
	DEFiRet;


	if(stat((char*)filename, &sb) == -1) {
		eno = errno;
		errmsg.LogError(0, RS_RET_FILE_NOT_FOUND,
			"lookup table file '%s' stat failed: %s",
			filename, rs_strerror_r(eno, errStr, sizeof(errStr)));
		ABORT_FINALIZE(RS_RET_FILE_NOT_FOUND);
	}

	CHKmalloc(iobuf = malloc(sb.st_size));

	if((fd = open((const char*) filename, O_RDONLY)) == -1) {
		eno = errno;
		errmsg.LogError(0, RS_RET_FILE_NOT_FOUND,
			"lookup table file '%s' could not be opened: %s",
			filename, rs_strerror_r(eno, errStr, sizeof(errStr)));
		ABORT_FINALIZE(RS_RET_FILE_NOT_FOUND);
	}

	tokener = json_tokener_new();
	nread = read(fd, iobuf, sb.st_size);
	if(nread != (ssize_t) sb.st_size) {
		eno = errno;
		errmsg.LogError(0, RS_RET_READ_ERR,
			"lookup table file '%s' read error: %s",
			filename, rs_strerror_r(eno, errStr, sizeof(errStr)));
		ABORT_FINALIZE(RS_RET_READ_ERR);
	}

	json = json_tokener_parse_ex(tokener, iobuf, sb.st_size);
	if(json == NULL) {
		errmsg.LogError(0, RS_RET_JSON_PARSE_ERR,
			"lookup table file '%s' json parsing error",
			filename);
		ABORT_FINALIZE(RS_RET_JSON_PARSE_ERR);
	}
	free(iobuf); /* early free to sever resources*/
	iobuf = NULL; /* make sure no double-free */

	/* got json object, now populate our own in-memory structure */
	CHKiRet(lookupBuildTable(pThis, json, name));

finalize_it:
	free(iobuf);
	if(tokener != NULL)
		json_tokener_free(tokener);
	if(json != NULL)
		json_object_put(json);
	RETiRet;
}


rsRetVal
lookupProcessCnf(struct cnfobj *o)
{
	struct cnfparamvals *pvals;
	lookup_ref_t *lu;
	short i;
	DEFiRet;
	lu = NULL;

	pvals = nvlstGetParams(o->nvlst, &modpblk, NULL);
	if(pvals == NULL) {
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}
	DBGPRINTF("lookupProcessCnf params:\n");
	cnfparamsPrint(&modpblk, pvals);
	
	CHKiRet(lookupNew(&lu));

	for(i = 0 ; i < modpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(modpblk.descr[i].name, "file")) {
			CHKmalloc(lu->filename = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL));
		} else if(!strcmp(modpblk.descr[i].name, "name")) {
			CHKmalloc(lu->name = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL));
		} else {
			dbgprintf("lookup_table: program error, non-handled "
			  "param '%s'\n", modpblk.descr[i].name);
		}
	}
	CHKiRet(lookupReadFile(lu->self, lu->name, lu->filename));
	DBGPRINTF("lookup table '%s' loaded from file '%s'\n", lu->name, lu->filename);

finalize_it:
	cnfparamvalsDestruct(pvals, &modpblk);
	if (iRet != RS_RET_OK) {
		if (lu != NULL) {
			lookupDestruct(lu->self);
			lu->self = NULL;
		}
	}
	RETiRet;
}

void
lookupClassExit(void)
{
	objRelease(glbl, CORE_COMPONENT);
	objRelease(errmsg, CORE_COMPONENT);
}

rsRetVal
lookupClassInit(void)
{
	DEFiRet;
	CHKiRet(objGetObjInterface(&obj));
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
finalize_it:
	RETiRet;
}
