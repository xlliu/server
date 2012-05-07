/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ident "Copyright (c) 2007-2009 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."
#ident "$Id$"

#include <db.h>
#include "ydb-internal.h"
#include "indexer.h"
#include <newbrt/log_header.h>
#include "ydb_row_lock.h"
#include "ydb_write.h"
#include "ydb_db.h"

static YDB_WRITE_LAYER_STATUS_S ydb_write_layer_status;
#ifdef STATUS_VALUE
#undef STATUS_VALUE
#endif
#define STATUS_VALUE(x) ydb_write_layer_status.status[x].value.num

#define STATUS_INIT(k,t,l) { \
        ydb_write_layer_status.status[k].keyname = #k; \
        ydb_write_layer_status.status[k].type    = t;  \
        ydb_write_layer_status.status[k].legend  = l; \
    }

static void
ydb_write_layer_status_init (void) {
    // Note, this function initializes the keyname, type, and legend fields.
    // Value fields are initialized to zero by compiler.
    STATUS_INIT(YDB_LAYER_NUM_INSERTS,                UINT64,   "dictionary inserts");
    STATUS_INIT(YDB_LAYER_NUM_INSERTS_FAIL,           UINT64,   "dictionary inserts fail");
    STATUS_INIT(YDB_LAYER_NUM_DELETES,                UINT64,   "dictionary deletes");
    STATUS_INIT(YDB_LAYER_NUM_DELETES_FAIL,           UINT64,   "dictionary deletes fail");
    STATUS_INIT(YDB_LAYER_NUM_UPDATES,                UINT64,   "dictionary updates");
    STATUS_INIT(YDB_LAYER_NUM_UPDATES_FAIL,           UINT64,   "dictionary updates fail");
    STATUS_INIT(YDB_LAYER_NUM_UPDATES_BROADCAST,      UINT64,   "dictionary broadcast updates");
    STATUS_INIT(YDB_LAYER_NUM_UPDATES_BROADCAST_FAIL, UINT64,   "dictionary broadcast updates fail");
    STATUS_INIT(YDB_LAYER_NUM_MULTI_INSERTS,          UINT64,   "dictionary multi inserts");
    STATUS_INIT(YDB_LAYER_NUM_MULTI_INSERTS_FAIL,     UINT64,   "dictionary multi inserts fail");
    STATUS_INIT(YDB_LAYER_NUM_MULTI_DELETES,          UINT64,   "dictionary multi deletes");
    STATUS_INIT(YDB_LAYER_NUM_MULTI_DELETES_FAIL,     UINT64,   "dictionary multi deletes fail");
    STATUS_INIT(YDB_LAYER_NUM_MULTI_UPDATES,          UINT64,   "dictionary updates multi");
    STATUS_INIT(YDB_LAYER_NUM_MULTI_UPDATES_FAIL,     UINT64,   "dictionary updates multi fail");
    ydb_write_layer_status.initialized = true;
}
#undef STATUS_INIT

void
ydb_write_layer_get_status(YDB_WRITE_LAYER_STATUS statp) {
    if (!ydb_write_layer_status.initialized)
        ydb_write_layer_status_init();
    *statp = ydb_write_layer_status;
}


static inline u_int32_t 
get_prelocked_flags(u_int32_t flags) {
    u_int32_t lock_flags = flags & (DB_PRELOCKED | DB_PRELOCKED_WRITE);
    return lock_flags;
}

// these next two static functions are defined
// both here and ydb.c. We should find a good
// place for them.
static int
ydb_getf_do_nothing(DBT const* UU(key), DBT const* UU(val), void* UU(extra)) {
    return 0;
}

// Check if the available file system space is less than the reserve
// Returns ENOSPC if not enough space, othersize 0
static inline int 
env_check_avail_fs_space(DB_ENV *env) {
    int r = env->i->fs_state == FS_RED ? ENOSPC : 0; 
    if (r) env->i->enospc_redzone_ctr++;
    return r;
}

// Return 0 if proposed pair do not violate size constraints of DB
// (insertion is legal)
// Return non zero otherwise.
static int
db_put_check_size_constraints(DB *db, const DBT *key, const DBT *val) {
    int r = 0;
    unsigned int klimit, vlimit;

    toku_brt_get_maximum_advised_key_value_lengths(&klimit, &vlimit);
    if (key->size > klimit) {
        r = toku_ydb_do_error(db->dbenv, EINVAL, 
                "The largest key allowed is %u bytes", klimit);
    } else if (val->size > vlimit) {
        r = toku_ydb_do_error(db->dbenv, EINVAL, 
                "The largest value allowed is %u bytes", vlimit);
    }
    return r;
}

