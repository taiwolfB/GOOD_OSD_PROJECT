#include "HAL9000.h"
#include "ex_timer.h"
#include "iomu.h"
#include "thread_internal.h"
#include "test_common.h"

typedef struct _GLOBAL_TIMER_LIST {
    // protect the global timer list
    LOCK        TimerListLock;

    // the list ’s head
    _Guarded_by_(TimerListLock)
    LIST_ENTRY  TimerListHead;
} GLOBAL_TIMER_LIST, *PGLOBAL_TIMER_LIST;

static GLOBAL_TIMER_LIST m_globalTimerList;

static INT64 (__cdecl ExTimerCompareListElems) (
    IN      PLIST_ENTRY     FirstElem,
    IN      PLIST_ENTRY     SecondElem,
    IN_OPT  PVOID           Context
    ) 
{
    PEX_TIMER pFirstTimer;
    PEX_TIMER pSecondTimer;

    ASSERT(NULL != FirstElem);
    ASSERT(NULL != SecondElem);
    ASSERT(Context == NULL);

    pFirstTimer = CONTAINING_RECORD(FirstElem, EX_TIMER, TimerListElem);
    pSecondTimer = CONTAINING_RECORD(SecondElem, EX_TIMER, TimerListElem);

    return ExTimerCompareTimers(pFirstTimer, pSecondTimer);
}

void
_No_competing_thread_
ExTimerSystemPreinit(
    void
    ) 
{
    memzero(&m_globalTimerList, sizeof(GLOBAL_TIMER_LIST));

    InitializeListHead(&m_globalTimerList.TimerListHead);
    LockInit(&m_globalTimerList.TimerListLock);
}

STATUS
ExTimerInit(
    OUT     PEX_TIMER       Timer,
    IN      EX_TIMER_TYPE   Type,
    IN      QWORD           Time
    )
{
    STATUS status;
    INTR_STATE dummyState;

    if (NULL == Timer)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (Type > ExTimerTypeMax)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    status = STATUS_SUCCESS;

    memzero(Timer, sizeof(EX_TIMER));

    Timer->Type = Type;
    if (Timer->Type != ExTimerTypeAbsolute)
    {
        // relative time

        // if the time trigger time has already passed the timer will
        // be signaled after the first scheduler tick
        Timer->TriggerTimeUs = IomuGetSystemTimeUs() + Time;
        Timer->ReloadTimeUs = Time;
    }
    else
    {
        // absolute
        Timer->TriggerTimeUs = Time;
    }

    ExEventInit(&Timer->TimerEvent, ExEventTypeNotification, FALSE);

    LockAcquire(&m_globalTimerList.TimerListLock, &dummyState);
    InsertOrderedList(&m_globalTimerList.TimerListHead, &Timer->TimerListElem, ExTimerCompareListElems, NULL);
    LockRelease(&m_globalTimerList.TimerListLock, dummyState);

    return status;
}

void
ExTimerStart(
    IN      PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    Timer->TimerStarted = TRUE;
}

void
ExTimerStop(
    IN      PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    Timer->TimerStarted = FALSE;
    ExEventSignal(&Timer->TimerEvent);
}

void
ExTimerWait(
    INOUT   PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    if (Timer->TimerStarted) {
        ExEventWaitForSignal(&Timer->TimerEvent);
    }
}

void
ExTimerUninit(
    INOUT   PEX_TIMER       Timer
    )
{
    INTR_STATE dummyState;
    ASSERT(Timer != NULL);

    ExTimerStop(Timer);

    Timer->TimerUninited = TRUE;

    LockAcquire(&m_globalTimerList.TimerListLock, &dummyState);
    RemoveEntryList(&Timer->TimerListElem);
    LockRelease(&m_globalTimerList.TimerListLock, dummyState);
}

INT64
ExTimerCompareTimers(
    IN      PEX_TIMER     FirstElem,
    IN      PEX_TIMER     SecondElem
)
{
    return FirstElem->TriggerTimeUs - SecondElem->TriggerTimeUs;
}

void ExTimerCheckAll(
    void
    ) 
{
    INTR_STATE dummyState;

    LockAcquire(&m_globalTimerList.TimerListLock, &dummyState);
    ForEachElementExecute(&m_globalTimerList.TimerListHead, ExTimerCheck, NULL, FALSE);
    LockRelease(&m_globalTimerList.TimerListLock, dummyState);
}

STATUS
(__cdecl ExTimerCheck) (
    IN      PLIST_ENTRY     ListEntry,
    IN_OPT  PVOID           FunctionContext
) {
    UNREFERENCED_PARAMETER(FunctionContext);

    PEX_TIMER pTimer = CONTAINING_RECORD(ListEntry, EX_TIMER, TimerListElem);

    if (IomuGetSystemTimeUs() >= pTimer->TriggerTimeUs && pTimer->TimerStarted) {
        ExEventSignal(&pTimer->TimerEvent);
    }

    return STATUS_SUCCESS;
}