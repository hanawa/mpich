/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *
 *  (C) 2001 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include "mpiimpl.h"

/*
=== BEGIN_MPI_T_CVAR_INFO_BLOCK ===

categories :
   - name : COLLECTIVE
     description : A category for collective communication variables.

cvars:
   - name      : MPIR_CVAR_ALLTOALL_SHORT_MSG_SIZE
     category  : COLLECTIVE
     type      : int
     default   : 256
     class     : device
     verbosity : MPI_T_VERBOSITY_USER_BASIC
     scope     : MPI_T_SCOPE_ALL_EQ
     description : >-
       the short message algorithm will be used if the per-destination
       message size (sendcount*size(sendtype)) is <= this value
       (See also: MPIR_CVAR_ALLTOALL_MEDIUM_MSG_SIZE)

   - name      : MPIR_CVAR_ALLTOALL_MEDIUM_MSG_SIZE
     category  : COLLECTIVE
     type      : int
     default   : 32768
     class     : device
     verbosity : MPI_T_VERBOSITY_USER_BASIC
     scope     : MPI_T_SCOPE_ALL_EQ
     description : >-
       the medium message algorithm will be used if the per-destination
       message size (sendcount*size(sendtype)) is <= this value and
       larger than MPIR_CVAR_ALLTOALL_SHORT_MSG_SIZE
       (See also: MPIR_CVAR_ALLTOALL_SHORT_MSG_SIZE)

   - name      : MPIR_CVAR_ALLTOALL_THROTTLE
     category  : COLLECTIVE
     type      : int
     default   : 32
     class     : device
     verbosity : MPI_T_VERBOSITY_USER_BASIC
     scope     : MPI_T_SCOPE_ALL_EQ
     description : >-
       max no. of irecvs/isends posted at a time in some alltoall
       algorithms. Setting it to 0 causes all irecvs/isends to be
       posted at once

   - name        : MPIR_CVAR_ALLTOALL_ALGORITHM_INTRA
     category    : COLLECTIVE
     type        : string
     default     : auto
     class       : device
     verbosity   : MPI_T_VERBOSITY_USER_BASIC
     scope       : MPI_T_SCOPE_ALL_EQ
     description : >-
       Variable to select alltoall algorithm
       auto - Internal algorithm selection
       brucks - Force brucks algorithm
       pairwise - Force pairwise algorithm
       pairwise_sendrecv_replace - Force pairwise sendrecv replace algorithm
       scattered - Force scattered algorithm

   - name        : MPIR_CVAR_ALLTOALL_ALGORITHM_INTER
     category    : COLLECTIVE
     type        : string
     default     : auto
     class       : device
     verbosity   : MPI_T_VERBOSITY_USER_BASIC
     scope       : MPI_T_SCOPE_ALL_EQ
     description : >-
       Variable to select alltoall algorithm
       auto - Internal algorithm selection
       generic - Force generic algorithm

   - name        : MPIR_CVAR_ALLTOALL_DEVICE_COLLECTIVE
     category    : COLLECTIVE
     type        : boolean
     default     : true
     class       : device
     verbosity   : MPI_T_VERBOSITY_USER_BASIC
     scope       : MPI_T_SCOPE_ALL_EQ
     description : >-
       If set to true, MPI_Alltoall will allow the device to override the
       MPIR-level collective algorithms. The device still has the
       option to call the MPIR-level algorithms manually.
       If set to false, the device-level alltoall function will not be
       called.

=== END_MPI_T_CVAR_INFO_BLOCK ===
*/

/* -- Begin Profiling Symbol Block for routine MPI_Alltoall */
#if defined(HAVE_PRAGMA_WEAK)
#pragma weak MPI_Alltoall = PMPI_Alltoall
#elif defined(HAVE_PRAGMA_HP_SEC_DEF)
#pragma _HP_SECONDARY_DEF PMPI_Alltoall  MPI_Alltoall
#elif defined(HAVE_PRAGMA_CRI_DUP)
#pragma _CRI duplicate MPI_Alltoall as PMPI_Alltoall
#elif defined(HAVE_WEAK_ATTRIBUTE)
int MPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype, void *recvbuf,
                 int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
                 __attribute__((weak,alias("PMPI_Alltoall")));
#endif
/* -- End Profiling Symbol Block */

/* Define MPICH_MPI_FROM_PMPI if weak symbols are not supported to build
   the MPI routines */
#ifndef MPICH_MPI_FROM_PMPI
#undef MPI_Alltoall
#define MPI_Alltoall PMPI_Alltoall

