/* -------------------------------------------------------------------------
 *
 * execGrouping.cpp
 *	  executor utility routines for grouping, hashing, and aggregation
 *
 * Note: we currently assume that equality and hashing functions are not
 * collation-sensitive, so the code in this file has no support for passing
 * collation settings through from callers.  That may have to change someday.
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/runtime/executor/execGrouping.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/tableam.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "access/hash.h"

static uint32 TupleHashTableHash(const void* key, Size keysize);
static int TupleHashTableMatch(const void* key1, const void* key2, Size keysize);

/*****************************************************************************
 *		Utility routines for grouping tuples together
 *****************************************************************************/
/*
 * execTuplesMatch
 *		Return true if two tuples match in all the indicated fields.
 *
 * This actually implements SQL's notion of "not distinct".  Two nulls
 * match, a null and a not-null don't match.
 *
 * slot1, slot2: the tuples to compare (must have same columns!)
 * numCols: the number of attributes to be examined
 * matchColIdx: array of attribute column numbers
 * eqFunctions: array of fmgr lookup info for the equality functions to use
 * evalContext: short-term memory context for executing the functions
 *
 * NB: evalContext is reset each time!
 */
bool execTuplesMatch(TupleTableSlot* slot1, TupleTableSlot* slot2, int numCols, AttrNumber* matchColIdx,
    FmgrInfo* eqfunctions, MemoryContext evalContext, Oid *collations)
{
    MemoryContext oldContext;
    bool result = false;
    int i;

    /* Reset and switch into the temp context. */
    MemoryContextReset(evalContext);
    oldContext = MemoryContextSwitchTo(evalContext);

    /*
     * We cannot report a match without checking all the fields, but we can
     * report a non-match as soon as we find unequal fields.  So, start
     * comparing at the last field (least significant sort key). That's the
     * most likely to be different if we are dealing with sorted input.
     */
    result = true;

    for (i = numCols; --i >= 0;) {
        AttrNumber att = matchColIdx[i];
        Datum attr1, attr2;
        bool isNull1 = false;
        bool isNull2 = false;

        attr1 = tableam_tslot_getattr(slot1, att, &isNull1);

        attr2 = tableam_tslot_getattr(slot2, att, &isNull2);

        if (isNull1 != isNull2) {
            result = false; /* one null and one not; they aren't equal */
            break;
        }

        if (isNull1) {
            continue; /* both are null, treat as equal */
        }

        /* Apply the type-specific equality function */
        if (DB_IS_CMPT(B_FORMAT) && collations != NULL) {
            if (!DatumGetBool(FunctionCall2Coll(&eqfunctions[i], collations[i], attr1, attr2))) {
                result = false; /* they aren't equal */
                break;
            }
        } else {
            if (!DatumGetBool(FunctionCall2(&eqfunctions[i], attr1, attr2))) {
                result = false; /* representing not equal */
                break;
            }
        }
    }

    MemoryContextSwitchTo(oldContext);

    return result;
}

/*
 * execTuplesUnequal
 *		Return true if two tuples are definitely unequal in the indicated
 *		fields.
 *
 * Nulls are neither equal nor unequal to anything else.  A true result
 * is obtained only if there are non-null fields that compare not-equal.
 *
 * Parameters are identical to execTuplesMatch.
 */
