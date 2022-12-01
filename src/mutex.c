#include "HAL9000.h"
#include "thread_internal.h"
#include "mutex.h"
#include "thread.h"
#include "log.h"


#define MUTEX_MAX_RECURSIVITY_DEPTH         MAX_BYTE

_No_competing_thread_
void
MutexInit(
    OUT         PMUTEX      Mutex,
    IN          BOOLEAN     Recursive
    )
{
    ASSERT( NULL != Mutex );

    memzero(Mutex, sizeof(MUTEX));

    LockInit(&Mutex->MutexLock);

    InitializeListHead(&Mutex->WaitingList);

    Mutex->MaxRecursivityDepth = Recursive ? MUTEX_MAX_RECURSIVITY_DEPTH : 1;
}

ACQUIRES_EXCL_AND_REENTRANT_LOCK(*Mutex)
REQUIRES_NOT_HELD_LOCK(*Mutex)
void
MutexAcquire(
    INOUT       PMUTEX      Mutex
    )
{
    INTR_STATE dummyState;
    INTR_STATE oldState;
    PTHREAD pCurrentThread = GetCurrentThread();

    // Bogdan:
    // Variables used for holding the priority for the current thread 
    // and for the thread which is holding the mutex, 
    // later used for comparisons for priority donation.
    THREAD_PRIORITY currentThreadPriority;
    THREAD_PRIORITY holderThreadPriority;

    ASSERT( NULL != Mutex);
    ASSERT( NULL != pCurrentThread );

    if (pCurrentThread == Mutex->Holder)
    {
        ASSERT( Mutex->CurrentRecursivityDepth < Mutex->MaxRecursivityDepth );
        // Bogdan:
        // If the current thread is the mutex holder it means that it does not have any Mutex 
        // which he is waiting for, thus we need to make the WaitedMutex field NULL.
        pCurrentThread->WaitedMutex = NULL;
        Mutex->CurrentRecursivityDepth++;
        return;
    }

    oldState = CpuIntrDisable();

    LockAcquire(&Mutex->MutexLock, &dummyState );
    if (NULL == Mutex->Holder)
    {
        Mutex->Holder = pCurrentThread;
        Mutex->CurrentRecursivityDepth = 1;
    }

    while (Mutex->Holder != pCurrentThread)
    {
        // Bogdan :
        // Assign the priorities to the previously defined variables
        currentThreadPriority = ThreadGetPriority(pCurrentThread);
        holderThreadPriority = ThreadGetPriority(Mutex->Holder);
      
        // Bogdan : 
        // Start the priority donation only if the thread which wants 
        // to donate (the current one)  has a priority greated than the holder.
        if (currentThreadPriority > holderThreadPriority) {
            ThreadDonatePriority(pCurrentThread, Mutex->Holder);
        }

        //David:
        // Changed to insert threads in an ordered fashion in the ready list
        //InsertTailList(&Mutex->WaitingList, &pCurrentThread->ReadyList);
        //Replaced with InsertOrderedList
        InsertOrderedList(&Mutex->WaitingList, &pCurrentThread->ReadyList, ThreadComparePriorityReadyList, NULL);
        // Bogdan:
        // Set the mutex which the thread will be waiting for to the current one
        pCurrentThread->WaitedMutex = Mutex;
        ThreadTakeBlockLock();
        
        LockRelease(&Mutex->MutexLock, dummyState);
        
        ThreadBlock();
        LockAcquire(&Mutex->MutexLock, &dummyState );
    }

    _Analysis_assume_lock_acquired_(*Mutex);
    // Bogdan::
    // After the lock was acquired, add it to the current thread's list of acquired mutexes.
    InsertTailList(&pCurrentThread->AcquiredMutexesList, &Mutex->AcquiredMutexListElem);
    LockRelease(&Mutex->MutexLock, dummyState);

    CpuIntrSetState(oldState);
}

RELEASES_EXCL_AND_REENTRANT_LOCK(*Mutex)
REQUIRES_EXCL_LOCK(*Mutex)
void
MutexRelease(
    INOUT       PMUTEX      Mutex
    )
{
    INTR_STATE oldState;
    PLIST_ENTRY pEntry;

    ASSERT(NULL != Mutex);
    ASSERT(GetCurrentThread() == Mutex->Holder);

    if (Mutex->CurrentRecursivityDepth > 1)
    {
        Mutex->CurrentRecursivityDepth--;
        return;
    }

    pEntry = NULL;

    LockAcquire(&Mutex->MutexLock, &oldState);

    // Bogdan:
    // When the lock is released, we have to give him back 
    // his original effective priority.
    RemoveEntryList(&Mutex->AcquiredMutexListElem);
    ThreadRecomputePriority(GetCurrentThread());

    pEntry = RemoveHeadList(&Mutex->WaitingList);
    if (pEntry != &Mutex->WaitingList)
    {
        PTHREAD pThread = CONTAINING_RECORD(pEntry, THREAD, ReadyList);

        // wakeup first thread
        Mutex->Holder = pThread;
        Mutex->CurrentRecursivityDepth = 1;
        ThreadUnblock(pThread);
    }
    else
    {
        Mutex->Holder = NULL;
    }

    _Analysis_assume_lock_released_(*Mutex);
    
    LockRelease(&Mutex->MutexLock, oldState);    
}