/* This is the machine-independent implementation of alltoall. The algorithm is:
   
   Algorithm: MPI_Alltoall

   We use four algorithms for alltoall. For short messages and
   (comm_size >= 8), we use the algorithm by Jehoshua Bruck et al,
   IEEE TPDS, Nov. 1997. It is a store-and-forward algorithm that
   takes lgp steps. Because of the extra communication, the bandwidth
   requirement is (n/2).lgp.beta.

   Cost = lgp.alpha + (n/2).lgp.beta

   where n is the total amount of data a process needs to send to all
   other processes.

   For medium size messages and (short messages for comm_size < 8), we
   use an algorithm that posts all irecvs and isends and then does a
   waitall. We scatter the order of sources and destinations among the
   processes, so that all processes don't try to send/recv to/from the
   same process at the same time.

   *** Modification: We post only a small number of isends and irecvs 
   at a time and wait on them as suggested by Tony Ladd. ***
   *** See comments below about an additional modification that 
   we may want to consider ***

   For long messages and power-of-two number of processes, we use a
   pairwise exchange algorithm, which takes p-1 steps. We
   calculate the pairs by using an exclusive-or algorithm:
           for (i=1; i<comm_size; i++)
               dest = rank ^ i;
   This algorithm doesn't work if the number of processes is not a power of
   two. For a non-power-of-two number of processes, we use an
   algorithm in which, in step i, each process  receives from (rank-i)
   and sends to (rank+i). 

   Cost = (p-1).alpha + n.beta

   where n is the total amount of data a process needs to send to all
   other processes.

   Possible improvements: 

   End Algorithm: MPI_Alltoall
*/


/* not declared static because a machine-specific function may call this one in some cases */
#undef FUNCNAME
#define FUNCNAME MPIR_Alltoall_intra
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Alltoall_intra( 
    const void *sendbuf,
    int sendcount, 
    MPI_Datatype sendtype, 
    void *recvbuf, 
    int recvcount, 
    MPI_Datatype recvtype, 
    MPIR_Comm *comm_ptr,
    MPIR_Errflag_t *errflag )
{
    int comm_size;
    int mpi_errno=MPI_SUCCESS, nbytes;
    int mpi_errno_ret = MPI_SUCCESS;
    int sendtype_size;
    MPI_Datatype newtype = MPI_DATATYPE_NULL;

    if (recvcount == 0) return MPI_SUCCESS;

    comm_size = comm_ptr->local_size;

    MPIR_Datatype_get_size_macro(sendtype, sendtype_size);
    nbytes = sendtype_size * sendcount;

    if (sendbuf == MPI_IN_PLACE) {
        /* We use pair-wise sendrecv_replace in order to conserve memory usage,
         * which is keeping with the spirit of the MPI-2.2 Standard.  But
         * because of this approach all processes must agree on the global
         * schedule of sendrecv_replace operations to avoid deadlock.
         *
         * Note that this is not an especially efficient algorithm in terms of
         * time and there will be multiple repeated malloc/free's rather than
         * maintaining a single buffer across the whole loop.  Something like
         * MADRE is probably the best solution for the MPI_IN_PLACE scenario. */
        mpi_errno = MPIR_Alltoall_pairwise_sendrecv_replace (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm_ptr, errflag);
    }
    else if ((nbytes <= MPIR_CVAR_ALLTOALL_SHORT_MSG_SIZE) && (comm_size >= 8)) {

        /* use the indexing algorithm by Jehoshua Bruck et al,
         * IEEE TPDS, Nov. 97 */ 
        mpi_errno = MPIR_Alltoall_brucks (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm_ptr, errflag);

    }

    else if (nbytes <= MPIR_CVAR_ALLTOALL_MEDIUM_MSG_SIZE) {
        /* Medium-size message. Use isend/irecv with scattered
           destinations. Use Tony Ladd's modification to post only
           a small number of isends/irecvs at a time. */
        mpi_errno = MPIR_Alltoall_scattered (sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm_ptr, errflag);
    }

    else {
        /* Long message. If comm_size is a power-of-two, do a pairwise
           exchange using exclusive-or to create pairs. Else send to
           rank+i, receive from rank-i. */

        mpi_errno = MPIR_Alltoall_pairwise(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm_ptr, errflag);
    }
    if (mpi_errno) MPIR_ERR_POP(mpi_errno);

 fn_exit:
    if (mpi_errno_ret)
        mpi_errno = mpi_errno_ret;
    else if (*errflag != MPIR_ERR_NONE)
        MPIR_ERR_SET(mpi_errno, *errflag, "**coll_fail");

    return mpi_errno;
 fn_fail:
    if (newtype != MPI_DATATYPE_NULL)
        MPIR_Type_free_impl(&newtype);
    goto fn_exit;
}