//Return 0 if insert is legal
static int
db_put_check_overwrite_constraint(DB *db, DB_TXN *txn, DBT *key,
                                  u_int32_t lock_flags, u_int32_t overwrite_flag) {
    int r;

    if (overwrite_flag == 0) { // 0 (yesoverwrite) does not impose constraints.
        r = 0;
    } else if (overwrite_flag == DB_NOOVERWRITE) {
        // Check if (key,anything) exists in dictionary.
        // If exists, fail.  Otherwise, do insert.
        // The DB_RMW flag causes the cursor to grab a write lock instead of a read lock on the key if it exists.
        r = db_getf_set(db, txn, lock_flags|DB_SERIALIZABLE|DB_RMW, key, ydb_getf_do_nothing, NULL);
        if (r == DB_NOTFOUND) 
            r = 0;
        else if (r == 0)      
            r = DB_KEYEXIST;
        //Any other error is passed through.
    } else if (overwrite_flag == DB_NOOVERWRITE_NO_ERROR) {
        r = 0;
    } else {
        //Other flags are not (yet) supported.
        r = EINVAL;
    }
    return r;
}


int
toku_db_del(DB *db, DB_TXN *txn, DBT *key, u_int32_t flags, BOOL holds_ydb_lock) {
    HANDLE_PANICKED_DB(db);
    HANDLE_DB_ILLEGAL_WORKING_PARENT_TXN(db, txn);

    u_int32_t unchecked_flags = flags;
    //DB_DELETE_ANY means delete regardless of whether it exists in the db.
    BOOL error_if_missing = (BOOL)(!(flags&DB_DELETE_ANY));
    unchecked_flags &= ~DB_DELETE_ANY;
    u_int32_t lock_flags = get_prelocked_flags(flags);
    unchecked_flags &= ~lock_flags;
    BOOL do_locking = (BOOL)(db->i->lt && !(lock_flags&DB_PRELOCKED_WRITE));

    int r = 0;
    if (unchecked_flags!=0) {
        r = EINVAL;
    }

    if (r == 0 && error_if_missing) {
        //Check if the key exists in the db.
        r = db_getf_set(db, txn, lock_flags|DB_SERIALIZABLE|DB_RMW, key, ydb_getf_do_nothing, NULL);
    }
    if (r == 0 && do_locking) {
        //Do locking if necessary.
        r = get_point_write_lock(db, txn, key);
    }
    if (r == 0) {
        //Do the actual deleting.
        if (!holds_ydb_lock) toku_ydb_lock();
        r = toku_brt_delete(db->i->brt, key, txn ? db_txn_struct_i(txn)->tokutxn : 0);
        if (!holds_ydb_lock) toku_ydb_unlock();
    }

    if (r == 0) {
        STATUS_VALUE(YDB_LAYER_NUM_DELETES)++;  // accountability 
    }
    else {
        STATUS_VALUE(YDB_LAYER_NUM_DELETES_FAIL)++;  // accountability 
    }
    return r;
}


int
toku_db_put(DB *db, DB_TXN *txn, DBT *key, DBT *val, u_int32_t flags, BOOL holds_ydb_lock) {
    HANDLE_PANICKED_DB(db);
    HANDLE_DB_ILLEGAL_WORKING_PARENT_TXN(db, txn);
    int r = 0;

    u_int32_t lock_flags = get_prelocked_flags(flags);
    flags &= ~lock_flags;

    r = db_put_check_size_constraints(db, key, val);
    if (r == 0) {
        //Do any checking required by the flags.
        r = db_put_check_overwrite_constraint(db, txn, key, lock_flags, flags);
    }
    BOOL do_locking = (BOOL)(db->i->lt && !(lock_flags&DB_PRELOCKED_WRITE));
    if (r == 0 && do_locking) {
        //Do locking if necessary.
        r = get_point_write_lock(db, txn, key);
    }
    if (r == 0) {
        //Insert into the brt.
        TOKUTXN ttxn = txn ? db_txn_struct_i(txn)->tokutxn : NULL;
        enum brt_msg_type type = BRT_INSERT;
        if (flags==DB_NOOVERWRITE_NO_ERROR) {
            type = BRT_INSERT_NO_OVERWRITE;
        }
        if (!holds_ydb_lock) toku_ydb_lock();
        r = toku_brt_maybe_insert(db->i->brt, key, val, ttxn, FALSE, ZERO_LSN, TRUE, type);
        if (!holds_ydb_lock) toku_ydb_unlock();
    }

    if (r == 0) {
        // helgrind flags a race on this status update.  we increment it atomically to satisfy helgrind.
	// STATUS_VALUE(YDB_LAYER_NUM_INSERTS)++;  // accountability 
        (void) __sync_fetch_and_add(&STATUS_VALUE(YDB_LAYER_NUM_INSERTS), 1);
    } else {
	// STATUS_VALUE(YDB_LAYER_NUM_INSERTS_FAIL)++;  // accountability 
        (void) __sync_fetch_and_add(&STATUS_VALUE(YDB_LAYER_NUM_INSERTS_FAIL), 1);
    }

    return r;
}

