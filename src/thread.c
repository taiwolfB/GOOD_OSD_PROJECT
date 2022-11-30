﻿#include "HAL9000.h"
#include "thread_internal.h"
#include "synch.h"
#include "cpumu.h"
#include "ex_event.h"
#include "core.h"
#include "vmm.h"
#include "process_internal.h"
#include "isr.h"
#include "gdtmu.h"
#include "pe_exports.h"
//David:
// Added the necessary includes for the new functions
#include "smp.h"
#include "log.h"

#define TID_INCREMENT               4

#define THREAD_TIME_SLICE           1

extern void ThreadStart();

typedef
void
(__cdecl FUNC_ThreadSwitch)(
    OUT_PTR         PVOID*          OldStack,
    IN              PVOID           NewStack
    );

extern FUNC_ThreadSwitch            ThreadSwitch;

typedef struct _THREAD_SYSTEM_DATA
{
    LOCK                AllThreadsLock;

    _Guarded_by_(AllThreadsLock)
    LIST_ENTRY          AllThreadsList;

    LOCK                ReadyThreadsLock;

    _Guarded_by_(ReadyThreadsLock)
    LIST_ENTRY          ReadyThreadsList;
} THREAD_SYSTEM_DATA, *PTHREAD_SYSTEM_DATA;


static THREAD_SYSTEM_DATA m_threadSystemData;

// Bogdan: 
// A context used to propagate the thread's maximum priority from the 
// ThreadRecomputePriority to the subsequent functions and backwards.
typedef struct _PRIORITY_DONATION_CTX
{
    THREAD_PRIORITY maximumPriority;
} PRIORITY_DONATION_CTX, *PPRIORITY_DONATION_CTX;

__forceinline
static
TID
_ThreadSystemGetNextTid(
    void
    )
{
    static volatile TID __currentTid = 0;

    return _InterlockedExchangeAdd64(&__currentTid, TID_INCREMENT);
}

static
STATUS
_ThreadInit(
    IN_Z        char*               Name,
    IN          THREAD_PRIORITY     Priority,
    OUT_PTR     PTHREAD*            Thread,
    IN          BOOLEAN             AllocateKernelStack
    );

static
STATUS
_ThreadSetupInitialState(
    IN      PTHREAD             Thread,
    IN      PVOID               StartFunction,
    IN      QWORD               FirstArgument,
    IN      QWORD               SecondArgument,
    IN      BOOLEAN             KernelStack
    );

static
STATUS
_ThreadSetupMainThreadUserStack(
    IN      PVOID               InitialStack,
    OUT     PVOID*              ResultingStack,
    IN      PPROCESS            Process
    );


REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
RELEASES_EXCL_AND_NON_REENTRANT_LOCK(m_threadSystemData.ReadyThreadsLock)
static
void
_ThreadSchedule(
    void
    );

REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
RELEASES_EXCL_AND_NON_REENTRANT_LOCK(m_threadSystemData.ReadyThreadsLock)
void
ThreadCleanupPostSchedule(
    void
    );

REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
static
_Ret_notnull_
PTHREAD
_ThreadGetReadyThread(
    void
    );

static
void
_ThreadForcedExit(
    void
    );

static
void
_ThreadReference(
    INOUT   PTHREAD                 Thread
    );

static
void
_ThreadDereference(
    INOUT   PTHREAD                 Thread
    );

static FUNC_FreeFunction            _ThreadDestroy;

static
void
_ThreadKernelFunction(
    IN      PFUNC_ThreadStart       Function,
    IN_OPT  PVOID                   Context
    );

static FUNC_ThreadStart     _IdleThread;
//David:
// This function is used by the InsertOrderedList function to compare the priorities of two threads so that the items are inserted in order in the list
INT64
ThreadComparePriorityReadyList(IN PLIST_ENTRY e1, 
    IN PLIST_ENTRY e2,
    IN_OPT PVOID Context){

	UNREFERENCED_PARAMETER(Context);
    //TODO
	//compare the two threads priority and return the result such that to
    //order the list in a descendant way(i.e.negative, if second thread's
    //priority is less the that of the rst, positive if the opposite, and zero if
    //equal)
	
	PTHREAD t1 = CONTAINING_RECORD(e1, THREAD, ReadyList);
	PTHREAD t2 = CONTAINING_RECORD(e2, THREAD, ReadyList);

	//Get priority of the two threads using the ThreadGetPriority function
	INT32 p1 = ThreadGetPriority(t1);
	INT32 p2 = ThreadGetPriority(t2);
	
	if (p1 < p2) {
		return 1;
	}
	else if (p1 > p2) {
		return -1;
	}
	else {
		return 0;
	}
}


STATUS
(__cdecl ThreadYieldForIpi)(
    IN_OPT  PVOID   Context
    ) {
	UNREFERENCED_PARAMETER(Context);
 
	//disable interupts
    INTR_STATE oldState;
    oldState = CpuIntrDisable();
	
    GetCurrentPcpu()->ThreadData.YieldOnInterruptReturn = TRUE;
	
	//enable interrupts
    CpuIntrSetState(oldState);
	return STATUS_SUCCESS;
}

void
_No_competing_thread_
ThreadSystemPreinit(
    void
    )
{
    memzero(&m_threadSystemData, sizeof(THREAD_SYSTEM_DATA));

    InitializeListHead(&m_threadSystemData.AllThreadsList);
    LockInit(&m_threadSystemData.AllThreadsLock);

    InitializeListHead(&m_threadSystemData.ReadyThreadsList);
    LockInit(&m_threadSystemData.ReadyThreadsLock);
}