/* not declared static because a machine-specific function may call this one in some cases */
#undef FUNCNAME
#define FUNCNAME MPIR_Alltoall_inter
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Alltoall_inter( 
    const void *sendbuf,
    int sendcount, 
    MPI_Datatype sendtype, 
    void *recvbuf, 
    int recvcount, 
    MPI_Datatype recvtype, 
    MPIR_Comm *comm_ptr,
    MPIR_Errflag_t *errflag )
{
    int mpi_errno = MPI_SUCCESS;

    mpi_errno = MPIR_Alltoall_generic_inter(sendbuf, sendcount, sendtype,
            recvbuf, recvcount, recvtype, comm_ptr, errflag);

    return mpi_errno;
}

#undef FUNCNAME
#define FUNCNAME MPIR_Alltoall
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
int MPIR_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPIR_Comm *comm_ptr, MPIR_Errflag_t *errflag)
{
    int mpi_errno = MPI_SUCCESS;
        
    if (comm_ptr->comm_kind == MPIR_COMM_KIND__INTRACOMM) {
        /* intracommunicator */
        switch (MPIR_Alltoall_alg_intra_choice) {
            case MPIR_ALLTOALL_ALG_INTRA_BRUCKS:
                mpi_errno = MPIR_Alltoall_brucks(sendbuf, sendcount, sendtype,
                                        recvbuf, recvcount, recvtype,
                                        comm_ptr, errflag);
                break;
            case MPIR_ALLTOALL_ALG_INTRA_PAIRWISE:
                mpi_errno = MPIR_Alltoall_pairwise(sendbuf, sendcount, sendtype,
                                        recvbuf, recvcount, recvtype,
                                        comm_ptr, errflag);
                break;
            case MPIR_ALLTOALL_ALG_INTRA_PAIRWISE_SENDRECV_REPLACE:
                mpi_errno = MPIR_Alltoall_pairwise_sendrecv_replace(sendbuf, sendcount,
                                        sendtype, recvbuf, recvcount, recvtype,
                                        comm_ptr, errflag);
                break;
            case MPIR_ALLTOALL_ALG_INTRA_SCATTERED:
                mpi_errno = MPIR_Alltoall_scattered(sendbuf, sendcount, sendtype,
                                        recvbuf, recvcount, recvtype,
                                        comm_ptr, errflag);
                break;
            case MPIR_ALLTOALL_ALG_INTRA_AUTO:
                MPL_FALLTHROUGH;
            default:
                mpi_errno = MPIR_Alltoall_intra(sendbuf, sendcount, sendtype,
                                        recvbuf, recvcount, recvtype,
                                        comm_ptr, errflag);
                break;
        }
    } else {
        /* intercommunicator */
        switch (MPIR_Alltoall_alg_inter_choice) {
            case MPIR_ALLTOALL_ALG_INTER_GENERIC:
                mpi_errno = MPIR_Alltoall_generic_inter(sendbuf, sendcount, sendtype,
                                        recvbuf, recvcount, recvtype,
                                        comm_ptr, errflag);
                break;
            case MPIR_ALLTOALL_ALG_INTER_AUTO:
                MPL_FALLTHROUGH;
            default:
                mpi_errno = MPIR_Alltoall_inter(sendbuf, sendcount, sendtype,
                                        recvbuf, recvcount, recvtype,
                                        comm_ptr, errflag);
                break;
        }
    }
    if (mpi_errno) MPIR_ERR_POP(mpi_errno);

 fn_exit:
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

#endif

#undef FUNCNAME
#define FUNCNAME MPI_Alltoall
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
/*@
MPI_Alltoall - Sends data from all to all processes

Input Parameters:
+ sendbuf - starting address of send buffer (choice) 
. sendcount - number of elements to send to each process (integer) 
. sendtype - data type of send buffer elements (handle) 
. recvcount - number of elements received from any process (integer) 
. recvtype - data type of receive buffer elements (handle) 
- comm - communicator (handle) 

Output Parameters:
. recvbuf - address of receive buffer (choice) 

.N ThreadSafe

.N Fortran

.N Errors
.N MPI_ERR_COMM
.N MPI_ERR_COUNT
.N MPI_ERR_TYPE
.N MPI_ERR_BUFFER
@*/
int MPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 MPI_Comm comm)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Comm *comm_ptr = NULL;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    MPIR_FUNC_TERSE_STATE_DECL(MPID_STATE_MPI_ALLTOALL);

    MPIR_ERRTEST_INITIALIZED_ORDIE();
    
    MPID_THREAD_CS_ENTER(GLOBAL, MPIR_THREAD_GLOBAL_ALLFUNC_MUTEX);
    MPIR_FUNC_TERSE_COLL_ENTER(MPID_STATE_MPI_ALLTOALL);

    /* Validate parameters, especially handles needing to be converted */