static int
toku_db_update(DB *db, DB_TXN *txn,
               const DBT *key,
               const DBT *update_function_extra,
               u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    HANDLE_DB_ILLEGAL_WORKING_PARENT_TXN(db, txn);
    int r = 0;

    u_int32_t lock_flags = get_prelocked_flags(flags);
    flags &= ~lock_flags;

    r = db_put_check_size_constraints(db, key, update_function_extra);
    if (r != 0) { goto cleanup; }

    BOOL do_locking = (db->i->lt && !(lock_flags & DB_PRELOCKED_WRITE));
    if (do_locking) {
        r = get_point_write_lock(db, txn, key);
        if (r != 0) { goto cleanup; }
    }

    TOKUTXN ttxn = txn ? db_txn_struct_i(txn)->tokutxn : NULL;
    toku_ydb_lock();
    r = toku_brt_maybe_update(db->i->brt, key, update_function_extra, ttxn,
                              FALSE, ZERO_LSN, TRUE);
    toku_ydb_unlock();

cleanup:
    if (r == 0) 
	STATUS_VALUE(YDB_LAYER_NUM_UPDATES)++;  // accountability 
    else
	STATUS_VALUE(YDB_LAYER_NUM_UPDATES_FAIL)++;  // accountability 
    return r;
}


// DB_IS_RESETTING_OP is true if the dictionary should be considered as if created by this transaction.
// For example, it will be true if toku_db_update_broadcast() is used to implement a schema change (such
// as adding a column), and will be false if used simply to update all the rows of a table (such as 
// incrementing a field).
static int
toku_db_update_broadcast(DB *db, DB_TXN *txn,
                         const DBT *update_function_extra,
                         u_int32_t flags) {
    HANDLE_PANICKED_DB(db);
    HANDLE_DB_ILLEGAL_WORKING_PARENT_TXN(db, txn);
    int r = 0;

    u_int32_t lock_flags = get_prelocked_flags(flags);
    flags &= ~lock_flags;
    u_int32_t is_resetting_op_flag = flags & DB_IS_RESETTING_OP;
    flags &= is_resetting_op_flag;
    BOOL is_resetting_op = (is_resetting_op_flag != 0);
    

    if (is_resetting_op) {
        if (txn->parent != NULL) {
            r = EINVAL; // cannot have a parent if you are a resetting op
            goto cleanup;
        }
        r = toku_db_pre_acquire_fileops_lock(db, txn);
        if (r != 0) { goto cleanup; }
    }
    {
        DBT null_key;
        toku_init_dbt(&null_key);
        r = db_put_check_size_constraints(db, &null_key, update_function_extra);
        if (r != 0) { goto cleanup; }
    }

    BOOL do_locking = (db->i->lt && !(lock_flags & DB_PRELOCKED_WRITE));
    if (do_locking) {
        r = toku_db_pre_acquire_table_lock(db, txn);
        if (r != 0) { goto cleanup; }
    }

    TOKUTXN ttxn = txn ? db_txn_struct_i(txn)->tokutxn : NULL;
    toku_ydb_lock();
    r = toku_brt_maybe_update_broadcast(db->i->brt, update_function_extra, ttxn,
                                        FALSE, ZERO_LSN, TRUE, is_resetting_op);
    toku_ydb_unlock();

cleanup:
    if (r == 0) 
	STATUS_VALUE(YDB_LAYER_NUM_UPDATES_BROADCAST)++;  // accountability 
    else
	STATUS_VALUE(YDB_LAYER_NUM_UPDATES_BROADCAST_FAIL)++;  // accountability 
    return r;
}

