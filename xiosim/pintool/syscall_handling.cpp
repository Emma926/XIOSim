#include <map>
#include <syscall.h>
#include <sys/mman.h>

#ifdef TIME_TRANSPARENCY
#include "rdtsc.h"
#endif

#include "boost_interprocess.h"

#include "feeder.h"
#include "multiprocess_shared.h"
#include "ipc_queues.h"
#include "BufferManagerProducer.h"
#include "speculation.h"

#include "syscall_handling.h"

// from linux/arch/x86/ia32/sys_ia32.c
struct mmap_arg_struct {
    ADDRINT addr;
    size_t len;
    int prot;
    int flags;
    int fd;
    off_t offset;
};

// from times.h
struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

#ifdef TIME_TRANSPARENCY
// Tracks the time we spend in simulation and tries to subtract it from timing
// calls
UINT64 sim_time = 0;
#endif

XIOSIM_LOCK syscall_lock;

VOID SyscallEntry(THREADID threadIndex, CONTEXT* ictxt, SYSCALL_STANDARD std, VOID* v);
VOID SyscallExit(THREADID threadIndex, CONTEXT* ictxt, SYSCALL_STANDARD std, VOID* v);

/* ========================================================================== */
VOID InitSyscallHandling() {
    PIN_AddSyscallEntryFunction(SyscallEntry, 0);
    PIN_AddSyscallExitFunction(SyscallExit, 0);
}

/* ========================================================================== */
VOID SyscallEntry(THREADID threadIndex, CONTEXT* ictxt, SYSCALL_STANDARD std, VOID* v) {
    /* Kill speculative feeder before reaching a syscall.
     * This guarantees speculative processes don't have side effects. */
    if (speculation_mode) {
        FinishSpeculation(get_tls(threadIndex));
        return;
    }

    lk_lock(&syscall_lock, threadIndex + 1);

    ADDRINT syscall_num = PIN_GetSyscallNumber(ictxt, std);
    ADDRINT arg1 = PIN_GetSyscallArgument(ictxt, std, 0);
    ADDRINT arg2;
    ADDRINT arg3;
    mmap_arg_struct mmap_arg;

    thread_state_t* tstate = get_tls(threadIndex);

    tstate->last_syscall_number = syscall_num;

    switch (syscall_num) {
    case __NR_brk:
#ifdef SYSCALL_DEBUG
        cerr << "Syscall brk(" << dec << syscall_num << ") addr: 0x" << hex << arg1 << dec << endl;
#endif
        tstate->last_syscall_arg1 = arg1;
        break;

    case __NR_munmap:
        arg2 = PIN_GetSyscallArgument(ictxt, std, 1);
#ifdef SYSCALL_DEBUG
        cerr << "Syscall munmap(" << dec << syscall_num << ") addr: 0x" << hex << arg1
             << " length: " << arg2 << dec << endl;
#endif
        tstate->last_syscall_arg1 = arg1;
        tstate->last_syscall_arg2 = arg2;
        break;

    case __NR_mmap:  // oldmmap
#ifndef _LP64
        memcpy(&mmap_arg, (void*)arg1, sizeof(mmap_arg_struct));
#else
        mmap_arg.addr = arg1;
        mmap_arg.len = PIN_GetSyscallArgument(ictxt, std, 1);
#endif
        tstate->last_syscall_arg1 = mmap_arg.len;
#ifdef SYSCALL_DEBUG
        cerr << "Syscall oldmmap(" << dec << syscall_num << ") addr: 0x" << hex << mmap_arg.addr
             << " length: " << mmap_arg.len << dec << endl;
#endif
        break;

#ifndef _LP64
    case __NR_mmap2: // ia32-only
        arg2 = PIN_GetSyscallArgument(ictxt, std, 1);
#ifdef SYSCALL_DEBUG
        cerr << "Syscall mmap2(" << dec << syscall_num << ") addr: 0x" << hex << arg1
             << " length: " << arg2 << dec << endl;
#endif
        tstate->last_syscall_arg1 = arg2;
        break;
#endif // _LP64

    case __NR_mremap:
        arg2 = PIN_GetSyscallArgument(ictxt, std, 1);
        arg3 = PIN_GetSyscallArgument(ictxt, std, 2);
#ifdef SYSCALL_DEBUG
        cerr << "Syscall mremap(" << dec << syscall_num << ") old_addr: 0x" << hex << arg1
             << " old_length: " << arg2 << " new_length: " << arg3 << dec << endl;
#endif
        tstate->last_syscall_arg1 = arg1;
        tstate->last_syscall_arg2 = arg2;
        tstate->last_syscall_arg3 = arg3;
        break;

#ifdef TIME_TRANSPARENCY
    case __NR_times:
#ifdef SYSCALL_DEBUG
        cerr << "Syscall times(" << dec << syscall_num << ") num_ins: " << SimOrgInsCount << endl;
#endif
        tstate->last_syscall_arg1 = arg1;
        break;
#endif
    case __NR_mprotect:
        arg2 = PIN_GetSyscallArgument(ictxt, std, 1);
        arg3 = PIN_GetSyscallArgument(ictxt, std, 2);
#ifdef SYSCALL_DEBUG
        cerr << "Syscall mprotect(" << dec << syscall_num << ") addr: " << hex << arg1 << dec
             << " length: " << arg2 << " prot: " << hex << arg3 << dec << endl;
#endif
        tstate->last_syscall_arg1 = arg1;
        tstate->last_syscall_arg2 = arg2;
        tstate->last_syscall_arg3 = arg3;
        break;

#ifdef SYSCALL_DEBUG
    case __NR_open:
        cerr << "Syscall open (" << dec << syscall_num << ") path: " << (char*)arg1 << endl;
        break;
#endif

#ifdef SYSCALL_DEBUG
    case __NR_exit:
        cerr << "Syscall exit (" << dec << syscall_num << ") code: " << arg1 << endl;
        break;
#endif

    /*
        case __NR_sysconf:
    #ifdef SYSCALL_DEBUG
            cerr << "Syscall sysconf (" << dec << syscall_num << ") arg: " << arg1
    << endl;
    #endif
            tstate->last_syscall_arg1 = arg1;
            break;
    */
    default:
#ifdef SYSCALL_DEBUG
        cerr << "Syscall " << dec << syscall_num << endl;
#endif
        break;
    }
    lk_unlock(&syscall_lock);
}

