/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
#ident "Copyright (c) 2007-2012 Tokutek Inc.  All rights reserved."
#include "includes.h"
#include "test.h"
#include "cachetable-test.h"

CACHETABLE ct;

//
// This test exposed a bug (#3970) caught only by Valgrind.
// freed memory was being accessed by toku_test_cachetable_unpin_and_remove
//
static void *run_end_chkpt(void *arg) {
    assert(arg == NULL);
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    int r = toku_cachetable_end_checkpoint(
        cp, 
        false, 
        NULL,
        NULL
        );
    assert(r==0);
    return arg;
}

static void
run_test (void) {
    const int test_limit = 12;
    int r;
    ct = NULL;
    r = toku_create_cachetable(&ct, test_limit, ZERO_LSN, NULL_LOGGER); assert(r == 0);
    char fname1[] = __SRCFILE__ "test1.dat";
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    create_dummy_functions(f1);

    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    void* v1;
    //void* v2;
    long s1;
    //long s2;
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), &v1, &s1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    toku_test_cachetable_unpin(
        f1, 
        make_blocknum(1), 
        toku_cachetable_hash(f1, make_blocknum(1)),
        CACHETABLE_DIRTY,
        make_pair_attr(8)
        );

    // now this should mark the pair for checkpoint
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    r = toku_cachetable_begin_checkpoint(cp);
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), &v1, &s1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);

    toku_pthread_t mytid;
    r = toku_pthread_create(&mytid, NULL, run_end_chkpt, NULL);
    assert(r==0);

    // give checkpoint thread a chance to start waiting on lock
    sleep(1);
    r = toku_test_cachetable_unpin_and_remove(f1, make_blocknum(1), NULL, NULL);
    assert(r==0);

    void* ret;
    r = toku_pthread_join(mytid, &ret);
    assert(r==0);
    
    toku_cachetable_verify(ct);
    r = toku_cachefile_close(&f1, 0, false, ZERO_LSN); assert(r == 0);
    r = toku_cachetable_close(&ct); lazy_assert_zero(r);
    
    
}

int
test_main(int argc, const char *argv[]) {
  default_parse_args(argc, argv);
  run_test();
  return 0;
}