bool execTuplesUnequal(TupleTableSlot* slot1, TupleTableSlot* slot2, int numCols, AttrNumber* matchColIdx,
    FmgrInfo* eqfunctions, MemoryContext evalContext, Oid *collations)
{
    MemoryContext oldContext;
    bool result = false;
    int i;

    Assert(slot1->tts_tupleDescriptor->td_tam_ops == slot2->tts_tupleDescriptor->td_tam_ops);

    /* Reset and switch into the temp context. */
    MemoryContextReset(evalContext);
    oldContext = MemoryContextSwitchTo(evalContext);

    /*
     * We cannot report a match without checking all the fields, but we can
     * report a non-match as soon as we find unequal fields.  So, start
     * comparing at the last field (least significant sort key). That's the
     * most likely to be different if we are dealing with sorted input.
     */
    result = false;

    for (i = numCols; --i >= 0;) {
        AttrNumber att = matchColIdx[i];
        Datum attr1, attr2;
        bool isNull1 = false;
        bool isNull2 = false;

        attr1 = tableam_tslot_getattr(slot1, att, &isNull1);

        if (isNull1) {
            continue; /* can't prove anything here */
        }

        attr2 = tableam_tslot_getattr(slot2, att, &isNull2);

        if (isNull2) {
            continue; /* can't prove anything here */
        }

        /* Apply the type-specific equality function */
        if (DB_IS_CMPT(B_FORMAT) && collations != NULL) {
            if (!DatumGetBool(FunctionCall2Coll(&eqfunctions[i], collations[i], attr1, attr2))) {
                result = true; /* they aren't equal */
                break;
            }
        } else {
            if (!DatumGetBool(FunctionCall2(&eqfunctions[i], attr1, attr2))) {
                /* representing not equal */
                result = true;
                break;
            }
        }
    }

    MemoryContextSwitchTo(oldContext);

    return result;
}

/*
 * execTuplesMatchPrepare
 *		Look up the equality functions needed for execTuplesMatch or
 *		execTuplesUnequal, given an array of equality operator OIDs.
 *
 * The result is a palloc'd array.
 */
FmgrInfo* execTuplesMatchPrepare(int numCols, Oid* eqOperators)
{
    FmgrInfo* eqFunctions = (FmgrInfo*)palloc(numCols * sizeof(FmgrInfo));
    int i;

    for (i = 0; i < numCols; i++) {
        Oid eq_opr = eqOperators[i];
        Oid eq_function;

        eq_function = get_opcode(eq_opr);
        fmgr_info(eq_function, &eqFunctions[i]);
    }

    return eqFunctions;
}

/*
 * execTuplesHashPrepare
 *		Look up the equality and hashing functions needed for a TupleHashTable.
 *
 * This is similar to execTuplesMatchPrepare, but we also need to find the
 * hash functions associated with the equality operators.  *eqFunctions and
 * *hashFunctions receive the palloc'd result arrays.
 *
 * Note: we expect that the given operators are not cross-type comparisons.
 */
void execTuplesHashPrepare(int numCols, Oid* eqOperators, FmgrInfo** eqFunctions, FmgrInfo** hashFunctions)
{
    int i;

    *eqFunctions = (FmgrInfo*)palloc(numCols * sizeof(FmgrInfo));
    *hashFunctions = (FmgrInfo*)palloc(numCols * sizeof(FmgrInfo));

    for (i = 0; i < numCols; i++) {
        Oid eq_opr = eqOperators[i];
        Oid eq_function;
        Oid left_hash_function;
        Oid right_hash_function;

        eq_function = get_opcode(eq_opr);
        if (!get_op_hash_functions(eq_opr, &left_hash_function, &right_hash_function))
            ereport(ERROR,
                (errmodule(MOD_EXECUTOR),
                    errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("could not find hash function for hash operator %u for TupleHashTable, column number %d, "
                           "total column number %d.",
                        eq_opr,
                        i,
                        numCols)));

        /* We're not supporting cross-type cases here */
        Assert(left_hash_function == right_hash_function);
        fmgr_info(eq_function, &(*eqFunctions)[i]);
        fmgr_info(right_hash_function, &(*hashFunctions)[i]);
    }
}

/*****************************************************************************
 *		Utility routines for all-in-memory hash tables
 *
 * These routines build hash tables for grouping tuples together (eg, for
 * hash aggregation).  There is one entry for each not-distinct set of tuples
 * presented.
 *****************************************************************************/
/*
 * Construct an empty TupleHashTable
 *
 *	numCols, keyColIdx: identify the tuple fields to use as lookup key
 *	eqfunctions: equality comparison functions to use
 *	hashfunctions: datatype-specific hashing functions to use
 *	nbuckets: initial estimate of hashtable size
 *	entrysize: size of each entry (at least sizeof(TupleHashEntryData))
 *	tablecxt: memory context in which to store table and table entries
 *	tempcxt: short-lived context for evaluation hash and comparison functions
 *
 * The function arrays may be made with execTuplesHashPrepare().  Note they
 * are not cross-type functions, but expect to see the table datatype(s)
 * on both sides.
 *
 * Note that keyColIdx, eqfunctions, and hashfunctions must be allocated in
 * storage that will live as long as the hashtable does.
 */