/* ========================================================================== */
VOID SyscallExit(THREADID threadIndex, CONTEXT* ictxt, SYSCALL_STANDARD std, VOID* v) {
    lk_lock(&syscall_lock, threadIndex + 1);
    ADDRINT retval = PIN_GetSyscallReturn(ictxt, std);
    ipc_message_t msg;

    thread_state_t* tstate = get_tls(threadIndex);

#ifdef TIME_TRANSPARENCY
    // for times()
    tms* buf;
    clock_t adj_time;
#endif

    switch (tstate->last_syscall_number) {
    case __NR_brk:
#ifdef SYSCALL_DEBUG
        cerr << "Ret syscall brk(" << dec << tstate->last_syscall_number << ") addr: 0x" << hex
             << retval << dec << endl;
#endif
        if (tstate->last_syscall_arg1 != 0)
            msg.UpdateBrk(asid, tstate->last_syscall_arg1, true);
        /* Seemingly libc code calls sbrk(0) to get the initial value of the sbrk.
         * We intercept that and send result to zesto, so that we can correclty deal
         * with virtual memory. */
        else
            msg.UpdateBrk(asid, retval, false);
        SendIPCMessage(msg);
        break;

    case __NR_munmap:
#ifdef SYSCALL_DEBUG
        cerr << "Ret syscall munmap(" << dec << tstate->last_syscall_number << ") addr: 0x" << hex
             << tstate->last_syscall_arg1 << " length: " << tstate->last_syscall_arg2 << dec
             << endl;
#endif
        if (retval != (ADDRINT)-1) {
            msg.Munmap(asid, tstate->last_syscall_arg1, tstate->last_syscall_arg2, false);
            SendIPCMessage(msg);
        }
        break;

    case __NR_mmap:  // oldmap
#ifdef SYSCALL_DEBUG
        cerr << "Ret syscall oldmmap(" << dec << tstate->last_syscall_number << ") addr: 0x" << hex
             << retval << " length: " << tstate->last_syscall_arg1 << dec << endl;
#endif
        if (retval != (ADDRINT)-1) {
            msg.Mmap(asid, retval, tstate->last_syscall_arg1, false);
            SendIPCMessage(msg);
        }
        break;

#ifndef _LP64
    case __NR_mmap2: // ia32-only
#ifdef SYSCALL_DEBUG
        cerr << "Ret syscall mmap2(" << dec << tstate->last_syscall_number << ") addr: 0x" << hex
             << retval << " length: " << tstate->last_syscall_arg1 << dec << endl;
#endif
        if (retval != (ADDRINT)-1) {
            msg.Mmap(asid, retval, tstate->last_syscall_arg1, false);
            SendIPCMessage(msg);
        }
        break;
#endif // _LP64

    case __NR_mremap:
#ifdef SYSCALL_DEBUG
        cerr << "Ret syscall mremap(" << dec << tstate->last_syscall_number << ") " << hex
             << " old_addr: 0x" << tstate->last_syscall_arg1
             << " old_length: " << tstate->last_syscall_arg2 << " new address: 0x" << retval
             << " new_length: " << tstate->last_syscall_arg3 << dec << endl;
#endif
        if (retval != (ADDRINT)-1) {
            msg.Munmap(asid, tstate->last_syscall_arg1, tstate->last_syscall_arg2, false);
            SendIPCMessage(msg);
            msg.Mmap(asid, retval, tstate->last_syscall_arg3, false);
            SendIPCMessage(msg);
        }
        break;

    case __NR_mprotect:
        if (retval != (ADDRINT)-1) {
            if ((tstate->last_syscall_arg3 & PROT_READ) == 0)
                msg.Munmap(asid, tstate->last_syscall_arg1, tstate->last_syscall_arg2, false);
            else
                msg.Mmap(asid, tstate->last_syscall_arg1, tstate->last_syscall_arg2, false);
            SendIPCMessage(msg);
        }
        break;

/* Present ourself as if we have num_cores cores */
/*    case __NR_sysconf:
#ifdef SYSCALL_DEBUG
        cerr << "Syscall sysconf (" << dec << syscall_num << ") ret" << endl;
#endif
        if (tstate->last_syscall_arg1 == _SC_NPROCESSORS_ONLN)
            if ((INT32)retval != - 1) {
                PIN_SetContextReg(ictxt, REG_EAX, num_cores);
                PIN_ExecuteAt(ictxt);
            }
        break;*/

#ifdef TIME_TRANSPARENCY
    case __NR_times:
        buf = (tms*)tstate->last_syscall_arg1;
        adj_time = retval - (clock_t)sim_time;
#ifdef SYSCALL_DEBUG
        cerr << "Ret syscall times(" << dec << tstate->last_syscall_number << ") old: " << retval
             << " adjusted: " << adj_time << " user: " << buf->tms_utime
             << " user_adj: " << (buf->tms_utime - sim_time) << " system: " << buf->tms_stime
             << endl;
#endif
        /* Compensate for time we spent on simulation
         * Included for full transparency - some apps detect we are taking a long
         time
         * and do bad things like dropping frames
         * Since we have no decent way of measuring how much time the simulator
         spends in the OS
         * (other than calling times() for every instruction), we assume the
         simulator is ainly
          user code. XXX: how reasonable is this assmuption???
         */
        buf->tms_utime -= sim_time;
        /* buf->tms_stime -=  0.1 * sim_time; ?? */
        // Don't touch child process timing -- we don't support child processes
        // anyway

        // Adjust aggregate time passed by time spent in sim
        // Return value as 32-bit int in EAX
        if ((INT32)retval != -1)
            PIN_SetContextReg(ictxt, REG_EAX, adj_time);
        // XXX: To make this work, we need to use PIN_ExecuteAt()
        break;
#endif

    default:
        break;
    }
    lk_unlock(&syscall_lock);
}