STATUS
ThreadSystemInitMainForCurrentCPU(
    void
    )
{
    STATUS status;
    PPCPU pCpu;
    char mainThreadName[MAX_PATH];
    PTHREAD pThread;
    PPROCESS pProcess;

    LOG_FUNC_START;

    status = STATUS_SUCCESS;
    pCpu = GetCurrentPcpu();
    pThread = NULL;
    pProcess = ProcessRetrieveSystemProcess();

    ASSERT( NULL != pCpu );

    snprintf( mainThreadName, MAX_PATH, "%s-%02x", "main", pCpu->ApicId );

    status = _ThreadInit(mainThreadName, ThreadPriorityDefault, &pThread, FALSE);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_ThreadInit", status );
        return status;
    }
    LOGPL("_ThreadInit succeeded\n");

    pThread->InitialStackBase = pCpu->StackTop;
    pThread->StackSize = pCpu->StackSize;

    pThread->State = ThreadStateRunning;
    SetCurrentThread(pThread);

    // In case of the main thread of the BSP the process will be NULL so we need to handle that case
    // When the system process will be initialized it will insert into its thread list the current thread (which will
    // be the main thread of the BSP)
    if (pProcess != NULL)
    {
        ProcessInsertThreadInList(pProcess, pThread);
    }

    LOG_FUNC_END;

    return status;
}

STATUS
ThreadSystemInitIdleForCurrentCPU(
    void
    )
{
    EX_EVENT idleStarted;
    STATUS status;
    PPCPU pCpu;
    char idleThreadName[MAX_PATH];
    PTHREAD idleThread;

    ASSERT( INTR_OFF == CpuIntrGetState() );

    LOG_FUNC_START_THREAD;

    status = STATUS_SUCCESS;
    pCpu = GetCurrentPcpu();

    ASSERT(NULL != pCpu);

    status = ExEventInit(&idleStarted, ExEventTypeSynchronization, FALSE);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("EvtInitialize", status);
        return status;
    }
    LOGPL("EvtInitialize succeeded\n");

    snprintf(idleThreadName, MAX_PATH, "%s-%02x", "idle", pCpu->ApicId);

    // create idle thread
    status = ThreadCreate(idleThreadName,
                          ThreadPriorityDefault,
                          _IdleThread,
                          &idleStarted,
                          &idleThread
                          );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("ThreadCreate", status);
        return status;
    }
    LOGPL("ThreadCreate for IDLE thread succeeded\n");

    ThreadCloseHandle(idleThread);
    idleThread = NULL;

    LOGPL("About to enable interrupts\n");

    // lets enable some interrupts :)
    CpuIntrEnable();

    LOGPL("Interrupts enabled :)\n");

    // wait for idle thread
    LOG_TRACE_THREAD("Waiting for idle thread signal\n");
    ExEventWaitForSignal(&idleStarted);
    LOG_TRACE_THREAD("Received idle thread signal\n");

    LOG_FUNC_END_THREAD;

    return status;
}

STATUS
ThreadCreate(
    IN_Z        char*               Name,
    IN          THREAD_PRIORITY     Priority,
    IN          PFUNC_ThreadStart   Function,
    IN_OPT      PVOID               Context,
    OUT_PTR     PTHREAD*            Thread
    )
{
    return ThreadCreateEx(Name,
                          Priority,
                          Function,
                          Context,
                          Thread,
                          ProcessRetrieveSystemProcess());
}

STATUS
ThreadCreateEx(
    IN_Z        char*               Name,
    IN          THREAD_PRIORITY     Priority,
    IN          PFUNC_ThreadStart   Function,
    IN_OPT      PVOID               Context,
    OUT_PTR     PTHREAD*            Thread,
    INOUT       struct _PROCESS*    Process
    )
{
    STATUS status;
    PTHREAD pThread;
    PPCPU pCpu;
    BOOLEAN bProcessIniialThread;
    PVOID pStartFunction;
    QWORD firstArg;
    QWORD secondArg;

    if (NULL == Name)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (NULL == Function)
    {
        return STATUS_INVALID_PARAMETER3;
    }

    if (NULL == Thread)
    {
        return STATUS_INVALID_PARAMETER5;
    }

    if (NULL == Process)
    {
        return STATUS_INVALID_PARAMETER6;
    }

    status = STATUS_SUCCESS;
    pThread = NULL;
    pCpu = GetCurrentPcpu();
    bProcessIniialThread = FALSE;
    pStartFunction = NULL;
    firstArg = 0;
    secondArg = 0;

    ASSERT(NULL != pCpu);

    status = _ThreadInit(Name, Priority, &pThread, TRUE);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_ThreadInit", status);
        return status;
    }

    ProcessInsertThreadInList(Process, pThread);

    // the reference must be done outside _ThreadInit
    _ThreadReference(pThread);

    if (!Process->PagingData->Data.KernelSpace)
    {
        // Create user-mode stack
        pThread->UserStack = MmuAllocStack(STACK_DEFAULT_SIZE,
                                           TRUE,
                                           FALSE,
                                           Process);
        if (pThread->UserStack == NULL)
        {
            status = STATUS_MEMORY_CANNOT_BE_COMMITED;
            LOG_FUNC_ERROR_ALLOC("MmuAllocStack", STACK_DEFAULT_SIZE);
            return status;
        }

        bProcessIniialThread = (Function == Process->HeaderInfo->Preferred.AddressOfEntryPoint);

        // We are the first thread => we must pass the argc and argv parameters
        // and the whole command line which spawned the process
        if (bProcessIniialThread)
        {
            // It's one because we already incremented it when we called ProcessInsertThreadInList earlier
            ASSERT(Process->NumberOfThreads == 1);

            status = _ThreadSetupMainThreadUserStack(pThread->UserStack,
                                                     &pThread->UserStack,
                                                     Process);
            if (!SUCCEEDED(status))
            {
                LOG_FUNC_ERROR("_ThreadSetupUserStack", status);
                return status;
            }
        }
        else
        {
            pThread->UserStack = (PVOID) PtrDiff(pThread->UserStack, SHADOW_STACK_SIZE + sizeof(PVOID));
        }

        pStartFunction = (PVOID) (bProcessIniialThread ? Process->HeaderInfo->Preferred.AddressOfEntryPoint : Function);
        firstArg       = (QWORD) (bProcessIniialThread ? Process->NumberOfArguments : (QWORD) Context);
        secondArg      = (QWORD) (bProcessIniialThread ? PtrOffset(pThread->UserStack, SHADOW_STACK_SIZE + sizeof(PVOID)) : 0);
    }
    else
    {
        // Kernel mode

        // warning C4152: nonstandard extension, function/data pointer conversion in expression
#pragma warning(suppress:4152)
        pStartFunction = _ThreadKernelFunction;

        firstArg =  (QWORD) Function;
        secondArg = (QWORD) Context;
    }

    status = _ThreadSetupInitialState(pThread,
                                      pStartFunction,
                                      firstArg,
                                      secondArg,
                                      Process->PagingData->Data.KernelSpace);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("_ThreadSetupInitialState", status);
        return status;
    }

    if (NULL == pCpu->ThreadData.IdleThread)
    {
        pThread->State = ThreadStateReady;

        // this is the IDLE thread creation
        pCpu->ThreadData.IdleThread = pThread;
    }
    else
    {
        ThreadUnblock(pThread);
    }

    *Thread = pThread;

    return status;
}