static int
log_del_single(DB_TXN *txn, BRT brt, const DBT *key) {
    TOKUTXN ttxn = db_txn_struct_i(txn)->tokutxn;
    int r = toku_brt_log_del(ttxn, brt, key);
    return r;
}

static uint32_t
sum_size(uint32_t num_keys, DBT keys[], uint32_t overhead) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < num_keys; i++) 
        sum += keys[i].size + overhead;
    return sum;
}

static int
log_del_multiple(DB_TXN *txn, DB *src_db, const DBT *key, const DBT *val, uint32_t num_dbs, BRT brts[], DBT keys[]) {
    int r = 0;
    if (num_dbs > 0) {
        TOKUTXN ttxn = db_txn_struct_i(txn)->tokutxn;
        BRT src_brt  = src_db ? src_db->i->brt : NULL;
        uint32_t del_multiple_size = key->size + val->size + num_dbs*sizeof (uint32_t) + toku_log_enq_delete_multiple_overhead;
        uint32_t del_single_sizes = sum_size(num_dbs, keys, toku_log_enq_delete_any_overhead);
        if (del_single_sizes < del_multiple_size) {
            for (uint32_t i = 0; r == 0 && i < num_dbs; i++)
                r = log_del_single(txn, brts[i], &keys[i]);
        } else {
            r = toku_brt_log_del_multiple(ttxn, src_brt, brts, num_dbs, key, val);
        }
    }
    return r;
}

static uint32_t 
lookup_src_db(uint32_t num_dbs, DB *db_array[], DB *src_db) {
    uint32_t which_db;
    for (which_db = 0; which_db < num_dbs; which_db++) 
        if (db_array[which_db] == src_db)
            break;
    return which_db;
}

static int
do_del_multiple(DB_TXN *txn, uint32_t num_dbs, DB *db_array[], DBT keys[], DB *src_db, const DBT *src_key) {
    int r = 0;
    TOKUTXN ttxn = db_txn_struct_i(txn)->tokutxn;
    for (uint32_t which_db = 0; r == 0 && which_db < num_dbs; which_db++) {
        DB *db = db_array[which_db];

        // if db is being indexed by an indexer, then insert a delete message into the db if the src key is to the left or equal to the 
        // indexers cursor.  we have to get the src_db from the indexer and find it in the db_array.
	int do_delete = TRUE;
	DB_INDEXER *indexer = toku_db_get_indexer(db);
	if (indexer) { // if this db is the index under construction
            DB *indexer_src_db = toku_indexer_get_src_db(indexer);
            invariant(indexer_src_db != NULL);
            const DBT *indexer_src_key;
            if (src_db == indexer_src_db)
                indexer_src_key = src_key;
            else {
                uint32_t which_src_db = lookup_src_db(num_dbs, db_array, indexer_src_db);
                invariant(which_src_db < num_dbs);
                indexer_src_key = &keys[which_src_db];
            }
            do_delete = !toku_indexer_is_key_right_of_le_cursor(indexer, indexer_src_db, indexer_src_key);
        }
	if (r == 0 && do_delete) {
            r = toku_brt_maybe_delete(db->i->brt, &keys[which_db], ttxn, FALSE, ZERO_LSN, FALSE);
        }
    }
    return r;
}

