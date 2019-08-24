/*
 * This file is part of the LAIK library.
 * Copyright (c) 2017, 2018 Josef Weidendorfer <Josef.Weidendorfer@gmx.de>
 *
 * LAIK is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, version 3 or later.
 *
 * LAIK is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#ifdef USE_MPI

#include "laik-internal.h"
#include "laik-backend-mpi.h"

#include <assert.h>
#include <stdlib.h>
#include <mpi.h>
#include <mpi-ext.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

// forward decls, types/structs , global variables

static void laik_mpi_finalize(Laik_Instance*);
static void laik_mpi_prepare(Laik_ActionSeq*);
static void laik_mpi_cleanup(Laik_ActionSeq*);
static void laik_mpi_exec(Laik_ActionSeq* as);
static void laik_mpi_updateGroup(Laik_Group*);
static bool laik_mpi_log_action(Laik_Action* a);
static void laik_mpi_sync(Laik_KVStore* kvs);
static void laik_mpi_eliminate_nodes(Laik_Group* oldGroup, Laik_Group* newGroup, int* nodeStatuses);
static int laik_mpi_status_check(Laik_Group *group, int *nodeStatuses);

// C guarantees that unset function pointers are NULL
static Laik_Backend laik_backend_mpi = {
    .name        = "MPI (two-sided)",
    .finalize    = laik_mpi_finalize,
    .prepare     = laik_mpi_prepare,
    .cleanup     = laik_mpi_cleanup,
    .exec        = laik_mpi_exec,
    .updateGroup = laik_mpi_updateGroup,
    .log_action  = laik_mpi_log_action,
    .sync        = laik_mpi_sync,
    .eliminateNodes = laik_mpi_eliminate_nodes,
    .statusCheck = laik_mpi_status_check,
};

static Laik_Instance* mpi_instance = 0;

typedef struct {
    MPI_Comm comm;
    bool didInit;
} MPIData;

typedef struct {
    MPI_Comm comm;
} MPIGroupData;

//----------------------------------------------------------------
// MPI backend behavior configurable by environment variables

// LAIK_MPI_REDUCE: make use of MPI_(All)Reduce? Default: Yes
// If not, we do own algorithm with send/recv.
static int mpi_reduce = 1;

// LAIK_MPI_ASYNC: convert send/recv to isend/irecv? Default: Yes
static int mpi_async = 1;


//----------------------------------------------------------------
// buffer space for messages if packing/unpacking from/to not-1d layout
// is necessary
#define PACKBUFSIZE (10*1024*1024)
//#define PACKBUFSIZE (10*800)
static char packbuf[PACKBUFSIZE];


//----------------------------------------------------------------------------
// MPI-specific actions + transformation

#define LAIK_AT_MpiReq   (LAIK_AT_Backend + 0)
#define LAIK_AT_MpiIrecv (LAIK_AT_Backend + 1)
#define LAIK_AT_MpiIsend (LAIK_AT_Backend + 2)
#define LAIK_AT_MpiWait  (LAIK_AT_Backend + 3)

// action structs must be packed
#pragma pack(push,1)

// ReqBuf action: provide base address for MPI_Request array
// referenced in following IRecv/Wait actions via req_it operands
typedef struct {
    Laik_Action h;
    unsigned int count;
    MPI_Request* req;
} Laik_A_MpiReq;

// IRecv action
typedef struct {
    Laik_Action h;
    unsigned int count;
    int from_rank;
    int req_id;
    char* buf;
} Laik_A_MpiIrecv;

// ISend action
typedef struct {
    Laik_Action h;
    unsigned int count;
    int to_rank;
    int req_id;
    char* buf;
} Laik_A_MpiIsend;

#pragma pack(pop)

static
void laik_mpi_addMpiReq(Laik_ActionSeq* as, int round,
                        unsigned int count, MPI_Request* buf)
{
    Laik_A_MpiReq* a;
    a = (Laik_A_MpiReq*) laik_aseq_addAction(as, sizeof(*a),
                                             LAIK_AT_MpiReq, round, 0);
    a->count = count;
    a->req = buf;
}

static
void laik_mpi_addMpiIrecv(Laik_ActionSeq* as, int round,
                          char* toBuf, unsigned int count, int from, int req_id)
{
    Laik_A_MpiIrecv* a;
    a = (Laik_A_MpiIrecv*) laik_aseq_addAction(as, sizeof(*a),
                                               LAIK_AT_MpiIrecv, round, 0);
    a->buf = toBuf;
    a->count = count;
    a->from_rank = from;
    a->req_id = req_id;
}

static
void laik_mpi_addMpiIsend(Laik_ActionSeq* as, int round,
                          char* fromBuf, unsigned int count, int to, int req_id)
{
    Laik_A_MpiIsend* a;
    a = (Laik_A_MpiIsend*) laik_aseq_addAction(as, sizeof(*a),
                                               LAIK_AT_MpiIsend, round, 0);
    a->buf = fromBuf;
    a->count = count;
    a->to_rank = to;
    a->req_id = req_id;
}

// Wait action
typedef struct {
    Laik_Action h;
    int req_id;
} Laik_A_MpiWait;

static
void laik_mpi_addMpiWait(Laik_ActionSeq* as, int round, int req_id)
{
    Laik_A_MpiWait* a;
    a = (Laik_A_MpiWait*) laik_aseq_addAction(as, sizeof(*a),
                                              LAIK_AT_MpiWait, round, 0);
    a->req_id = req_id;
}

static
bool laik_mpi_log_action(Laik_Action* a)
{
    switch(a->type) {
    case LAIK_AT_MpiReq: {
        Laik_A_MpiReq* aa = (Laik_A_MpiReq*) a;
        laik_log_append("MPI-Req: count %d, req %p", aa->count, aa->req);
        break;
    }

    case LAIK_AT_MpiIsend: {
        Laik_A_MpiIsend* aa = (Laik_A_MpiIsend*) a;
        laik_log_append("MPI-ISend: from %p ==> T%d, count %d, reqid %d",
                        aa->buf, aa->to_rank, aa->count, aa->req_id);
        break;
    }

    case LAIK_AT_MpiIrecv: {
        Laik_A_MpiIrecv* aa = (Laik_A_MpiIrecv*) a;
        laik_log_append("MPI-IRecv: T%d ==> to %p, count %d, reqid %d",
                        aa->from_rank, aa->buf, aa->count, aa->req_id);
        break;
    }

    case LAIK_AT_MpiWait: {
        Laik_A_MpiWait* aa = (Laik_A_MpiWait*) a;
        laik_log_append("MPI-Wait: reqid %d", aa->req_id);
        break;
    }

    default:
        return false;
    }
    return true;
}

// transformation: split send/recv actions into isend/irecv + wait
// - replace send with isend and wait for completion at end
// - replace recv with irecv at begin and wait at original position
bool laik_mpi_asyncSendRecv(Laik_ActionSeq* as)
{
    // must not have new actions, we want to start a new build
    assert(as->newActionCount == 0);

    unsigned int count = 0;
    int maxround = 0;
    Laik_Action* a = as->action;
    for(unsigned int i = 0; i < as->actionCount; i++, a = nextAction(a)) {
        if (a->round > maxround) maxround = a->round;
        if ((a->type == LAIK_AT_BufRecv) || (a->type == LAIK_AT_BufSend))
            count++;
    }

    if (count == 0) return false;

    // add 2 new rounds: 0 and maxround+2
    // - round 0 gets MpiReq and all MpiIrecv actions
    // - round maxround+2 gets Waits from MpiISend actions

    MPI_Request* buf = malloc(count * sizeof(MPI_Request));
    laik_mpi_addMpiReq(as, 0, count, buf);

    int req_id = 0;
    a = as->action;
    for(unsigned int i = 0; i < as->actionCount; i++, a = nextAction(a)) {
        switch(a->type) {
        case LAIK_AT_BufSend: {
            Laik_A_BufSend* aa = (Laik_A_BufSend*) a;
            laik_mpi_addMpiIsend(as, a->round + 1,
                                 aa->buf, aa->count, aa->to_rank, req_id);
            laik_mpi_addMpiWait(as, maxround + 2, req_id);
            req_id++;
            break;
        }

        case LAIK_AT_BufRecv: {
            Laik_A_BufRecv* aa = (Laik_A_BufRecv*) a;
            laik_mpi_addMpiIrecv(as, 0,
                                 aa->buf, aa->count, aa->from_rank, req_id);
            laik_mpi_addMpiWait(as, a->round + 1, req_id);
            req_id++;
            break;
        }

        default:
            // all rounds up by one due to new round 0
            laik_aseq_add(a, as, a->round + 1);
            break;
        }
    }
    assert(count == (unsigned) req_id);

    laik_aseq_activateNewActions(as);
    return true;
}

//----------------------------------------------------------------------------
// error helpers

static
void laik_mpi_panic(int err)
{
    char str[MPI_MAX_ERROR_STRING];
    int len;

    assert(err != MPI_SUCCESS);

    if(laik_mpi_get_error_handler() != NULL) {

        laik_log(LAIK_LL_Info, "Error handler found, attempting to handle error.\n");
        if(MPI_Error_string(err, str, &len) != MPI_SUCCESS) {
            laik_log(LAIK_LL_Warning, "Unknown mini-MPI error!");
        } else {
            laik_log(LAIK_LL_Warning, "MPI error: %s", str);
        }
        laik_mpi_get_error_handler()(0);
        fprintf(stderr, "[LAIK MPI Backend] Error handler exited, attempting to continue\n");
        return;
    }

    if (MPI_Error_string(err, str, &len) != MPI_SUCCESS)
        laik_panic("MPI backend: Unknown MPI error!");
    else
        laik_log(LAIK_LL_Panic, "MPI backend: MPI error '%s'", str);
    exit(1);
}


//----------------------------------------------------------------------------
// backend interface implementation: initialization

Laik_Instance* laik_init_mpi(int* argc, char*** argv)
{
    if (mpi_instance) return mpi_instance;

    int err;

    MPIData* d = malloc(sizeof(MPIData));
    if (!d) {
        laik_panic("Out of memory allocating MPIData object");
        exit(1); // not actually needed, laik_panic never returns
    }
    d->didInit = false;

    MPIGroupData* gd = malloc(sizeof(MPIGroupData));
    if (!gd) {
        laik_panic("Out of memory allocating MPIGroupData object");
        exit(1); // not actually needed, laik_panic never returns
    }

    // eventually initialize MPI first before accessing MPI_COMM_WORLD
    if (argc) {
        err = MPI_Init(argc, argv);
        if (err != MPI_SUCCESS) laik_mpi_panic(err);
        d->didInit = true;
    }

    // create own communicator duplicating WORLD to
    // - not have to worry about conflicting use of MPI_COMM_WORLD by application
    // - install error handler which passes errors through - we want them
    MPI_Comm ownworld;
    err = MPI_Comm_dup(MPI_COMM_WORLD, &ownworld);
    if (err != MPI_SUCCESS) laik_mpi_panic(err);
    err = MPI_Comm_set_errhandler(ownworld, MPI_ERRORS_RETURN);
    if (err != MPI_SUCCESS) laik_mpi_panic(err);

    // now finish initilization of <gd>/<d>, as MPI_Init is run
    gd->comm = ownworld;
    d->comm = ownworld;

    int size, rank;
    err = MPI_Comm_size(d->comm, &size);
    if (err != MPI_SUCCESS) laik_mpi_panic(err);
    err = MPI_Comm_rank(d->comm, &rank);
    if (err != MPI_SUCCESS) laik_mpi_panic(err);

    // Get the name of the processor
    char processor_name[MPI_MAX_PROCESSOR_NAME];
    int name_len;
    err = MPI_Get_processor_name(processor_name, &name_len);
    if (err != MPI_SUCCESS) laik_mpi_panic(err);

    Laik_Instance* inst;
    inst = laik_new_instance(&laik_backend_mpi, size, rank,
                             processor_name, d, gd);

    sprintf(inst->guid, "%d", rank);

    laik_log(2, "MPI backend initialized (at %s:%d, rank %d/%d)\n",
             inst->mylocation, (int) getpid(),
             rank, size);

    // do own reduce algorithm?
    char* str = getenv("LAIK_MPI_REDUCE");
    if (str) mpi_reduce = atoi(str);

    // do async convertion?
    str = getenv("LAIK_MPI_ASYNC");
    if (str) mpi_async = atoi(str);

    mpi_instance = inst;
    return inst;
}

static
MPIData* mpiData(Laik_Instance* i)
{
    return (MPIData*) i->backend_data;
}

static
MPIGroupData* mpiGroupData(Laik_Group* g)
{
    return (MPIGroupData*) g->backend_data;
}

static
void laik_mpi_finalize(Laik_Instance* inst)
{
    assert(inst == mpi_instance);

    if (mpiData(mpi_instance)->didInit) {
        int err = MPI_Finalize();
        if (err != MPI_SUCCESS) laik_mpi_panic(err);
    }
}

// update backend specific data for group if needed
static
void laik_mpi_updateGroup(Laik_Group* g)
{
    // calculate MPI communicator for group <g>
    // TODO: only supports shrinking of parent for now
    assert(g->parent);
    assert(g->parent->size >= g->size);

    laik_log(1, "MPI backend updateGroup: parent %d (size %d, myid %d) "
             "=> group %d (size %d, myid %d)",
             g->parent->gid, g->parent->size, g->parent->myid,
             g->gid, g->size, g->myid);

    // only interesting if this task is still part of parent
    if (g->parent->myid < 0) return;

    MPIGroupData* gdParent = (MPIGroupData*) g->parent->backend_data;
    assert(gdParent);

    MPIGroupData* gd = (MPIGroupData*) g->backend_data;
    assert(gd == 0); // must not be updated yet
    gd = malloc(sizeof(MPIGroupData));
    if (!gd) {
        laik_panic("Out of memory allocating MPIGroupData object");
        exit(1); // not actually needed, laik_panic never returns
    }
    g->backend_data = gd;

    laik_log(1, "MPI Comm_split: old myid %d => new myid %d",
             g->parent->myid, g->fromParent[g->parent->myid]);

    int err = MPI_Comm_split(gdParent->comm, g->myid < 0 ? MPI_UNDEFINED : 0,
                             g->myid, &(gd->comm));
    if (err != MPI_SUCCESS) laik_mpi_panic(err);
}

static
MPI_Datatype getMPIDataType(Laik_Data* d)
{
    MPI_Datatype mpiDataType;
    if      (d->type == laik_Double) mpiDataType = MPI_DOUBLE;
    else if (d->type == laik_Float)  mpiDataType = MPI_FLOAT;
    else if (d->type == laik_Int64)  mpiDataType = MPI_INT64_T;
    else if (d->type == laik_Int32)  mpiDataType = MPI_INT32_T;
    else if (d->type == laik_Char)   mpiDataType = MPI_INT8_T;
    else if (d->type == laik_UInt64) mpiDataType = MPI_UINT64_T;
    else if (d->type == laik_UInt32) mpiDataType = MPI_UINT32_T;
    else if (d->type == laik_UChar)  mpiDataType = MPI_UINT8_T;
    else assert(0);

    return mpiDataType;
}

static
MPI_Op getMPIOp(Laik_ReductionOperation redOp)
{
    MPI_Op mpiRedOp;
    switch(redOp) {
    case LAIK_RO_Sum:  mpiRedOp = MPI_SUM; break;
    case LAIK_RO_Prod: mpiRedOp = MPI_PROD; break;
    case LAIK_RO_Min:  mpiRedOp = MPI_MIN; break;
    case LAIK_RO_Max:  mpiRedOp = MPI_MAX; break;
    case LAIK_RO_And:  mpiRedOp = MPI_LAND; break;
    case LAIK_RO_Or:   mpiRedOp = MPI_LOR; break;
    default: assert(0);
    }
    return mpiRedOp;
}

static
void laik_mpi_exec_packAndSend(Laik_Mapping* map, Laik_Slice* slc,
                               int to_rank, uint64_t slc_size,
                               MPI_Datatype dataType, int tag, MPI_Comm comm)
{
    Laik_Index idx = slc->from;
    int dims = slc->space->dims;
    unsigned int packed;
    uint64_t count = 0;
    while(1) {
        packed = (map->layout->pack)(map, slc, &idx,
                                     packbuf, PACKBUFSIZE);
        assert(packed > 0);
        int err = MPI_Send(packbuf, (int) packed,
                           dataType, to_rank, tag, comm);
        if (err != MPI_SUCCESS) laik_mpi_panic(err);

        count += packed;
        if (laik_index_isEqual(dims, &idx, &(slc->to))) break;
    }
    assert(count == slc_size);
}

static
void laik_mpi_exec_recvAndUnpack(Laik_Mapping* map, Laik_Slice* slc,
                                 int from_rank, uint64_t slc_size,
                                 int elemsize,
                                 MPI_Datatype dataType, int tag, MPI_Comm comm)
{
    MPI_Status st;
    Laik_Index idx = slc->from;
    int dims = slc->space->dims;
    int recvCount, unpacked;
    uint64_t count = 0;
    while(1) {
        int err = MPI_Recv(packbuf, PACKBUFSIZE / elemsize,
                           dataType, from_rank, tag, comm, &st);
        if (err != MPI_SUCCESS) laik_mpi_panic(err);
        err = MPI_Get_count(&st, dataType, &recvCount);
        if (err != MPI_SUCCESS) laik_mpi_panic(err);

        unpacked = (map->layout->unpack)(map, slc, &idx,
                                         packbuf, recvCount * elemsize);
        assert(recvCount == unpacked);
        count += unpacked;
        if (laik_index_isEqual(dims, &idx, &(slc->to))) break;
    }
    assert(count == slc_size);
}

static
void laik_mpi_exec_reduce(Laik_TransitionContext* tc, Laik_BackendAction* a,
                          MPI_Datatype dataType, MPI_Comm comm)
{
    assert(mpi_reduce > 0);

    MPI_Op mpiRedOp = getMPIOp(a->redOp);
    int rootTask = a->rank;
    int err;

    if (rootTask == -1) {
        if (a->fromBuf == a->toBuf) {
            laik_log(1, "      exec MPI_Allreduce in-place, count %d", a->count);
            err = MPI_Allreduce(MPI_IN_PLACE, a->toBuf, (int) a->count,
                                    dataType, mpiRedOp, comm);
        }
        else {
            laik_log(1, "      exec MPI_Allreduce, count %d", a->count);
            err = MPI_Allreduce(a->fromBuf, a->toBuf, (int) a->count,
                                dataType, mpiRedOp, comm);
        }
    }
    else {
        if ((a->fromBuf == a->toBuf) && (tc->transition->group->myid == rootTask)) {
            laik_log(1, "      exec MPI_Reduce in-place, count %d, root %d",
                     a->count, rootTask);
            err = MPI_Reduce(MPI_IN_PLACE, a->toBuf, (int) a->count,
                             dataType, mpiRedOp, rootTask, comm);
        }
        else {
            laik_log(1, "      exec MPI_Reduce, count %d, root %d", a->count, rootTask);
            err = MPI_Reduce(a->fromBuf, a->toBuf, (int) a->count,
                             dataType, mpiRedOp, rootTask, comm);
        }
    }
    if (err != MPI_SUCCESS) laik_mpi_panic(err);
}

// a naive, manual reduction using send/recv:
// one process is chosen to do the reduction: the smallest rank from processes
// which are interested in the result. All other processes with input
// send their data to him, he does the reduction, and sends to all processes
// interested in the result
static
void laik_mpi_exec_groupReduce(Laik_TransitionContext* tc,
                               Laik_BackendAction* a,
                               MPI_Datatype dataType, MPI_Comm comm)
{
    assert(a->h.type == LAIK_AT_GroupReduce);
    Laik_Transition* t = tc->transition;
    Laik_Data* data = tc->data;

    // do the manual reduction on smallest rank of output group
    int reduceTask = laik_trans_taskInGroup(t, a->outputGroup, 0);
    laik_log(1, "      exec reduce at T%d", reduceTask);

    int myid = t->group->myid;
    MPI_Status st;
    int count, err;

    if (myid != reduceTask) {
        // not the reduce task: eventually send input and recv result

        if (laik_trans_isInGroup(t, a->inputGroup, myid)) {
            laik_log(1, "        exec MPI_Send to T%d", reduceTask);
            err = MPI_Send(a->fromBuf, (int) a->count, dataType,
                           reduceTask, 1, comm);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
        }
        if (laik_trans_isInGroup(t, a->outputGroup, myid)) {
            laik_log(1, "        exec MPI_Recv from T%d", reduceTask);
            err = MPI_Recv(a->toBuf, (int) a->count, dataType,
                           reduceTask, 1, comm, &st);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            // check that we received the expected number of elements
            err = MPI_Get_count(&st, dataType, &count);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            assert((int)a->count == count);
        }
        return;
    }

    // we are the reduce task
    int inCount = laik_trans_groupCount(t, a->inputGroup);
    uint64_t byteCount = a->count * data->elemsize;
    bool inputFromMe = laik_trans_isInGroup(t, a->inputGroup, myid);

    // for direct execution: use global <packbuf> (size PACKBUFSIZE)
    // check that bufsize is enough. TODO: dynamically increase?
    int bufSize = (inCount - (inputFromMe ? 1:0)) * byteCount;
    assert(bufSize < PACKBUFSIZE);

    // collect values from tasks in input group
    int bufOff[32], off = 0;
    assert(inCount <= 32);

    // always put this task in front: we use toBuf to calculate
    // our results, but there may be input from us, which would
    // be overwritten if not starting with our input
    int ii = 0;
    if (inputFromMe) {
        ii++; // slot 0 reserved for this task (use a->fromBuf)
        bufOff[0] = 0;
    }
    for(int i = 0; i< inCount; i++) {
        int inTask = laik_trans_taskInGroup(t, a->inputGroup, i);
        if (inTask == myid) continue;

        laik_log(1, "        exec MPI_Recv from T%d (buf off %d, count %d)",
                 inTask, off, a->count);

        bufOff[ii++] = off;
        err = MPI_Recv(packbuf + off, (int) a->count, dataType,
                       inTask, 1, comm, &st);
        if (err != MPI_SUCCESS) laik_mpi_panic(err);
        // check that we received the expected number of elements
        err = MPI_Get_count(&st, dataType, &count);
        if (err != MPI_SUCCESS) laik_mpi_panic(err);
        assert((int)a->count == count);
        off += byteCount;
    }
    assert(ii == inCount);
    assert(off == bufSize);

    // do the reduction, put result back to my input buffer
    if (data->type->reduce) {
        // reduce with 0/1 inputs by setting input pointer to 0
        char* buf0 = inputFromMe ? a->fromBuf : (packbuf + bufOff[0]);
        (data->type->reduce)(a->toBuf,
                             (inCount < 1) ? 0 : buf0,
                             (inCount < 2) ? 0 : (packbuf + bufOff[1]),
                             a->count, a->redOp);
        for(int t = 2; t < inCount; t++)
            (data->type->reduce)(a->toBuf, a->toBuf, packbuf + bufOff[t],
                                 a->count, a->redOp);
    }
    else {
        laik_log(LAIK_LL_Panic,
                 "Need reduce function for type '%s'. Not set!",
                 data->type->name);
        assert(0);
    }

    // send result to tasks in output group
    int outCount = laik_trans_groupCount(t, a->outputGroup);
    for(int i = 0; i< outCount; i++) {
        int outTask = laik_trans_taskInGroup(t, a->outputGroup, i);
        if (outTask == myid) {
            // that's myself: nothing to do
            continue;
        }

        laik_log(1, "        exec MPI_Send result to T%d", outTask);
        err = MPI_Send(a->toBuf, (int) a->count, dataType, outTask, 1, comm);
        if (err != MPI_SUCCESS) laik_mpi_panic(err);
    }
}

static
void laik_mpi_exec(Laik_ActionSeq* as)
{
    if (as->actionCount == 0) {
        laik_log(1, "MPI backend exec: nothing to do\n");
        return;
    }

    if (as->backend == 0) {
        // no preparation: do minimal transformations, sorting send/recv
        laik_log(1, "MPI backend exec: prepare before exec\n");
        laik_log_ActionSeqIfChanged(true, as, "Original sequence");
        bool changed = laik_aseq_splitTransitionExecs(as);
        laik_log_ActionSeqIfChanged(changed, as, "After splitting texecs");
        changed = laik_aseq_flattenPacking(as);
        laik_log_ActionSeqIfChanged(changed, as, "After flattening");
        changed = laik_aseq_allocBuffer(as);
        laik_log_ActionSeqIfChanged(changed, as, "After buffer alloc");
        changed = laik_aseq_sort_2phases(as);
        laik_log_ActionSeqIfChanged(changed, as, "After sorting");

        int not_handled = laik_aseq_calc_stats(as);
        assert(not_handled == 0); // there should be no MPI-specific actions
    }

    if (laik_log_begin(1)) {
        laik_log_append("MPI backend exec:\n");
        laik_log_ActionSeq(as, false);
        laik_log_flush(0);
    }

    // TODO: use transition context given by each action
    Laik_TransitionContext* tc = as->context[0];
    Laik_MappingList* fromList = tc->fromList;
    Laik_MappingList* toList = tc->toList;
    int elemsize = tc->data->elemsize;

    // common for all MPI calls: tag, comm, datatype
    int tag = 1;
    MPIGroupData* gd = mpiGroupData(tc->transition->group);
    assert(gd);
    MPI_Comm comm = gd->comm;
    MPI_Datatype dataType = getMPIDataType(tc->data);
    MPI_Status st;
    int err, count;

    // MPI_Request array: not set yet
    int req_count = 0;
    MPI_Request* req = 0;

    Laik_Action* a = as->action;
    for(unsigned int i = 0; i < as->actionCount; i++, a = nextAction(a)) {
        Laik_BackendAction* ba = (Laik_BackendAction*) a;
        if (laik_log_begin(1)) {
            laik_log_Action(a, as);
            laik_log_flush(0);
        }

        switch(a->type) {
        case LAIK_AT_BufReserve:
        case LAIK_AT_Nop:
            // no need to do anything
            break;

        case LAIK_AT_MpiReq: {
            // MPI-specific action: setup MPI_Request array
            Laik_A_MpiReq* aa = (Laik_A_MpiReq*) a;
            assert(aa->req != 0);
            assert(aa->count > 0);
            req_count = aa->count;
            req = aa->req;
            break;
        }

        case LAIK_AT_MpiIsend: {
            // MPI-specific action: call MPI_Isend
            Laik_A_MpiIsend* aa = (Laik_A_MpiIsend*) a;
            assert(aa->req_id < req_count);
            err = MPI_Isend(aa->buf, aa->count,
                            dataType, aa->to_rank, tag, comm, req + aa->req_id);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            break;
        }

        case LAIK_AT_MpiIrecv: {
            // MPI-specific action: exec MPI_IRecv
            Laik_A_MpiIrecv* aa = (Laik_A_MpiIrecv*) a;
            assert(aa->req_id < req_count);
            err = MPI_Irecv(aa->buf, aa->count,
                            dataType, aa->from_rank, tag, comm, req + aa->req_id);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            break;
        }

        case LAIK_AT_MpiWait: {
            // MPI-specific action: wait for request
            Laik_A_MpiWait* aa = (Laik_A_MpiWait*) a;
            assert(aa->req_id < req_count);
            err = MPI_Wait(req + aa->req_id, &st);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            break;
        }

        case LAIK_AT_MapSend: {
            assert(ba->fromMapNo < fromList->count);
            Laik_Mapping* fromMap = &(fromList->map[ba->fromMapNo]);
            assert(fromMap->base != 0);
            err = MPI_Send(fromMap->base + ba->offset, ba->count,
                           dataType, ba->rank, tag, comm);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            break;
        }

        case LAIK_AT_RBufSend: {
            Laik_A_RBufSend* aa = (Laik_A_RBufSend*) a;
            assert(aa->bufID < ASEQ_BUFFER_MAX);
            err = MPI_Send(as->buf[aa->bufID] + aa->offset, aa->count,
                           dataType, aa->to_rank, tag, comm);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            break;
        }

        case LAIK_AT_BufSend: {
            Laik_A_BufSend* aa = (Laik_A_BufSend*) a;
            err = MPI_Send(aa->buf, aa->count,
                           dataType, aa->to_rank, tag, comm);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            break;
        }

        case LAIK_AT_MapRecv: {
            assert(ba->toMapNo < toList->count);
            Laik_Mapping* toMap = &(toList->map[ba->toMapNo]);
            assert(toMap->base != 0);
            err = MPI_Recv(toMap->base + ba->offset, ba->count,
                           dataType, ba->rank, tag, comm, &st);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);

            // check that we received the expected number of elements
            err = MPI_Get_count(&st, dataType, &count);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            assert((int)ba->count == count);
            break;
        }

        case LAIK_AT_RBufRecv: {
            Laik_A_RBufRecv* aa = (Laik_A_RBufRecv*) a;
            assert(aa->bufID < ASEQ_BUFFER_MAX);
            err = MPI_Recv(as->buf[aa->bufID] + aa->offset, aa->count,
                           dataType, aa->from_rank, tag, comm, &st);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);

            // check that we received the expected number of elements
            err = MPI_Get_count(&st, dataType, &count);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            assert((int)ba->count == count);
            break;
        }

        case LAIK_AT_BufRecv: {
            Laik_A_BufRecv* aa = (Laik_A_BufRecv*) a;
            err = MPI_Recv(aa->buf, aa->count,
                           dataType, aa->from_rank, tag, comm, &st);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);

            // check that we received the expected number of elements
            err = MPI_Get_count(&st, dataType, &count);
            if (err != MPI_SUCCESS) laik_mpi_panic(err);
            assert((int)ba->count == count);
            break;
        }

        case LAIK_AT_CopyFromBuf:
            for(unsigned int i = 0; i < ba->count; i++)
                memcpy(ba->ce[i].ptr,
                       ba->fromBuf + ba->ce[i].offset,
                       ba->ce[i].bytes);
            break;

        case LAIK_AT_CopyToBuf:
            for(unsigned int i = 0; i < ba->count; i++)
                memcpy(ba->toBuf + ba->ce[i].offset,
                       ba->ce[i].ptr,
                       ba->ce[i].bytes);
            break;

        case LAIK_AT_PackToBuf:
            laik_exec_pack(ba, ba->map);
            break;

        case LAIK_AT_MapPackToBuf: {
            assert(ba->fromMapNo < fromList->count);
            Laik_Mapping* fromMap = &(fromList->map[ba->fromMapNo]);
            assert(fromMap->base != 0);
            laik_exec_pack(ba, fromMap);
            break;
        }

        case LAIK_AT_UnpackFromBuf:
            laik_exec_unpack(ba, ba->map);
            break;

        case LAIK_AT_MapUnpackFromBuf: {
            assert(ba->toMapNo < toList->count);
            Laik_Mapping* toMap = &(toList->map[ba->toMapNo]);
            assert(toMap->base);
            laik_exec_unpack(ba, toMap);
            break;
        }


        case LAIK_AT_MapPackAndSend: {
            Laik_A_MapPackAndSend* aa = (Laik_A_MapPackAndSend*) a;
            assert(aa->fromMapNo < fromList->count);
            Laik_Mapping* fromMap = &(fromList->map[aa->fromMapNo]);
            assert(fromMap->base != 0);
            laik_mpi_exec_packAndSend(fromMap, aa->slc, aa->to_rank, aa->count,
                                      dataType, tag, comm);
            break;
        }

        case LAIK_AT_PackAndSend:
            laik_mpi_exec_packAndSend(ba->map, ba->slc, ba->rank,
                                      (uint64_t) ba->count,
                                      dataType, tag, comm);
            break;

        case LAIK_AT_MapRecvAndUnpack: {
            Laik_A_MapRecvAndUnpack* aa = (Laik_A_MapRecvAndUnpack*) a;
            assert(aa->toMapNo < toList->count);
            Laik_Mapping* toMap = &(toList->map[aa->toMapNo]);
            assert(toMap->base);
            laik_mpi_exec_recvAndUnpack(toMap, aa->slc, aa->from_rank, aa->count,
                                        elemsize, dataType, tag, comm);
            break;
        }

        case LAIK_AT_RecvAndUnpack:
            laik_mpi_exec_recvAndUnpack(ba->map, ba->slc, ba->rank,
                                        (uint64_t) ba->count,
                                        elemsize, dataType, tag, comm);
            break;

        case LAIK_AT_Reduce:
            laik_mpi_exec_reduce(tc, ba, dataType, comm);
            break;

        case LAIK_AT_GroupReduce:
            laik_mpi_exec_groupReduce(tc, ba, dataType, comm);
            break;

        case LAIK_AT_RBufLocalReduce:
            assert(ba->bufID < ASEQ_BUFFER_MAX);
            assert(ba->dtype->reduce != 0);
            (ba->dtype->reduce)(ba->toBuf, ba->toBuf, as->buf[ba->bufID] + ba->offset,
                               ba->count, ba->redOp);
            break;

        case LAIK_AT_RBufCopy:
            assert(ba->bufID < ASEQ_BUFFER_MAX);
            memcpy(ba->toBuf, as->buf[ba->bufID] + ba->offset, ba->count * elemsize);
            break;

        case LAIK_AT_BufCopy:
            memcpy(ba->toBuf, ba->fromBuf, ba->count * elemsize);
            break;

        case LAIK_AT_BufInit:
            assert(ba->dtype->init != 0);
            (ba->dtype->init)(ba->toBuf, ba->count, ba->redOp);
            break;

        default:
            laik_log(LAIK_LL_Panic, "mpi_exec: no idea how to exec action %d (%s)",
                     a->type, laik_at_str(a->type));
            assert(0);
        }
    }
    assert( ((char*)as->action) + as->bytesUsed == ((char*)a) );
}


// calc statistics updates for MPI-specific actions
static
void laik_mpi_aseq_calc_stats(Laik_ActionSeq* as)
{
    unsigned int count;
    Laik_TransitionContext* tc = as->context[0];
    int current_tid = 0;
    Laik_Action* a = as->action;
    for(unsigned int i = 0; i < as->actionCount; i++, a = nextAction(a)) {
        assert(a->tid == current_tid); // TODO: only assumes actions from one transition
        switch(a->type) {
        case LAIK_AT_MpiIsend:
            count = ((Laik_A_MpiIsend*)a)->count;
            as->msgAsyncSendCount++;
            as->elemSendCount += count;
            as->byteSendCount += count * tc->data->elemsize;
            break;
        case LAIK_AT_MpiIrecv:
            count = ((Laik_A_MpiIrecv*)a)->count;
            as->msgAsyncRecvCount++;
            as->elemRecvCount += count;
            as->byteRecvCount += count * tc->data->elemsize;
            break;
        default: break;
        }
    }
}


static
void laik_mpi_prepare(Laik_ActionSeq* as)
{
    if (laik_log_begin(1)) {
        laik_log_append("MPI backend prepare:\n");
        laik_log_ActionSeq(as, false);
        laik_log_flush(0);
    }

    // mark as prepared by MPI backend: for MPI-specific cleanup + action logging
    as->backend = &laik_backend_mpi;

    bool changed = laik_aseq_splitTransitionExecs(as);
    laik_log_ActionSeqIfChanged(changed, as, "After splitting transition execs");
    if (as->actionCount == 0) {
        laik_aseq_calc_stats(as);
        return;
    }

    changed = laik_aseq_flattenPacking(as);
    laik_log_ActionSeqIfChanged(changed, as, "After flattening actions");

    if (mpi_reduce) {
        // detect group reduce actions which can be replaced by all-reduce
        // can be prohibited by setting LAIK_MPI_REDUCE=0
        changed = laik_aseq_replaceWithAllReduce(as);
        laik_log_ActionSeqIfChanged(changed, as, "After all-reduce detection");
    }

    changed = laik_aseq_combineActions(as);
    laik_log_ActionSeqIfChanged(changed, as, "After combining actions 1");

    changed = laik_aseq_allocBuffer(as);
    laik_log_ActionSeqIfChanged(changed, as, "After buffer allocation 1");

    changed = laik_aseq_splitReduce(as);
    laik_log_ActionSeqIfChanged(changed, as, "After splitting reduce actions");

    changed = laik_aseq_allocBuffer(as);
    laik_log_ActionSeqIfChanged(changed, as, "After buffer allocation 2");

    changed = laik_aseq_sort_rounds(as);
    laik_log_ActionSeqIfChanged(changed, as, "After sorting rounds");

    changed = laik_aseq_combineActions(as);
    laik_log_ActionSeqIfChanged(changed, as, "After combining actions 2");

    changed = laik_aseq_allocBuffer(as);
    laik_log_ActionSeqIfChanged(changed, as, "After buffer allocation 3");

    changed = laik_aseq_sort_2phases(as);
    //changed = laik_aseq_sort_rankdigits(as);
    laik_log_ActionSeqIfChanged(changed, as, "After sorting for deadlock avoidance");

    if (mpi_async) {
        changed = laik_mpi_asyncSendRecv(as);
        laik_log_ActionSeqIfChanged(changed, as, "After makeing send/recv async");

        changed = laik_aseq_sort_rounds(as);
        laik_log_ActionSeqIfChanged(changed, as, "After sorting rounds 2");
    }
    laik_aseq_freeTempSpace(as);

    laik_aseq_calc_stats(as);
    laik_mpi_aseq_calc_stats(as);
}

static void laik_mpi_cleanup(Laik_ActionSeq* as)
{
    if (laik_log_begin(1)) {
        laik_log_append("MPI backend cleanup:\n");
        laik_log_ActionSeq(as, false);
        laik_log_flush(0);
    }

    assert(as->backend == &laik_backend_mpi);

    if ((as->actionCount > 0) && (as->action->type == LAIK_AT_MpiReq)) {
        Laik_A_MpiReq* aa = (Laik_A_MpiReq*) as->action;
        free(aa->req);
        laik_log(1, "  freed MPI_Request array with %d entries", aa->count);
    }
}


//----------------------------------------------------------------------------
// KV store

// helper struct for first step in sync
typedef struct _CountEntry {
    unsigned int offCount;
    unsigned int dataCount;
} CountEntry;
static CountEntry* cespace = 0; // size array of local data for each MPI rank
static unsigned int cespace_size = 0;

static void laik_mpi_sync(Laik_KVStore* kvs)
{
    assert(kvs->inst == mpi_instance);
    MPI_Comm comm = mpiData(mpi_instance)->comm;

    // step 1:
    // all-to-all exchange number of updates from each process

    unsigned int count = (unsigned int) kvs->inst->size;
    if (cespace == 0) {
        cespace = (CountEntry*) malloc(count * sizeof(CountEntry));
        cespace_size = count;
    }
    else
        assert(cespace_size >= count);

    CountEntry se;
    if (kvs->myOffUsed > 0) {
        se.offCount = kvs->myOffUsed + 1;
        assert(kvs->myDataUsed > 0);
        se.dataCount = kvs->myDataUsed;
    }
    else {
        se.offCount = 0;
        se.dataCount = 0;
    }
    MPI_Allgather(&se, 2, MPI_INTEGER, cespace, 2, MPI_INTEGER, comm);

    // (2) allocate space (maximum of what each rank announces)
    unsigned int maxOffCount = 0, maxDataCount = 0;
    unsigned int i, offCount = 0;
    int myid = kvs->inst->myid;
    for(i = 0; i < count; i++) {
        if (myid == (int) i) continue;
        offCount += (cespace[i].offCount - 1);
        if (maxOffCount < cespace[i].offCount) maxOffCount = cespace[i].offCount;
        if (maxDataCount < cespace[i].dataCount) maxDataCount = cespace[i].dataCount;
    }

    laik_log(1, "MPI sync: getting %d entries (max %d from one)",
             offCount / 2, maxOffCount / 2);

    // nothing to exchange?
    if (maxOffCount == 0) {
        assert(maxDataCount == 0);
        return;
    }

    unsigned int* offArray = (unsigned int*) malloc(maxOffCount * sizeof(int));
    char* dataArray = (char*) malloc(maxDataCount);

    // (3) rank-by-rank: if non-zero-length data is to be send, broadcast to all other,
    //     directly update local KV entries
    for(i = 0; i < count; i++) {
        CountEntry* ce = &(cespace[i]);
        if (ce->offCount == 0) continue;
        assert(ce->dataCount > 0);

        if (myid == (int) i) {
            // I am sender
            MPI_Bcast(kvs->myOff, (int) ce->offCount, MPI_INTEGER, (int) i, comm);
            MPI_Bcast(kvs->myData, (int) ce->dataCount, MPI_CHAR, (int) i, comm);
            // skip own entries
            continue;
        }
        else {
            // I am receiver: receive into off/data arrays
            MPI_Bcast(offArray, (int) ce->offCount, MPI_INTEGER, (int) i, comm);
            MPI_Bcast(dataArray, (int) ce->dataCount, MPI_CHAR, (int) i, comm);
        }

        offCount = ce->offCount;
        assert((offCount & 1) == 1); // must be odd number of entries
        offCount--;

        laik_log(1, "  got %d entries from T%d", offCount / 2, i);

        for(unsigned int j = 0; j < offCount; j += 2)
            laik_kvs_set(kvs, dataArray + offArray[j],
                         offArray[j+2] - offArray[j+1], // data size
                         dataArray + offArray[j+1]);
    }

    free(offArray);
    free(dataArray);
}


static void laik_mpi_eliminate_nodes(Laik_Group* oldGroup, Laik_Group* newGroup, int* nodeStatuses) {
    (void) oldGroup; (void)newGroup; (void)nodeStatuses;
    int err;
    MPI_Comm oldComm = ((MPIGroupData *) oldGroup->backend_data)->comm;

    // We still need the old communicator to recover the checkpoints, don't invalidate it.
//    err = MPIX_Comm_revoke(oldComm);
//    if(err != MPI_SUCCESS) {
//        laik_mpi_panic(err);
//    }

    MPIGroupData* gd = (MPIGroupData*) newGroup->backend_data;
    assert(gd == 0); // must not be updated yet
    gd = malloc(sizeof(MPIGroupData));
    if (!gd) {
        laik_panic("Out of memory allocating MPIGroupData object");
        exit(1); // not actually needed, laik_panic never returns
    }
    newGroup->backend_data = gd;

    // Was this assertion wrong?
//    assert(oldComm != NULL && gd->comm != NULL);
    assert(oldComm != NULL);
    err = MPIX_Comm_shrink(oldComm, &gd->comm);
    if(err != MPI_SUCCESS) {
        laik_mpi_panic(err);
    }
    assert(gd->comm != NULL);

    int newSize;
    MPI_Comm_size(gd->comm, &newSize);
    assert(newSize == newGroup->size);
}

static int laik_mpi_status_check(Laik_Group *group, int *nodeStatuses) {
    laik_log(LAIK_LL_Warning, "Starting agreement protocol\n");

//    // Make sure my position fits into the integer
//    assert((unsigned long) group->myid < sizeof(int) * 8);
//    int status = 1U << group->myid;
//    MPIX_Comm_agree(((MPIGroupData*)group->backend_data)->comm, &status);
//
//    int numFailed = 0;
//    for (int i = 0; i < group->size; ++i) {
//        bool hasFailed = (status & (1U << i)) > 0;
//        if(hasFailed) {
//            nodeStatuses[i] = LAIK_FT_NODE_FAULT;
//            numFailed++;
//        } else {
//            nodeStatuses[i] = LAIK_FT_NODE_OK;
//        }
//    }
//    return numFailed;


    MPI_Comm originalComm = ((MPIGroupData *) group->backend_data)->comm;
//    MPI_Comm testComm;
//    MPI_Group worldGroup, originalGroup, shrinkedGroup;
//
//    MPIX_Comm_shrink(originalComm, &testComm);
//
//    MPI_Comm_group(originalComm, &originalGroup);
//    MPI_Comm_group(testComm, &shrinkedGroup);
//    MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);
//
//    MPI_Group difference;
//
//    MPI_Group_difference(originalGroup, shrinkedGroup, &difference);
//
//    int n = -1;
//    MPI_Group_size(shrinkedGroup, &n);
//    int ranks[n];
//    int worldRanks[n];
//
//    laik_log(LAIK_LL_Warning, "Shrinked MPI_Group size is %i", n);
//
//    for (int i = 0; i < n; ++i) {
//        ranks[i] = i;
//    }
//
//    MPI_Group_translate_ranks(shrinkedGroup, n, ranks, worldGroup, worldRanks);
//
//    if(nodeStatuses != NULL) {
//        for (int i = 0; i < group->size; ++i) {
//            nodeStatuses[i] = LAIK_FT_NODE_FAULT;
//        }
//
//        for (int i = 0; i < n; ++i) {
//            nodeStatuses[worldRanks[i]] = LAIK_FT_NODE_OK;
//        }
//    }
//
//    MPI_Group_free(&originalGroup);
//    MPI_Group_free(&shrinkedGroup);
//    MPI_Group_free(&worldGroup);
//
//    MPI_Comm_free(&testComm);

//    return group->size - n;

    int result;
    int reduceFlag = 1;
    do {
        MPIX_Comm_failure_ack(originalComm);
        result = MPIX_Comm_agree(originalComm, &reduceFlag);
    } while (result != MPI_SUCCESS);

    MPI_Group failedGroup, worldGroup;
    MPIX_Comm_failure_get_acked(originalComm, &failedGroup);

    int n = -1;
    MPI_Group_size(failedGroup, &n);
    int ranks[n];
    int worldRanks[n];

    MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);

    laik_log(LAIK_LL_Warning, "Failed MPI_Group size is %i", n);

    for (int i = 0; i < n; ++i) {
        ranks[i] = i;
    }

    // WARNING: Different from above, this group contains only failed ones, not the survivors
    MPI_Group_translate_ranks(failedGroup, n, ranks, worldGroup, worldRanks);
    if (nodeStatuses != NULL) {
        for (int i = 0; i < group->size; ++i) {
            nodeStatuses[i] = LAIK_FT_NODE_OK;
        }

        for (int i = 0; i < n; ++i) {
            nodeStatuses[worldRanks[i]] = LAIK_FT_NODE_FAULT;
        }
    }

    MPI_Group_free(&failedGroup);
    MPI_Group_free(&worldGroup);

    return n;
}

LaikMPIErrorHandler abortErrorHandler;

void laik_mpi_set_error_handler(LaikMPIErrorHandler newErrorHandler) {
    abortErrorHandler = newErrorHandler;
}

LaikMPIErrorHandler laik_mpi_get_error_handler() {
    return abortErrorHandler;
}


#endif // USE_MPI