void
ThreadTick(
    void
    )
{
    PPCPU pCpu = GetCurrentPcpu();
    PTHREAD pThread = GetCurrentThread();

    ASSERT( INTR_OFF == CpuIntrGetState());
    ASSERT( NULL != pCpu);

    LOG_TRACE_THREAD("Thread tick\n");
    if (pCpu->ThreadData.IdleThread == pThread)
    {
        pCpu->ThreadData.IdleTicks++;
    }
    else
    {
        pCpu->ThreadData.KernelTicks++;
    }
    pThread->TickCountCompleted++;

    if (++pCpu->ThreadData.RunningThreadTicks >= THREAD_TIME_SLICE)
    {
        LOG_TRACE_THREAD("Will yield on return\n");
        pCpu->ThreadData.YieldOnInterruptReturn = TRUE;
    }
}

void
ThreadYield(
    void
    )
{
    INTR_STATE dummyState;
    INTR_STATE oldState;
    PTHREAD pThread = GetCurrentThread();
    PPCPU pCpu;
    BOOLEAN bForcedYield;

    ASSERT( NULL != pThread);

    oldState = CpuIntrDisable();

    pCpu = GetCurrentPcpu();

    ASSERT( NULL != pCpu );

    bForcedYield = pCpu->ThreadData.YieldOnInterruptReturn;
    pCpu->ThreadData.YieldOnInterruptReturn = FALSE;

    if (THREAD_FLAG_FORCE_TERMINATE_PENDING == _InterlockedAnd(&pThread->Flags, MAX_DWORD))
    {
        _ThreadForcedExit();
        NOT_REACHED;
    }

    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &dummyState);
    if (pThread != pCpu->ThreadData.IdleThread)
    {
        //David:
        // Made the thread to be inserted in a order fashion in the ready list
        //InsertTailList(&m_threadSystemData.ReadyThreadsList, &pThread->ReadyList);
		//Replaced with InsertOrderedList function
		InsertOrderedList(&m_threadSystemData.ReadyThreadsList, &pThread->ReadyList, ThreadComparePriorityReadyList, NULL);
    }
    if (!bForcedYield)
    {
        pThread->TickCountEarly++;
    }
    pThread->State = ThreadStateReady;
    _ThreadSchedule();
    ASSERT( !LockIsOwner(&m_threadSystemData.ReadyThreadsLock));
    LOG_TRACE_THREAD("Returned from _ThreadSchedule\n");

    CpuIntrSetState(oldState);
}

void
ThreadBlock(
    void
    )
{
    INTR_STATE oldState;
    PTHREAD pCurrentThread;

    pCurrentThread = GetCurrentThread();

    ASSERT( INTR_OFF == CpuIntrGetState());
    ASSERT(LockIsOwner(&pCurrentThread->BlockLock));

    if (THREAD_FLAG_FORCE_TERMINATE_PENDING == _InterlockedAnd(&pCurrentThread->Flags, MAX_DWORD))
    {
        _ThreadForcedExit();
        NOT_REACHED;
    }

    pCurrentThread->TickCountEarly++;
    pCurrentThread->State = ThreadStateBlocked;
    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &oldState);
    _ThreadSchedule();
    ASSERT( !LockIsOwner(&m_threadSystemData.ReadyThreadsLock));
}

//David:
// This function is currently unused in the code, but it can be activated in the ThreadBlock function to make that function more efficient
// This function gets the min priority of a thread that is currentlly running on the CPU
THREAD_PRIORITY
GetMinPriorityOfRunningThreads(
void
) {	
		PLIST_ENTRY pListEntry;
		PTHREAD pThread;
		THREAD_PRIORITY minPriority = ThreadPriorityMaximum;
        INTR_STATE dummyState;

		//Get AllThreadsList lock
		LockAcquire(&m_threadSystemData.AllThreadsLock, &dummyState);
        //Itterate through the all threads and find the minimum priority of the running threads
		for (pListEntry = m_threadSystemData.AllThreadsList.Flink;
			pListEntry != &m_threadSystemData.AllThreadsList;
			pListEntry = pListEntry->Flink)
		{
			pThread = CONTAINING_RECORD(pListEntry, THREAD, AllList);
			if (pThread->State == ThreadStateRunning && ThreadGetPriority(pThread) < minPriority)
			{
                minPriority = ThreadGetPriority(pThread);
			}
		}
		LockRelease(&m_threadSystemData.AllThreadsLock,dummyState);
		
	return minPriority;
}

void
ThreadUnblock(
    IN      PTHREAD              Thread
    )
{
    INTR_STATE oldState;
    INTR_STATE dummyState;

    ASSERT(NULL != Thread);

    LockAcquire(&Thread->BlockLock, &oldState);

    ASSERT(ThreadStateBlocked == Thread->State);

    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &dummyState);
    //David:
    // Made the thread to be inserted in a order fashion in the ready list
    //InsertTailList(&m_threadSystemData.ReadyThreadsList, &Thread->ReadyList);
    //Changed with InsertOrderedList
    InsertOrderedList(&m_threadSystemData.ReadyThreadsList, &Thread->ReadyList, ThreadComparePriorityReadyList, NULL);
    Thread->State = ThreadStateReady;
    LockRelease(&m_threadSystemData.ReadyThreadsLock, dummyState);
    LockRelease(&Thread->BlockLock, oldState);

    //David:
	//send ipi so each cpu will then call ThreadYield if the unblocked thread has higher priority than any running thread
    //if (ThreadGetPriority(Thread) > GetMinPriorityOfRunningThreads()) {
		    SMP_DESTINATION dest = { 0 };
        SmpSendGenericIpiEx(ThreadYieldForIpi, NULL, NULL, NULL,
        FALSE, SmpIpiSendToAllIncludingSelf, dest);
    //}
}