int
env_del_multiple(
    DB_ENV *env, 
    DB *src_db, 
    DB_TXN *txn, 
    const DBT *src_key, 
    const DBT *src_val, 
    uint32_t num_dbs, 
    DB **db_array, 
    DBT *keys, 
    uint32_t *flags_array) 
{
    int r;
    DBT del_keys[num_dbs];

    HANDLE_PANICKED_ENV(env);

    if (!txn) {
        r = EINVAL;
        goto cleanup;
    }
    if (!env->i->generate_row_for_del) {
        r = EINVAL;
        goto cleanup;
    }

    HANDLE_ILLEGAL_WORKING_PARENT_TXN(env, txn);

    {
    uint32_t lock_flags[num_dbs];
    uint32_t remaining_flags[num_dbs];
    BRT brts[num_dbs];

    for (uint32_t which_db = 0; which_db < num_dbs; which_db++) {
        DB *db = db_array[which_db];
        lock_flags[which_db] = get_prelocked_flags(flags_array[which_db]);
        remaining_flags[which_db] = flags_array[which_db] & ~lock_flags[which_db];

        if (db == src_db) {
            del_keys[which_db] = *src_key;
        }
        else {
        //Generate the key
            r = env->i->generate_row_for_del(db, src_db, &keys[which_db], src_key, src_val);
            if (r != 0) goto cleanup;
            del_keys[which_db] = keys[which_db];
        }

        if (remaining_flags[which_db] & ~DB_DELETE_ANY) {
            r = EINVAL;
            goto cleanup;
        }
        BOOL error_if_missing = (BOOL)(!(remaining_flags[which_db]&DB_DELETE_ANY));
        if (error_if_missing) {
            //Check if the key exists in the db.
            r = db_getf_set(db, txn, lock_flags[which_db]|DB_SERIALIZABLE|DB_RMW, &del_keys[which_db], ydb_getf_do_nothing, NULL);
            if (r != 0) goto cleanup;
        }

        //Do locking if necessary.
        if (db->i->lt && !(lock_flags[which_db] & DB_PRELOCKED_WRITE)) {
            //Needs locking
            r = get_point_write_lock(db, txn, &del_keys[which_db]);
            if (r != 0) goto cleanup;
        }
        brts[which_db] = db->i->brt;
    }

    toku_ydb_lock();
    if (num_dbs == 1) {
        r = log_del_single(txn, brts[0], &del_keys[0]);
    }
    else {
        r = log_del_multiple(txn, src_db, src_key, src_val, num_dbs, brts, del_keys);
    }
    if (r == 0) 
        r = do_del_multiple(txn, num_dbs, db_array, del_keys, src_db, src_key);
    }
    toku_ydb_unlock();

cleanup:
    if (r == 0)
	STATUS_VALUE(YDB_LAYER_NUM_MULTI_DELETES) += num_dbs;  // accountability 
    else
	STATUS_VALUE(YDB_LAYER_NUM_MULTI_DELETES_FAIL) += num_dbs;  // accountability 
    return r;
}

static int
log_put_single(DB_TXN *txn, BRT brt, const DBT *key, const DBT *val) {
    TOKUTXN ttxn = db_txn_struct_i(txn)->tokutxn;
    int r = toku_brt_log_put(ttxn, brt, key, val);
    return r;
}

static int
log_put_multiple(DB_TXN *txn, DB *src_db, const DBT *src_key, const DBT *src_val, uint32_t num_dbs, BRT brts[]) {
    int r = 0;
    if (num_dbs > 0) {
        TOKUTXN ttxn = db_txn_struct_i(txn)->tokutxn;
        BRT src_brt  = src_db ? src_db->i->brt : NULL;
        r = toku_brt_log_put_multiple(ttxn, src_brt, brts, num_dbs, src_key, src_val);
    }
    return r;
}

static int
do_put_multiple(DB_TXN *txn, uint32_t num_dbs, DB *db_array[], DBT keys[], DBT vals[], DB *src_db, const DBT *src_key) {
    int r = 0;
    TOKUTXN ttxn = db_txn_struct_i(txn)->tokutxn;
    for (uint32_t which_db = 0; r == 0 && which_db < num_dbs; which_db++) {
        DB *db = db_array[which_db];

        // if db is being indexed by an indexer, then put into that db if the src key is to the left or equal to the 
        // indexers cursor.  we have to get the src_db from the indexer and find it in the db_array.
	int do_put = TRUE;
	DB_INDEXER *indexer = toku_db_get_indexer(db);
	if (indexer) { // if this db is the index under construction
            DB *indexer_src_db = toku_indexer_get_src_db(indexer);
            invariant(indexer_src_db != NULL);
            const DBT *indexer_src_key;
            if (src_db == indexer_src_db)
                indexer_src_key = src_key;
            else {
                uint32_t which_src_db = lookup_src_db(num_dbs, db_array, indexer_src_db);
                invariant(which_src_db < num_dbs);
                indexer_src_key = &keys[which_src_db];
            }
            do_put = !toku_indexer_is_key_right_of_le_cursor(indexer, indexer_src_db, indexer_src_key);
        }
        if (r == 0 && do_put) {
            r = toku_brt_maybe_insert(db->i->brt, &keys[which_db], &vals[which_db], ttxn, FALSE, ZERO_LSN, FALSE, BRT_INSERT);
        }
    }
    return r;
}