#   ifdef HAVE_ERROR_CHECKING
    {
        MPID_BEGIN_ERROR_CHECKS;
        {
	    MPIR_ERRTEST_COMM(comm, mpi_errno);
	}
        MPID_END_ERROR_CHECKS;
    }
#   endif /* HAVE_ERROR_CHECKING */

    /* Convert MPI object handles to object pointers */
    MPIR_Comm_get_ptr( comm, comm_ptr );

    /* Validate parameters and objects (post conversion) */
#   ifdef HAVE_ERROR_CHECKING
    {
        MPID_BEGIN_ERROR_CHECKS;
        {
	    MPIR_Datatype *sendtype_ptr=NULL, *recvtype_ptr=NULL;
	    
            MPIR_Comm_valid_ptr( comm_ptr, mpi_errno, FALSE );
            if (mpi_errno != MPI_SUCCESS) goto fn_fail;

            if (sendbuf != MPI_IN_PLACE) {
                MPIR_ERRTEST_COUNT(sendcount, mpi_errno);
                MPIR_ERRTEST_DATATYPE(sendtype, "sendtype", mpi_errno);
                if (HANDLE_GET_KIND(sendtype) != HANDLE_KIND_BUILTIN) {
                    MPIR_Datatype_get_ptr(sendtype, sendtype_ptr);
                    MPIR_Datatype_valid_ptr( sendtype_ptr, mpi_errno );
                    if (mpi_errno != MPI_SUCCESS) goto fn_fail;
                    MPIR_Datatype_committed_ptr( sendtype_ptr, mpi_errno );
                    if (mpi_errno != MPI_SUCCESS) goto fn_fail;
                }

                if (comm_ptr->comm_kind == MPIR_COMM_KIND__INTRACOMM &&
                        sendbuf != MPI_IN_PLACE &&
                        sendcount == recvcount &&
                        sendtype == recvtype &&
                        sendcount != 0)
                    MPIR_ERRTEST_ALIAS_COLL(sendbuf,recvbuf,mpi_errno);
            }

	    MPIR_ERRTEST_COUNT(recvcount, mpi_errno);
	    MPIR_ERRTEST_DATATYPE(recvtype, "recvtype", mpi_errno);
            if (HANDLE_GET_KIND(recvtype) != HANDLE_KIND_BUILTIN) {
                MPIR_Datatype_get_ptr(recvtype, recvtype_ptr);
                MPIR_Datatype_valid_ptr( recvtype_ptr, mpi_errno );
                if (mpi_errno != MPI_SUCCESS) goto fn_fail;
                MPIR_Datatype_committed_ptr( recvtype_ptr, mpi_errno );
                if (mpi_errno != MPI_SUCCESS) goto fn_fail;
            }

            if (comm_ptr->comm_kind == MPIR_COMM_KIND__INTERCOMM) {
                MPIR_ERRTEST_SENDBUF_INPLACE(sendbuf, sendcount, mpi_errno);
            }
            MPIR_ERRTEST_RECVBUF_INPLACE(recvbuf, recvcount, mpi_errno);
            MPIR_ERRTEST_USERBUFFER(sendbuf,sendcount,sendtype,mpi_errno);
	    MPIR_ERRTEST_USERBUFFER(recvbuf,recvcount,recvtype,mpi_errno);
        }
        MPID_END_ERROR_CHECKS;
    }
#   endif /* HAVE_ERROR_CHECKING */

    /* ... body of routine ...  */

    if (MPIR_CVAR_ALLTOALL_DEVICE_COLLECTIVE && MPIR_CVAR_DEVICE_COLLECTIVES) {
        mpi_errno = MPID_Alltoall(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                     comm_ptr, &errflag);
    } else {
        mpi_errno = MPIR_Alltoall(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype,
                     comm_ptr, &errflag);
    }
    if (mpi_errno) goto fn_fail;

    /* ... end of body of routine ... */
    
  fn_exit:
    MPIR_FUNC_TERSE_COLL_EXIT(MPID_STATE_MPI_ALLTOALL);
    MPID_THREAD_CS_EXIT(GLOBAL, MPIR_THREAD_GLOBAL_ALLFUNC_MUTEX);
    return mpi_errno;

  fn_fail:
    /* --BEGIN ERROR HANDLING-- */
#   ifdef HAVE_ERROR_CHECKING
    {
	mpi_errno = MPIR_Err_create_code(
	    mpi_errno, MPIR_ERR_RECOVERABLE, FCNAME, __LINE__, MPI_ERR_OTHER, "**mpi_alltoall",
	    "**mpi_alltoall %p %d %D %p %d %D %C", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm);
    }
#   endif
    mpi_errno = MPIR_Err_return_comm( comm_ptr, FCNAME, mpi_errno );
    goto fn_exit;
    /* --END ERROR HANDLING-- */
}