void
ThreadExit(
    IN      STATUS              ExitStatus
    )
{
    PTHREAD pThread;
    INTR_STATE oldState;

    LOG_FUNC_START_THREAD;

    pThread = GetCurrentThread();

    CpuIntrDisable();

    if (LockIsOwner(&pThread->BlockLock))
    {
        LockRelease(&pThread->BlockLock, INTR_OFF);
    }

    pThread->State = ThreadStateDying;
    pThread->ExitStatus = ExitStatus;
    ExEventSignal(&pThread->TerminationEvt);

    ProcessNotifyThreadTermination(pThread);

    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &oldState);
    _ThreadSchedule();
    NOT_REACHED;
}

BOOLEAN
ThreadYieldOnInterrupt(
    void
    )
{
    return GetCurrentPcpu()->ThreadData.YieldOnInterruptReturn;
}

void
ThreadTakeBlockLock(
    void
    )
{
    INTR_STATE dummyState;

    LockAcquire(&GetCurrentThread()->BlockLock, &dummyState);
}

void
ThreadWaitForTermination(
    IN      PTHREAD             Thread,
    OUT     STATUS*             ExitStatus
    )
{
    ASSERT( NULL != Thread );
    ASSERT( NULL != ExitStatus);

    ExEventWaitForSignal(&Thread->TerminationEvt);

    *ExitStatus = Thread->ExitStatus;
}

void
ThreadCloseHandle(
    INOUT   PTHREAD             Thread
    )
{
    ASSERT( NULL != Thread);

    _ThreadDereference(Thread);
}

void
ThreadTerminate(
    INOUT   PTHREAD             Thread
    )
{
    ASSERT( NULL != Thread );

    // it's not a problem if the thread already finished
    _InterlockedOr(&Thread->Flags, THREAD_FLAG_FORCE_TERMINATE_PENDING );
}

const
char*
ThreadGetName(
    IN_OPT  PTHREAD             Thread
    )
{
    PTHREAD pThread = (NULL != Thread) ? Thread : GetCurrentThread();

    return (NULL != pThread) ? pThread->Name : "";
}

TID
ThreadGetId(
    IN_OPT  PTHREAD             Thread
    )
{
    PTHREAD pThread = (NULL != Thread) ? Thread : GetCurrentThread();

    return (NULL != pThread) ? pThread->Id : 0;
}

THREAD_PRIORITY
ThreadGetPriority(
    IN_OPT  PTHREAD             Thread
    )
{
    PTHREAD pThread = (NULL != Thread) ? Thread : GetCurrentThread();

    return (NULL != pThread) ? pThread->Priority : 0;
}

void
ThreadSetPriority(
    IN      THREAD_PRIORITY     NewPriority
    )
{
    ASSERT(ThreadPriorityLowest <= NewPriority && NewPriority <= ThreadPriorityMaximum);
    // Bogdan: 
    // In case in which a thread wants to increase his own real priority, 
    // then an effective priority must be assigned as well.
    PTHREAD pCurrentThread = GetCurrentThread();
    pCurrentThread->RealPriority = NewPriority;
    if (NewPriority > pCurrentThread->Priority)
    ThreadRecomputePriority(pCurrentThread);

	//David:
    /*if a currently running thread calling ThreadSetPriority() would de - crease its priority, there could be two subcases :
    (a) if the new priority is larger than those of all threads in ready list,
        noting should happen, while this is equivalent to the previous case;
    (b) if the new priority is smaller than one of threads in ready list,
        then the currently running thread must give up the CPU in favor
        of a higher - priority thread in ready list; this could done be very
        simple by calling the ThreadYield() function.
        */
	// see if the new priority is smaller than one of threads in ready list
	BOOLEAN found = FALSE;
    // get readyThreadlock
	
	INTR_STATE dummyState;
    LockAcquire(&m_threadSystemData.ReadyThreadsLock, &dummyState);
	
    LIST_ITERATOR it;
    ListIteratorInit(&m_threadSystemData.ReadyThreadsList, &it);
	
	for (PLIST_ENTRY pEntry = ListIteratorNext(&it); pEntry != NULL; pEntry = ListIteratorNext(&it))
	{
		PTHREAD pThread = CONTAINING_RECORD(pEntry, THREAD, ReadyList);
		if (pThread->Priority > GetCurrentThread()->Priority)
		{
            found = TRUE;
			break;
		}
	}
	
	LockRelease(&m_threadSystemData.ReadyThreadsLock, dummyState);
		
	if (found)
	{
		ThreadYield();
	}
}

STATUS
ThreadExecuteForEachThreadEntry(
    IN      PFUNC_ListFunction  Function,
    IN_OPT  PVOID               Context
    )
{
    STATUS status;
    INTR_STATE oldState;

    if (NULL == Function)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    status = STATUS_SUCCESS;

    LockAcquire(&m_threadSystemData.AllThreadsLock, &oldState);
    status = ForEachElementExecute(&m_threadSystemData.AllThreadsList,
                                   Function,
                                   Context,
                                   FALSE
                                   );
    LockRelease(&m_threadSystemData.AllThreadsLock, oldState );

    return status;
}

void
SetCurrentThread(
    IN      PTHREAD     Thread
    )
{
    PPCPU pCpu;

    __writemsr(IA32_FS_BASE_MSR, Thread);

    pCpu = GetCurrentPcpu();
    ASSERT(pCpu != NULL);

    pCpu->ThreadData.CurrentThread = Thread->Self;
    if (NULL != Thread->Self)
    {
        pCpu->StackTop = Thread->InitialStackBase;
        pCpu->StackSize = Thread->StackSize;
        pCpu->Tss.Rsp[0] = (QWORD) Thread->InitialStackBase;
    }
}