static int
env_put_multiple_internal(
    DB_ENV *env, 
    DB *src_db, 
    DB_TXN *txn, 
    const DBT *src_key, 
    const DBT *src_val, 
    uint32_t num_dbs, 
    DB **db_array, 
    DBT *keys, 
    DBT *vals, 
    uint32_t *flags_array) 
{
    int r;
    DBT put_keys[num_dbs];
    DBT put_vals[num_dbs];

    HANDLE_PANICKED_ENV(env);

    uint32_t lock_flags[num_dbs];
    uint32_t remaining_flags[num_dbs];
    BRT brts[num_dbs];

    if (!txn || !num_dbs) {
        r = EINVAL;
        goto cleanup;
    }
    if (!env->i->generate_row_for_put) {
        r = EINVAL;
        goto cleanup;
    }

    HANDLE_ILLEGAL_WORKING_PARENT_TXN(env, txn);

    for (uint32_t which_db = 0; which_db < num_dbs; which_db++) {
        DB *db = db_array[which_db];

        lock_flags[which_db] = get_prelocked_flags(flags_array[which_db]);
        remaining_flags[which_db] = flags_array[which_db] & ~lock_flags[which_db];

        //Generate the row
        if (db == src_db) {
            put_keys[which_db] = *src_key;
            put_vals[which_db] = *src_val;
        }
        else {
            r = env->i->generate_row_for_put(db, src_db, &keys[which_db], &vals[which_db], src_key, src_val);
            if (r != 0) goto cleanup;
            put_keys[which_db] = keys[which_db];
            put_vals[which_db] = vals[which_db];            
        }

        // check size constraints
        r = db_put_check_size_constraints(db, &put_keys[which_db], &put_vals[which_db]);
        if (r != 0) goto cleanup;

        //Check overwrite constraints
        r = db_put_check_overwrite_constraint(db, txn,
                                              &put_keys[which_db],
                                              lock_flags[which_db], remaining_flags[which_db]);
        if (r != 0) goto cleanup;
        if (remaining_flags[which_db] == DB_NOOVERWRITE_NO_ERROR) {
            //put_multiple does not support delaying the no error, since we would
            //have to log the flag in the put_multiple.
            r = EINVAL; goto cleanup;
        }

        //Do locking if necessary.
        if (db->i->lt && !(lock_flags[which_db] & DB_PRELOCKED_WRITE)) {
            //Needs locking
            r = get_point_write_lock(db, txn, &put_keys[which_db]);
            if (r != 0) goto cleanup;
        }
        brts[which_db] = db->i->brt;
    }
    
    toku_ydb_lock();
    if (num_dbs == 1) {
        r = log_put_single(txn, brts[0], &put_keys[0], &put_vals[0]);
    }
    else {
        r = log_put_multiple(txn, src_db, src_key, src_val, num_dbs, brts);
    }
    if (r == 0) {
        r = do_put_multiple(txn, num_dbs, db_array, put_keys, put_vals, src_db, src_key);
    }
    toku_ydb_unlock();

cleanup:
    if (r == 0)
	STATUS_VALUE(YDB_LAYER_NUM_MULTI_INSERTS) += num_dbs;  // accountability 
    else
	STATUS_VALUE(YDB_LAYER_NUM_MULTI_INSERTS_FAIL) += num_dbs;  // accountability 
    return r;
}