TupleHashTable BuildTupleHashTable(int numCols, AttrNumber* keyColIdx, FmgrInfo* eqfunctions, FmgrInfo* hashfunctions,
    long nbuckets, Size entrysize, MemoryContext tablecxt, MemoryContext tempcxt, int workMem, Oid *collations)
{
    TupleHashTable hashtable;
    HASHCTL hash_ctl;

    Assert(nbuckets > 0);
    Assert(entrysize >= sizeof(TupleHashEntryData));

    /* Limit initial table size request to not more than work_mem */
    nbuckets = Min(nbuckets, (long)((workMem * 1024L) / entrysize));
    if (u_sess->attr.attr_sql.hashagg_table_size != 0)
        nbuckets = Min(nbuckets, u_sess->attr.attr_sql.hashagg_table_size);

    hashtable = (TupleHashTable)MemoryContextAlloc(tablecxt, sizeof(TupleHashTableData));

    hashtable->numCols = numCols;
    hashtable->keyColIdx = keyColIdx;
    hashtable->tab_hash_funcs = hashfunctions;
    hashtable->tab_eq_funcs = eqfunctions;
    hashtable->tablecxt = tablecxt;
    hashtable->tempcxt = tempcxt;
    hashtable->entrysize = entrysize;
    hashtable->tableslot = NULL; /* will be made on first lookup */
    hashtable->inputslot = NULL;
    hashtable->in_hash_funcs = NULL;
    hashtable->cur_eq_funcs = NULL;
    hashtable->width = 0;
    hashtable->add_width = true;
    hashtable->causedBySysRes = false;

    errno_t rc = memset_s(&hash_ctl, sizeof(hash_ctl), 0, sizeof(hash_ctl));
    securec_check(rc, "\0", "\0");
    hash_ctl.keysize = sizeof(TupleHashEntryData);
    hash_ctl.entrysize = entrysize;
    hash_ctl.hash = TupleHashTableHash;
    hash_ctl.match = TupleHashTableMatch;
    hash_ctl.hcxt = tablecxt;
    hashtable->hashtab =
        hash_create("TupleHashTable", nbuckets, &hash_ctl, HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
    hashtable->tab_collations = collations;

    return hashtable;
}

/*
 * Find or create a hashtable entry for the tuple group containing the
 * given tuple.  The tuple must be the same type as the hashtable entries.
 *
 * If isnew is NULL, we do not create new entries; we return NULL if no
 * match is found.
 *
 * If isnew isn't NULL, then a new entry is created if no existing entry
 * matches.  On return, *isnew is true if the entry is newly created,
 * false if it existed already.  Any extra space in a new entry has been
 * zeroed.
 *
 * If isinserthashtbl is false, the para of hash search is HASH_FIND
 * instead of HASH_ENTER. This slot will be insert into temp file instead of
 * hash table if it is new
 *
 */
TupleHashEntry LookupTupleHashEntry(TupleHashTable hashtable, TupleTableSlot* slot, bool* isnew, bool isinserthashtbl)
{
    TupleHashEntry entry;
    MemoryContext oldContext;
    TupleHashTable saveCurHT;
    TupleHashEntryData dummy;
    bool found = false;

    /* If first time through, clone the input slot to make table slot */
    if (hashtable->tableslot == NULL) {
        TupleDesc tupdesc;

        oldContext = MemoryContextSwitchTo(hashtable->tablecxt);

        /*
         * We copy the input tuple descriptor just for safety --- we assume
         * all input tuples will have equivalent descriptors.
         */
        tupdesc = CreateTupleDescCopy(slot->tts_tupleDescriptor);
        hashtable->tableslot = MakeSingleTupleTableSlot(tupdesc);
        MemoryContextSwitchTo(oldContext);
    }

    /* Need to run the hash functions in short-lived context */
    oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

    /*
     * Set up data needed by hash and match functions
     *
     * We save and restore u_sess->exec_cxt.cur_tuple_hash_table just in case someone manages to
     * invoke this code re-entrantly.
     */
    hashtable->inputslot = slot;
    hashtable->in_hash_funcs = hashtable->tab_hash_funcs;
    hashtable->cur_eq_funcs = hashtable->tab_eq_funcs;

    saveCurHT = u_sess->exec_cxt.cur_tuple_hash_table;
    u_sess->exec_cxt.cur_tuple_hash_table = hashtable;

    /* Search the hash table */
    dummy.firstTuple = NULL; /* flag to reference inputslot */

    if (isinserthashtbl) {
        entry = (TupleHashEntry)hash_search(hashtable->hashtab, &dummy, isnew ? HASH_ENTER : HASH_FIND, &found);
    } else {
        /* this slot will be insert into temp file instead of hash table if it is not found in hash table */
        entry = (TupleHashEntry)hash_search(hashtable->hashtab, &dummy, HASH_FIND, &found);
    }

    if (isnew != NULL) {
        if (found) {
            /* found pre-existing entry */
            *isnew = false;
        } else {
            if (entry) {
                Assert(isinserthashtbl);
                /*
                 * created new entry
                 *
                 * Zero any caller-requested space in the entry.  (This zaps the
                 * "key data" dynahash.c copied into the new entry, but we don't
                 * care since we're about to overwrite it anyway.)
                 */
                errno_t errorno = memset_s(entry, hashtable->entrysize, 0, hashtable->entrysize);
                securec_check(errorno, "\0", "\0");

                /* Copy the first tuple into the table context */
                MemoryContextSwitchTo(hashtable->tablecxt);
                entry->firstTuple = ExecCopySlotMinimalTuple(slot);
                if (hashtable->add_width)
                    hashtable->width += entry->firstTuple->t_len;
            }

            *isnew = true;
        }
    }

    u_sess->exec_cxt.cur_tuple_hash_table = saveCurHT;

    MemoryContextSwitchTo(oldContext);

    return entry;
}

/*
 * Search for a hashtable entry matching the given tuple.  No entry is
 * created if there's not a match.  This is similar to the non-creating
 * case of LookupTupleHashEntry, except that it supports cross-type
 * comparisons, in which the given tuple is not of the same type as the
 * table entries.  The caller must provide the hash functions to use for
 * the input tuple, as well as the equality functions, since these may be
 * different from the table's internal functions.
 */
TupleHashEntry FindTupleHashEntry(
    TupleHashTable hashtable, TupleTableSlot* slot, FmgrInfo* eqfunctions, FmgrInfo* hashfunctions)
{
    TupleHashEntry entry;
    MemoryContext oldContext;
    TupleHashTable saveCurHT;
    TupleHashEntryData dummy;

    /* Need to run the hash functions in short-lived context */
    oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

    /*
     * Set up data needed by hash and match functions
     *
     * We save and restore u_sess->exec_cxt.cur_tuple_hash_table just in case someone manages to
     * invoke this code re-entrantly.
     */
    hashtable->inputslot = slot;
    hashtable->in_hash_funcs = hashfunctions;
    hashtable->cur_eq_funcs = eqfunctions;

    saveCurHT = u_sess->exec_cxt.cur_tuple_hash_table;
    u_sess->exec_cxt.cur_tuple_hash_table = hashtable;

    /* Search the hash table */
    dummy.firstTuple = NULL; /* flag to reference inputslot */
    entry = (TupleHashEntry)hash_search(hashtable->hashtab, &dummy, HASH_FIND, NULL);

    u_sess->exec_cxt.cur_tuple_hash_table = saveCurHT;

    MemoryContextSwitchTo(oldContext);

    return entry;
}

/*
 * Compute the hash value for a tuple
 *
 * The passed-in key is a pointer to TupleHashEntryData.  In an actual hash
 * table entry, the firstTuple field points to a tuple (in MinimalTuple
 * format).  LookupTupleHashEntry sets up a dummy TupleHashEntryData with a
 * NULL firstTuple field --- that cues us to look at the inputslot instead.
 * This convention avoids the need to materialize virtual input tuples unless
 * they actually need to get copied into the table.
 *
 * u_sess->exec_cxt.cur_tuple_hash_table must be set before calling this, since dynahash.c
 * doesn't provide any API that would let us get at the hashtable otherwise.
 *
 * Also, the caller must select an appropriate memory context for running
 * the hash functions. (dynahash.c doesn't change CurrentMemoryContext.)
 */
static uint32 TupleHashTableHash(const void* key, Size keysize)
{
    MinimalTuple tuple = ((const TupleHashEntryData*)key)->firstTuple;
    TupleTableSlot* slot = NULL;
    TupleHashTable hashtable = u_sess->exec_cxt.cur_tuple_hash_table;
    int numCols = hashtable->numCols;
    AttrNumber* keyColIdx = hashtable->keyColIdx;
    FmgrInfo* hashfunctions = NULL;
    uint32 hashkey = 0;
    int i;

    if (tuple == NULL) {
        /* Process the current input tuple for the table */
        slot = hashtable->inputslot;
        hashfunctions = hashtable->in_hash_funcs;
    } else {
        /* Process a tuple already stored in the table */
        /* (this case never actually occurs in current dynahash.c code) */
        slot = hashtable->tableslot;
        ExecStoreMinimalTuple(tuple, slot, false);
        hashfunctions = hashtable->tab_hash_funcs;
    }

    /* Get the Table Accessor Method*/
    for (i = 0; i < numCols; i++) {
        AttrNumber att = keyColIdx[i];
        Datum attr;
        bool isNull = false;

        /* rotate hashkey left 1 bit at each step */
        hashkey = (hashkey << 1) | ((hashkey & 0x80000000) ? 1 : 0);

        attr = tableam_tslot_getattr(slot, att, &isNull);
        /* treat nulls as having hash key 0 */
        if (!isNull) {
            uint32 hkey;
            if (DB_IS_CMPT(B_FORMAT) && hashtable->tab_collations != NULL) {
                hkey = DatumGetUInt32(FunctionCall1Coll(&hashfunctions[i], hashtable->tab_collations[i], attr));
            } else {
                hkey = DatumGetUInt32(FunctionCall1(&hashfunctions[i], attr));
            }
            hashkey ^= hkey;
        }
    }

    hashkey = DatumGetUInt32(hash_uint32(hashkey));

    return hashkey;
}

/*
 * See whether two tuples (presumably of the same hash value) match
 *
 * As above, the passed pointers are pointers to TupleHashEntryData.
 *
 * u_sess->exec_cxt.cur_tuple_hash_table must be set before calling this, since dynahash.c
 * doesn't provide any API that would let us get at the hashtable otherwise.
 *
 * Also, the caller must select an appropriate memory context for running
 * the compare functions.  (dynahash.c doesn't change CurrentMemoryContext.)
 */
static int TupleHashTableMatch(const void* key1, const void* key2, Size keysize)
{
    MinimalTuple tuple1 = ((const TupleHashEntryData*)key1)->firstTuple;

#ifdef USE_ASSERT_CHECKING
    MinimalTuple tuple2 = ((const TupleHashEntryData*)key2)->firstTuple;
#endif
    TupleTableSlot* slot1 = NULL;
    TupleTableSlot* slot2 = NULL;
    TupleHashTable hashtable = u_sess->exec_cxt.cur_tuple_hash_table;

    /*
     * We assume that dynahash.c will only ever call us with the first
     * argument being an actual table entry, and the second argument being
     * LookupTupleHashEntry's dummy TupleHashEntryData.  The other direction
     * could be supported too, but is not currently used by dynahash.c.
     */
    Assert(tuple1 != NULL);
    slot1 = hashtable->tableslot;
    ExecStoreMinimalTuple(tuple1, slot1, false);
    Assert(tuple2 == NULL);
    slot2 = hashtable->inputslot;

    /* For crosstype comparisons, the inputslot must be first */
    if (execTuplesMatch(slot2, slot1, hashtable->numCols, hashtable->keyColIdx, hashtable->cur_eq_funcs,
                        hashtable->tempcxt, hashtable->tab_collations)) {
        return 0;
    } else {
        return 1;
    }
}