static
STATUS
_ThreadInit(
    IN_Z        char*               Name,
    IN          THREAD_PRIORITY     Priority,
    OUT_PTR     PTHREAD*            Thread,
    IN          BOOLEAN             AllocateKernelStack
    )
{
    STATUS status;
    PTHREAD pThread;
    DWORD nameLen;
    PVOID pStack;
    INTR_STATE oldIntrState;

    LOG_FUNC_START;

    ASSERT(NULL != Name);
    ASSERT(NULL != Thread);
    ASSERT_INFO(ThreadPriorityLowest <= Priority && Priority <= ThreadPriorityMaximum,
                "Priority is 0x%x\n", Priority);

    status = STATUS_SUCCESS;
    pThread = NULL;
    nameLen = strlen(Name);
    pStack = NULL;

    __try
    {
        pThread = ExAllocatePoolWithTag(PoolAllocateZeroMemory, sizeof(THREAD), HEAP_THREAD_TAG, 0);
        if (NULL == pThread)
        {
            LOG_FUNC_ERROR_ALLOC("HeapAllocatePoolWithTag", sizeof(THREAD));
            status = STATUS_HEAP_INSUFFICIENT_RESOURCES;
            __leave;
        }

        RfcPreInit(&pThread->RefCnt);

        status = RfcInit(&pThread->RefCnt, _ThreadDestroy, NULL);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("RfcInit", status);
            __leave;
        }

        pThread->Self = pThread;

        status = ExEventInit(&pThread->TerminationEvt, ExEventTypeNotification, FALSE);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("ExEventInit", status);
            __leave;
        }

        if (AllocateKernelStack)
        {
            pStack = MmuAllocStack(STACK_DEFAULT_SIZE, TRUE, FALSE, NULL);
            if (NULL == pStack)
            {
                LOG_FUNC_ERROR_ALLOC("MmuAllocStack", STACK_DEFAULT_SIZE);
                status = STATUS_MEMORY_CANNOT_BE_COMMITED;
                __leave;
            }
            pThread->Stack = pStack;
            pThread->InitialStackBase = pStack;
            pThread->StackSize = STACK_DEFAULT_SIZE;
        }

        pThread->Name = ExAllocatePoolWithTag(PoolAllocateZeroMemory, sizeof(char)*(nameLen + 1), HEAP_THREAD_TAG, 0);
        if (NULL == pThread->Name)
        {
            LOG_FUNC_ERROR_ALLOC("HeapAllocatePoolWithTag", sizeof(char)*(nameLen + 1));
            status = STATUS_HEAP_INSUFFICIENT_RESOURCES;
            __leave;
        }

        strcpy(pThread->Name, Name);

        pThread->Id = _ThreadSystemGetNextTid();
        pThread->State = ThreadStateBlocked;
        pThread->Priority = Priority;
        // Bogdan:
        // Initialize the new added fields.
        pThread->RealPriority = Priority;
        pThread->WaitedMutex = NULL; // at the start, the thread is not waiting for anything
        InitializeListHead(&pThread->AcquiredMutexesList);
        
        LockInit(&pThread->BlockLock);

        LockAcquire(&m_threadSystemData.AllThreadsLock, &oldIntrState);
        //David:
        //add the thread to the allThreadsList in order
       // InsertTailList(&m_threadSystemData.AllThreadsList, &pThread->AllList);
		//Replace with InsertOrtderList
        InsertOrderedList(&m_threadSystemData.AllThreadsList, &pThread->AllList, ThreadComparePriorityReadyList, NULL);
        LockRelease(&m_threadSystemData.AllThreadsLock, oldIntrState);
    }
    __finally
    {
        if (!SUCCEEDED(status))
        {
            if (NULL != pThread)
            {
                _ThreadDereference(pThread);
                pThread = NULL;
            }
        }

        *Thread = pThread;

        LOG_FUNC_END;
    }

    return status;
}

//  STACK TOP
//  -----------------------------------------------------------------
//  |                                                               |
//  |       Shadow Space                                            |
//  |                                                               |
//  |                                                               |
//  -----------------------------------------------------------------
//  |     Dummy Function RA                                         |
//  ---------------------------------------------------------------------------------
//  |     SS     = DS64Supervisor        | DS64Usermode             |               |
//  -----------------------------------------------------------------               |
//  |     RSP    = &(Dummy Function RA)  | Thread->UserStack        |               |
//  -----------------------------------------------------------------               |
//  |     RFLAGS = RFLAGS_IF | RFLAGS_RESERVED                      |   Interrupt   |
//  -----------------------------------------------------------------     Stack     |
//  |     CS     = CS64Supervisor        | CS64Usermode             |               |
//  -----------------------------------------------------------------               |
//  |     RIP    = _ThreadKernelFunction | AddressOfEntryPoint      |               |
//  ---------------------------------------------------------------------------------
//  |     Thread Start Function                                     |
//  -----------------------------------------------------------------
//  |                                                               |
//  |       PROCESSOR_STATE                                         |
//  |                                                               |
//  |                                                               |
//  -----------------------------------------------------------------
//  STACK BASE <- RSP at ThreadSwitch
static
STATUS
_ThreadSetupInitialState(
    IN      PTHREAD             Thread,
    IN      PVOID               StartFunction,
    IN      QWORD               FirstArgument,
    IN      QWORD               SecondArgument,
    IN      BOOLEAN             KernelStack
    )
{
    STATUS status;
    PVOID* pStack;
    PCOMPLETE_PROCESSOR_STATE pState;
    PINTERRUPT_STACK pIst;

    ASSERT( NULL != Thread );
    ASSERT( NULL != StartFunction);

    status = STATUS_SUCCESS;

    pStack = (PVOID*) Thread->Stack;

    // The kernel function has to have a shadow space and a dummy RA
    pStack = pStack - ( 4 + 1 );

    pStack = (PVOID*) PtrDiff(pStack, sizeof(INTERRUPT_STACK));

    // setup pseudo-interrupt stack
    pIst = (PINTERRUPT_STACK) pStack;

    pIst->Rip = (QWORD) StartFunction;
    if (KernelStack)
    {
        pIst->CS = GdtMuGetCS64Supervisor();
        pIst->Rsp = (QWORD)(pIst + 1);
        pIst->SS = GdtMuGetDS64Supervisor();
    }
    else
    {
        ASSERT(Thread->UserStack != NULL);

        pIst->CS = GdtMuGetCS64Usermode() | RING_THREE_PL;
        pIst->Rsp = (QWORD) Thread->UserStack;
        pIst->SS = GdtMuGetDS64Usermode() | RING_THREE_PL;
    }

    pIst->RFLAGS = RFLAGS_INTERRUPT_FLAG_BIT | RFLAGS_RESERVED_BIT;

    pStack = pStack - 1;

    // warning C4054: 'type cast': from function pointer 'void (__cdecl *)(const PFUNC_ThreadStart,const PVOID)' to data pointer 'PVOID'
#pragma warning(suppress:4054)
    *pStack = (PVOID) ThreadStart;

    pStack = (PVOID*) PtrDiff(pStack, sizeof(COMPLETE_PROCESSOR_STATE));
    pState = (PCOMPLETE_PROCESSOR_STATE) pStack;

    memzero(pState, sizeof(COMPLETE_PROCESSOR_STATE));
    pState->RegisterArea.RegisterValues[RegisterRcx] = FirstArgument;
    pState->RegisterArea.RegisterValues[RegisterRdx] = SecondArgument;

    Thread->Stack = pStack;

    return STATUS_SUCCESS;
}