int
env_update_multiple(DB_ENV *env, DB *src_db, DB_TXN *txn,                                
                    DBT *old_src_key, DBT *old_src_data,
                    DBT *new_src_key, DBT *new_src_data,
                    uint32_t num_dbs, DB **db_array, uint32_t* flags_array, 
                    uint32_t num_keys, DBT keys[], 
                    uint32_t num_vals, DBT vals[]) {
    int r = 0;

    HANDLE_PANICKED_ENV(env);

    if (!txn) {
        r = EINVAL;
        goto cleanup;
    }
    if (!env->i->generate_row_for_put) {
        r = EINVAL;
        goto cleanup;
    }

    HANDLE_ILLEGAL_WORKING_PARENT_TXN(env, txn);

    {
        uint32_t n_del_dbs = 0;
        DB *del_dbs[num_dbs];
        BRT del_brts[num_dbs];
        DBT del_keys[num_dbs];
        
        uint32_t n_put_dbs = 0;
        DB *put_dbs[num_dbs];
        BRT put_brts[num_dbs];
        DBT put_keys[num_dbs];
        DBT put_vals[num_dbs];

        uint32_t lock_flags[num_dbs];
        uint32_t remaining_flags[num_dbs];

        for (uint32_t which_db = 0; which_db < num_dbs; which_db++) {
            DB *db = db_array[which_db];
            DBT curr_old_key, curr_new_key, curr_new_val;
            
            lock_flags[which_db] = get_prelocked_flags(flags_array[which_db]);
            remaining_flags[which_db] = flags_array[which_db] & ~lock_flags[which_db];

            // keys[0..num_dbs-1] are the new keys
            // keys[num_dbs..2*num_dbs-1] are the old keys
            // vals[0..num_dbs-1] are the new vals

            // Generate the old key and val
            if (which_db + num_dbs >= num_keys) {
                r = ENOMEM; goto cleanup;
            }
            if (db == src_db) {
                curr_old_key = *old_src_key;
            }
            else {
                r = env->i->generate_row_for_put(db, src_db, &keys[which_db + num_dbs], NULL, old_src_key, old_src_data);
                if (r != 0) goto cleanup;
                curr_old_key = keys[which_db + num_dbs];
            }
            // Generate the new key and val
            if (which_db >= num_keys || which_db >= num_vals) {
                r = ENOMEM; goto cleanup;
            }
            if (db == src_db) {
                curr_new_key = *new_src_key;
                curr_new_val = *new_src_data;
            }
            else {
                r = env->i->generate_row_for_put(db, src_db, &keys[which_db], &vals[which_db], new_src_key, new_src_data);
                if (r != 0) goto cleanup;
                curr_new_key = keys[which_db];
                curr_new_val = vals[which_db];
            }
            toku_dbt_cmp cmpfun = toku_db_get_compare_fun(db);
            BOOL key_eq = cmpfun(db, &curr_old_key, &curr_new_key) == 0;
            BOOL key_bytes_eq = (curr_old_key.size == curr_new_key.size && 
                                 (memcmp(curr_old_key.data, curr_new_key.data, curr_old_key.size) == 0)
                                 );
            if (!key_eq) {
                //Check overwrite constraints only in the case where 
                // the keys are not equal.
                // If the keys are equal, then we do not care of the flag is DB_NOOVERWRITE or 0
                r = db_put_check_overwrite_constraint(db, txn,
                                                      &curr_new_key,
                                                      lock_flags[which_db], remaining_flags[which_db]);
                if (r != 0) goto cleanup;
                if (remaining_flags[which_db] == DB_NOOVERWRITE_NO_ERROR) {
                    //update_multiple does not support delaying the no error, since we would
                    //have to log the flag in the put_multiple.
                    r = EINVAL; goto cleanup;
                }

                // lock old key
                if (db->i->lt && !(lock_flags[which_db] & DB_PRELOCKED_WRITE)) {
                    r = get_point_write_lock(db, txn, &curr_old_key);
                    if (r != 0) goto cleanup;
                }
                del_dbs[n_del_dbs] = db;
                del_brts[n_del_dbs] = db->i->brt;
                del_keys[n_del_dbs] = curr_old_key;
                n_del_dbs++;
                
            }

            // we take a shortcut and avoid generating the old val
            // we assume that any new vals with size > 0 are different than the old val
            // if (!key_eq || !(dbt_cmp(&vals[which_db], &vals[which_db + num_dbs]) == 0)) {
            if (!key_bytes_eq || curr_new_val.size > 0) {
                r = db_put_check_size_constraints(db, &curr_new_key, &curr_new_val);
                if (r != 0) goto cleanup;

                // lock new key
                if (db->i->lt) {
                    r = get_point_write_lock(db, txn, &curr_new_key);
                    if (r != 0) goto cleanup;
                }
                put_dbs[n_put_dbs] = db;
                put_brts[n_put_dbs] = db->i->brt;
                put_keys[n_put_dbs] = curr_new_key;
                put_vals[n_put_dbs] = curr_new_val;
                n_put_dbs++;
            }
        }
        // grab the ydb lock for the actual work that 
        // depends on it
        toku_ydb_lock();
        if (r == 0 && n_del_dbs > 0) {
            if (n_del_dbs == 1)
                r = log_del_single(txn, del_brts[0], &del_keys[0]);
            else
                r = log_del_multiple(txn, src_db, old_src_key, old_src_data, n_del_dbs, del_brts, del_keys);
            if (r == 0)
                r = do_del_multiple(txn, n_del_dbs, del_dbs, del_keys, src_db, old_src_key);
        }

        if (r == 0 && n_put_dbs > 0) {
            if (n_put_dbs == 1)
                r = log_put_single(txn, put_brts[0], &put_keys[0], &put_vals[0]);
            else
                r = log_put_multiple(txn, src_db, new_src_key, new_src_data, n_put_dbs, put_brts);
            if (r == 0)
                r = do_put_multiple(txn, n_put_dbs, put_dbs, put_keys, put_vals, src_db, new_src_key);
        }
        toku_ydb_unlock();
    }

cleanup:
    if (r == 0)
	STATUS_VALUE(YDB_LAYER_NUM_MULTI_UPDATES) += num_dbs;  // accountability 
    else
	STATUS_VALUE(YDB_LAYER_NUM_MULTI_UPDATES_FAIL) += num_dbs;  // accountability 
    return r;
}