//  USER STACK TOP
//  -----------------------------------------------------------------
//  |                       Argument N-1                            |
//  -----------------------------------------------------------------
//  |                          ...                                  |
//  -----------------------------------------------------------------
//  |                       Argument 0                              |
//  -----------------------------------------------------------------
//  |                 argv[N-1] = &(Argument N-1)                   |
//  -----------------------------------------------------------------
//  |                          ...                                  |
//  -----------------------------------------------------------------
//  |                 argv[0] = &(Argument 0)                       |
//  -----------------------------------------------------------------
//  |                 Dummy 4th Arg = 0xDEADBEEF                    |
//  -----------------------------------------------------------------
//  |                 Dummy 3rd Arg = 0xDEADBEEF                    |
//  -----------------------------------------------------------------
//  |                 argv = &argv[0]                               |
//  -----------------------------------------------------------------
//  |                 argc = N (Process->NumberOfArguments)         |
//  -----------------------------------------------------------------
//  |                 Dummy RA = 0xDEADC0DE                         |
//  -----------------------------------------------------------------
//  USER STACK BASE

static
STATUS
_ThreadSetupMainThreadUserStack(
    IN      PVOID               InitialStack,
    OUT     PVOID*              ResultingStack,
    IN      PPROCESS            Process
    )
{
    ASSERT(InitialStack != NULL);
    ASSERT(ResultingStack != NULL);
    ASSERT(Process != NULL);
	

    //LOG("Initial stack : 0x%X\n",InitialStack);
	
	//stackAlligmnet
	QWORD stackAlligmnet = 0;
	QWORD stackSize = strlen(Process->FullCommandLine) + Process->NumberOfArguments * sizeof(char*) + sizeof(char**) + SHADOW_STACK_SIZE + sizeof(QWORD) + sizeof(PVOID);
	
	//Calculate the stack allignment

    if (stackSize % 0x10 < 8) {
        stackAlligmnet = 8 - stackSize % 0x10;
    }
    else if(stackSize % 0x10 > 8) {
        stackAlligmnet = 0x10 - stackSize % 0x10 + 8;
    }

	//Get final stack size
    stackSize += stackAlligmnet;
	
	//stackBuffer
    PVOID stackBuffer = NULL;
    
    *ResultingStack = (PVOID)PtrDiff(InitialStack, stackSize);
    //LOG("Final stack : 0x%X\n", *ResultingStack);
    //allocate the stack
    MmuGetSystemVirtualAddressForUserBuffer(*ResultingStack, stackSize, PAGE_RIGHTS_READWRITE, Process, &stackBuffer);
    
    //clear the mem
    memzero(stackBuffer, (DWORD)stackSize);
    //LOG("Got the virtual mapping\n");
    
    //Add the stack data
	//Since we are using the stack from the bottom up we must first add the stack dummy return address
    QWORD currentOffset = 0;
	*(PQWORD)PtrOffset(stackBuffer, currentOffset) = 0xab12345;
    //LOG("Added dummy return adr\n");
	//LOG("Dummmy return : 0x%X\n", stackBuffer);
	
	//We will now add the nr of arguments
    currentOffset += sizeof(void*);
	*(PQWORD)PtrOffset(stackBuffer, currentOffset) = Process->NumberOfArguments;
    //LOG("Added nr args\n");
	//LOG("Nr args addr. : 0x%X\n", PtrOffset(stackBuffer, currentOffset));
	
	//We will now add the argv pointer
    currentOffset += sizeof(QWORD);
	*(char***)PtrOffset(stackBuffer, currentOffset) = (char**)PtrOffset(ResultingStack, currentOffset + 0x10);
    //LOG("Added argv pointer\n");
	//LOG("argv pointer : 0x%X\n", PtrOffset(stackBuffer, currentOffset));
	
    currentOffset += sizeof(char**);
	//Now to add the shadowSpace
    *(PQWORD)PtrOffset(stackBuffer, currentOffset) = 0xDEADBEEF;
    currentOffset += sizeof(void*);
    *(PQWORD)PtrOffset(stackBuffer, currentOffset) = 0xDEADBEEF;
    currentOffset += sizeof(void*);
    //LOG("Added shadow space\n");
	    
    
    //Final step adding the params	
	
    char* args = Process->FullCommandLine;
    char* token = (char*)strtok_s(NULL, " ", &args);
	QWORD dataOffset = currentOffset + (Process->NumberOfArguments * sizeof(char*));
    //LOG("Adding data\n");
    while (token != NULL) {
		*(char**)PtrOffset(stackBuffer, currentOffset) = (char*)PtrOffset(*ResultingStack, dataOffset);
        //LOG("argP 0x%X  argVal 0x%X\n", PtrOffset(stackBuffer, currentOffset), PtrOffset(*ResultingStack, dataOffset));
				
		//add string
        strcpy((char*)PtrOffset(stackBuffer, dataOffset), token);
					
		//add offsets
		currentOffset += sizeof(char*);
        dataOffset += strlen(token) + 1;
       // LOG("strLen %d", strlen(token) + 1);
		
		token = (char*)strtok_s(NULL, " ", &args);
	}
	
    MmuFreeSystemVirtualAddressForUserBuffer((PVOID)stackBuffer);
    //LOG("Freed the virtual mapping\n");
	
 
    return STATUS_SUCCESS;
}


REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
RELEASES_EXCL_AND_NON_REENTRANT_LOCK(m_threadSystemData.ReadyThreadsLock)
static
void
_ThreadSchedule(
    void
    )
{
    PTHREAD pCurrentThread;
    PTHREAD pNextThread;
    PCPU* pCpu;

    ASSERT(INTR_OFF == CpuIntrGetState());
    ASSERT(LockIsOwner(&m_threadSystemData.ReadyThreadsLock));

    pCurrentThread = GetCurrentThread();
    ASSERT( NULL != pCurrentThread );

    pCpu = GetCurrentPcpu();

    // save previous thread
    pCpu->ThreadData.PreviousThread = pCurrentThread;

    // get next thread
    pNextThread = _ThreadGetReadyThread();
    ASSERT( NULL != pNextThread );

	//log the id of the thread that is being scheduled
	//LOG("Thread %d is being scheduled\n", pNextThread->Id);
	//LOG("Current thread %d \n", pCurrentThread->Id);
	

    // if current differs from next
    // => schedule next
    if (pNextThread != pCurrentThread)
    {
        LOG_TRACE_THREAD("Before ThreadSwitch\n");
        LOG_TRACE_THREAD("Current thread: %s\n", pCurrentThread->Name);
        LOG_TRACE_THREAD("Next thread: %s\n", pNextThread->Name);

        if (pCurrentThread->Process != pNextThread->Process)
        {
            MmuChangeProcessSpace(pNextThread->Process);
        }

        // Before any thread is scheduled it executes this function, thus if we set the current
        // thread to be the next one it will be fine - there is no possibility of interrupts
        // appearing to cause inconsistencies
        pCurrentThread->UninterruptedTicks = 0;

        SetCurrentThread(pNextThread);
        ThreadSwitch( &pCurrentThread->Stack, pNextThread->Stack);

        ASSERT(INTR_OFF == CpuIntrGetState());
        ASSERT(LockIsOwner(&m_threadSystemData.ReadyThreadsLock));

        LOG_TRACE_THREAD("After ThreadSwitch\n");
        LOG_TRACE_THREAD("Current: %s\n", pCurrentThread->Name);

        // We cannot log the name of the 'next thread', i.e. the thread which formerly preempted
        // this one because a long time may have passed since then and it may have been destroyed

        // The previous thread may also have been destroyed after it was de-scheduled, we have
        // to be careful before logging its name
        if (pCpu->ThreadData.PreviousThread != NULL)
        {
            LOG_TRACE_THREAD("Prev thread: %s\n", pCpu->ThreadData.PreviousThread->Name);
        }
    }
    else
    {
        pCurrentThread->UninterruptedTicks++;
    }

    ThreadCleanupPostSchedule();
}

REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
RELEASES_EXCL_AND_NON_REENTRANT_LOCK(m_threadSystemData.ReadyThreadsLock)
void
ThreadCleanupPostSchedule(
    void
    )
{
    PTHREAD prevThread;

    ASSERT(INTR_OFF == CpuIntrGetState());

    GetCurrentPcpu()->ThreadData.RunningThreadTicks = 0;
    prevThread = GetCurrentPcpu()->ThreadData.PreviousThread;

    LockRelease(&m_threadSystemData.ReadyThreadsLock, INTR_OFF);

    if (NULL != prevThread)
    {
        if (LockIsOwner(&prevThread->BlockLock))
        {
            // Unfortunately, we cannot use the inverse condition because it is not always
            // true, i.e. if the previous thread is the idle thread it's not 100% sure that
            // it was previously holding the block hold, it may have been preempted before
            // acquiring it.
            ASSERT(prevThread->State == ThreadStateBlocked
                   || prevThread == GetCurrentPcpu()->ThreadData.IdleThread);

            LOG_TRACE_THREAD("Will release block lock for thread [%s]\n", prevThread->Name);

            _Analysis_assume_lock_held_(prevThread->BlockLock);
            LockRelease(&prevThread->BlockLock, INTR_OFF);
        }
        else if (prevThread->State == ThreadStateDying)
        {
            LOG_TRACE_THREAD("Will dereference thread: [%s]\n", prevThread->Name);

            // dereference thread
            _ThreadDereference(prevThread);
            GetCurrentPcpu()->ThreadData.PreviousThread = NULL;
        }
    }
}

static
STATUS
(__cdecl _IdleThread)(
    IN_OPT      PVOID       Context
    )
{
    PEX_EVENT pEvent;

    LOG_FUNC_START_THREAD;

    ASSERT( NULL != Context);

    pEvent = (PEX_EVENT) Context;
    ExEventSignal(pEvent);

    // warning C4127: conditional expression is constant
#pragma warning(suppress:4127)
    while (TRUE)
    {
        CpuIntrDisable();
        ThreadTakeBlockLock();
        ThreadBlock();

        __sti_and_hlt();
    }

    NOT_REACHED;
}