int 
autotxn_db_del(DB* db, DB_TXN* txn, DBT* key, u_int32_t flags) {
    BOOL changed; int r;
    r = toku_db_construct_autotxn(db, &txn, &changed, FALSE, FALSE);
    if (r!=0) return r;
    r = toku_db_del(db, txn, key, flags, FALSE);
    return toku_db_destruct_autotxn(txn, r, changed, FALSE);
}

int 
autotxn_db_put(DB* db, DB_TXN* txn, DBT* key, DBT* data, u_int32_t flags) {
    //{ unsigned i; printf("put %p keylen=%d key={", db, key->size); for(i=0; i<key->size; i++) printf("%d,", ((char*)key->data)[i]); printf("} datalen=%d data={", data->size); for(i=0; i<data->size; i++) printf("%d,", ((char*)data->data)[i]); printf("}\n"); }
    BOOL changed; int r;
    r = env_check_avail_fs_space(db->dbenv);
    if (r != 0) { goto cleanup; }
    r = toku_db_construct_autotxn(db, &txn, &changed, FALSE, FALSE);
    if (r!=0) {
        goto cleanup;
    }
    r = toku_db_put(db, txn, key, data, flags, FALSE);
    r = toku_db_destruct_autotxn(txn, r, changed, FALSE);
cleanup:
    return r;
}

int
autotxn_db_update(DB *db, DB_TXN *txn,
                  const DBT *key,
                  const DBT *update_function_extra,
                  u_int32_t flags) {
    BOOL changed; int r;
    r = env_check_avail_fs_space(db->dbenv);
    if (r != 0) { goto cleanup; }
    r = toku_db_construct_autotxn(db, &txn, &changed, FALSE, FALSE);
    if (r != 0) { return r; }
    r = toku_db_update(db, txn, key, update_function_extra, flags);
    r = toku_db_destruct_autotxn(txn, r, changed, FALSE);
cleanup:
    return r;
}

int
autotxn_db_update_broadcast(DB *db, DB_TXN *txn,
                            const DBT *update_function_extra,
                            u_int32_t flags) {
    BOOL changed; int r;
    r = env_check_avail_fs_space(db->dbenv);
    if (r != 0) { goto cleanup; }
    r = toku_db_construct_autotxn(db, &txn, &changed, FALSE, FALSE);
    if (r != 0) { return r; }
    r = toku_db_update_broadcast(db, txn, update_function_extra, flags);
    r = toku_db_destruct_autotxn(txn, r, changed, FALSE);
cleanup:
    return r;
}

int
env_put_multiple(DB_ENV *env, DB *src_db, DB_TXN *txn, const DBT *src_key, const DBT *src_val, uint32_t num_dbs, DB **db_array, DBT *keys, DBT *vals, uint32_t *flags_array) {
    int r = env_check_avail_fs_space(env);
    if (r == 0) {
        r = env_put_multiple_internal(env, src_db, txn, src_key, src_val, num_dbs, db_array, keys, vals, flags_array);
    }
    return r;
}

int
toku_ydb_check_avail_fs_space(DB_ENV *env) {
    int rval = env_check_avail_fs_space(env);
    return rval;
}
#undef STATUS_VALUE

#include <valgrind/helgrind.h>
void __attribute__((constructor)) toku_ydb_write_helgrind_ignore(void);
void
toku_ydb_write_helgrind_ignore(void) {
    VALGRIND_HG_DISABLE_CHECKING(&ydb_write_layer_status, sizeof ydb_write_layer_status);
}