REQUIRES_EXCL_LOCK(m_threadSystemData.ReadyThreadsLock)
static
_Ret_notnull_
PTHREAD
_ThreadGetReadyThread(
    void
    )
{
    PTHREAD pNextThread;
    PLIST_ENTRY pEntry;
    BOOLEAN bIdleScheduled;

    ASSERT( INTR_OFF == CpuIntrGetState());
    ASSERT( LockIsOwner(&m_threadSystemData.ReadyThreadsLock));

    pNextThread = NULL;

    pEntry = RemoveHeadList(&m_threadSystemData.ReadyThreadsList);
    if (pEntry == &m_threadSystemData.ReadyThreadsList)
    {
        pNextThread = GetCurrentPcpu()->ThreadData.IdleThread;
        bIdleScheduled = TRUE;
    }
    else
    {
        pNextThread = CONTAINING_RECORD( pEntry, THREAD, ReadyList );

        ASSERT( pNextThread->State == ThreadStateReady );
        bIdleScheduled = FALSE;
    }

    // maybe we shouldn't update idle time each time a thread is scheduled
    // maybe it is enough only every x times
    // or maybe we can update time only on RTC updates
    CoreUpdateIdleTime(bIdleScheduled);

    return pNextThread;
}

static
void
_ThreadForcedExit(
    void
    )
{
    PTHREAD pCurrentThread = GetCurrentThread();

    _InterlockedOr( &pCurrentThread->Flags, THREAD_FLAG_FORCE_TERMINATED );

    ThreadExit(STATUS_JOB_INTERRUPTED);
    NOT_REACHED;
}

static
void
_ThreadReference(
    INOUT   PTHREAD                 Thread
    )
{
    ASSERT( NULL != Thread );

    RfcReference(&Thread->RefCnt);
}

static
void
_ThreadDereference(
    INOUT   PTHREAD                 Thread
    )
{
    ASSERT( NULL != Thread );

    RfcDereference(&Thread->RefCnt);
}

static
void
_ThreadDestroy(
    IN      PVOID                   Object,
    IN_OPT  PVOID                   Context
    )
{
    INTR_STATE oldState;
    PTHREAD pThread = (PTHREAD) CONTAINING_RECORD(Object, THREAD, RefCnt);

    ASSERT(NULL != pThread);
    ASSERT(NULL == Context);

    LockAcquire(&m_threadSystemData.AllThreadsLock, &oldState);
    RemoveEntryList(&pThread->AllList);
    LockRelease(&m_threadSystemData.AllThreadsLock, oldState);

    // This must be done before removing the thread from the process list, else
    // this may be the last thread and the process VAS will be freed by the time
    // ProcessRemoveThreadFromList - this function also dereferences the process
    if (NULL != pThread->UserStack)
    {
        // Free UM stack
        MmuFreeStack(pThread->UserStack, pThread->Process);
        pThread->UserStack = NULL;
    }

    ProcessRemoveThreadFromList(pThread);

    if (NULL != pThread->Name)
    {
        ExFreePoolWithTag(pThread->Name, HEAP_THREAD_TAG);
        pThread->Name = NULL;
    }

    if (NULL != pThread->Stack)
    {
        // This is the kernel mode stack
        // It does not 'belong' to any process => pass NULL
        MmuFreeStack(pThread->Stack, NULL);
        pThread->Stack = NULL;
    }

    ExFreePoolWithTag(pThread, HEAP_THREAD_TAG);
}

static
void
_ThreadKernelFunction(
    IN      PFUNC_ThreadStart       Function,
    IN_OPT  PVOID                   Context
    )
{
    STATUS exitStatus;

    ASSERT(NULL != Function);

    CHECK_STACK_ALIGNMENT;

    ASSERT(CpuIntrGetState() == INTR_ON);
    exitStatus = Function(Context);

    ThreadExit(exitStatus);
    NOT_REACHED;
}

// Bogdan:
// Function used to find the maximum priority between two threads in the waiting list.
STATUS
(__cdecl RecomputePriorityForEachThreadInWaitingList) (
    IN      PLIST_ENTRY     ListEntry,
    IN_OPT  PVOID           FunctionContext
    )
{
    ASSERT(FunctionContext != NULL);
    PPRIORITY_DONATION_CTX priorityDonationContext = (PPRIORITY_DONATION_CTX)FunctionContext;
    PTHREAD pThread = CONTAINING_RECORD(ListEntry, THREAD, ReadyList);

    if (priorityDonationContext->maximumPriority < ThreadGetPriority(pThread)) {
        priorityDonationContext->maximumPriority = ThreadGetPriority(pThread);
    }

    return STATUS_SUCCESS;
}

// Bogdan: 
// Function used to recompute priority for each mutex acquired by a specific thread.
STATUS
(__cdecl RecomputePriorityForEachMutex) (
    IN      PLIST_ENTRY     ListEntry,
    IN_OPT  PVOID           FunctionContext
    ) 
{
    PMUTEX  waitedMutex = CONTAINING_RECORD(ListEntry, MUTEX, AcquiredMutexListElem);
    ForEachElementExecute(&waitedMutex->WaitingList, RecomputePriorityForEachThreadInWaitingList, FunctionContext, FALSE);
    return STATUS_SUCCESS;
}

// Bogdan:
// Function used to recompute the priority of a thread, considering that the thread 
// might have mutexes acquired which can have a list of threads waiting for them.
void
ThreadRecomputePriority(
    INOUT   PTHREAD     Thread
    )
{   
    PRIORITY_DONATION_CTX priorityDonationContext = { 0 };
    priorityDonationContext.maximumPriority = Thread->RealPriority;
    ForEachElementExecute(&Thread->AcquiredMutexesList, RecomputePriorityForEachMutex, &priorityDonationContext, FALSE);
 
    Thread->Priority = priorityDonationContext.maximumPriority;
}



// Bogdan:
// Function used to solve the priority donation and chained priority donation problem.
void
ThreadDonatePriority(
    INOUT PTHREAD  currentThread,
    INOUT PTHREAD MutexHolder
)
{
    ASSERT(currentThread != NULL);
    ASSERT(MutexHolder != NULL);

    do {
        if (ThreadGetPriority(currentThread) > ThreadGetPriority(MutexHolder))
        {
            MutexHolder->Priority = currentThread->Priority;
        }
        else
        {
            break;
        }

        if (MutexHolder->WaitedMutex)
        {
            currentThread = MutexHolder;
            MutexHolder = MutexHolder->WaitedMutex->Holder;
        }
        else 
        {
            MutexHolder = NULL;
        }
    } while (MutexHolder != NULL);
}
