#include "HAL9000.h"
#include "syscall.h"
#include "gdtmu.h"
#include "syscall_defs.h"
#include "syscall_func.h"
#include "syscall_no.h"
#include "mmu.h"
#include "process_internal.h"
#include "dmp_cpu.h"
#include "thread.h"
#include "thread_internal.h"

extern void SyscallEntry();

#define SYSCALL_IF_VERSION_KM       SYSCALL_IMPLEMENTED_IF_VERSION

void
SyscallHandler(
    INOUT   COMPLETE_PROCESSOR_STATE    *CompleteProcessorState
    )
{
    SYSCALL_ID sysCallId;
    PQWORD pSyscallParameters;
    PQWORD pParameters;
    STATUS status;
    REGISTER_AREA* usermodeProcessorState;

    ASSERT(CompleteProcessorState != NULL);

    // It is NOT ok to setup the FMASK so that interrupts will be enabled when the system call occurs
    // The issue is that we'll have a user-mode stack and we wouldn't want to receive an interrupt on
    // that stack. This is why we only enable interrupts here.
    ASSERT(CpuIntrGetState() == INTR_OFF);
    CpuIntrSetState(INTR_ON);

    LOG_TRACE_USERMODE("The syscall handler has been called!\n");

    status = STATUS_SUCCESS;
    pSyscallParameters = NULL;
    pParameters = NULL;
    usermodeProcessorState = &CompleteProcessorState->RegisterArea;

    __try
    {
        if (LogIsComponentTraced(LogComponentUserMode))
        {
            DumpProcessorState(CompleteProcessorState);
        }

        // Check if indeed the shadow stack is valid (the shadow stack is mandatory)
        pParameters = (PQWORD)usermodeProcessorState->RegisterValues[RegisterRbp];
        status = MmuIsBufferValid(pParameters, SHADOW_STACK_SIZE, PAGE_RIGHTS_READ, GetCurrentProcess());
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("MmuIsBufferValid", status);
            __leave;
        }

        sysCallId = usermodeProcessorState->RegisterValues[RegisterR8];

        LOG_TRACE_USERMODE("System call ID is %u\n", sysCallId);

        // The first parameter is the system call ID, we don't care about it => +1
        pSyscallParameters = (PQWORD)usermodeProcessorState->RegisterValues[RegisterRbp] + 1;

        // Dispatch syscalls
        switch (sysCallId)
        {
        case SyscallIdIdentifyVersion:
            status = SyscallValidateInterface((SYSCALL_IF_VERSION)*pSyscallParameters);
            break;
        case SyscallIdThreadExit:
            status = SyscallThreadExit((STATUS)pSyscallParameters[0]);
            break;
        case SyscallIdThreadCreate:
            status = SyscallThreadCreate((PFUNC_ThreadStart)pSyscallParameters[0], (PVOID)pSyscallParameters[1], (UM_HANDLE*)pSyscallParameters[2]);
            break;
        case SyscallIdThreadGetTid:
            status = SyscallThreadGetTid((UM_HANDLE)pSyscallParameters[0], (TID*)pSyscallParameters[1]);
            break;
        case SyscallIdThreadWaitForTermination:
            status = SyscallThreadWaitForTermination((UM_HANDLE)pSyscallParameters[0], (STATUS*)pSyscallParameters[1]);
            break;
        case SyscallIdThreadCloseHandle:
            status = SyscallThreadCloseHandle((UM_HANDLE)pSyscallParameters[0]);
            break;
        case SyscallIdProcessCreate:
            status = SyscallProcessCreate(
                (char*)pSyscallParameters[0],
                (QWORD)pSyscallParameters[1],
                (char*)pSyscallParameters[2],
                (QWORD)pSyscallParameters[3],
                (UM_HANDLE*)pSyscallParameters[4]);
            break;
        case SyscallIdProcessExit:
            status = SyscallProcessExit((STATUS)pSyscallParameters[0]);
            break;
        //case SyscallIdProcessGetPid:
        //    status = SyscallProcessGetPid(
        //        (UM_HANDLE)pSyscallParameters[0],
        //        (PID*)pSyscallParameters[1]);
        case SyscallIdFileWrite:
            status = SyscallFileWrite(
                (UM_HANDLE)pSyscallParameters[0],
                (PVOID)pSyscallParameters[1],
                (QWORD)pSyscallParameters[2],
                (QWORD*)pSyscallParameters[3]);
            break;
        //case SyscallIdFileCreate:
        //    status = SyscallFileCreate(
        //        (char*)pSyscallParameters[0], 
        //        (QWORD)pSyscallParameters[1],
        //        (BOOLEAN)pSyscallParameters[2],
        //        (BOOLEAN)pSyscallParameters[3],
        //        (UM_HANDLE*)pSyscallParameters[4]);
        default:
            LOG_ERROR("Unimplemented syscall called from User-space!\n");
            status = STATUS_UNSUPPORTED;
            break;
        }

    }
    __finally
    {
        LOG_TRACE_USERMODE("Will set UM RAX to 0x%x\n", status);

        usermodeProcessorState->RegisterValues[RegisterRax] = status;

        CpuIntrSetState(INTR_OFF);
    }
}

void
SyscallPreinitSystem(
    void
    )
{

}

STATUS
SyscallInitSystem(
    void
    )
{
    return STATUS_SUCCESS;
}

STATUS
SyscallUninitSystem(
    void
    )
{
    return STATUS_SUCCESS;
}

void
SyscallCpuInit(
    void
    )
{
    IA32_STAR_MSR_DATA starMsr;
    WORD kmCsSelector;
    WORD umCsSelector;

    memzero(&starMsr, sizeof(IA32_STAR_MSR_DATA));

    kmCsSelector = GdtMuGetCS64Supervisor();
    ASSERT(kmCsSelector + 0x8 == GdtMuGetDS64Supervisor());

    umCsSelector = GdtMuGetCS32Usermode();
    /// DS64 is the same as DS32
    ASSERT(umCsSelector + 0x8 == GdtMuGetDS32Usermode());
    ASSERT(umCsSelector + 0x10 == GdtMuGetCS64Usermode());

    // Syscall RIP <- IA32_LSTAR
    __writemsr(IA32_LSTAR, (QWORD) SyscallEntry);

    LOG_TRACE_USERMODE("Successfully set LSTAR to 0x%X\n", (QWORD) SyscallEntry);

    // Syscall RFLAGS <- RFLAGS & ~(IA32_FMASK)
    __writemsr(IA32_FMASK, RFLAGS_INTERRUPT_FLAG_BIT);

    LOG_TRACE_USERMODE("Successfully set FMASK to 0x%X\n", RFLAGS_INTERRUPT_FLAG_BIT);

    // Syscall CS.Sel <- IA32_STAR[47:32] & 0xFFFC
    // Syscall DS.Sel <- (IA32_STAR[47:32] + 0x8) & 0xFFFC
    starMsr.SyscallCsDs = kmCsSelector;

    // Sysret CS.Sel <- (IA32_STAR[63:48] + 0x10) & 0xFFFC
    // Sysret DS.Sel <- (IA32_STAR[63:48] + 0x8) & 0xFFFC
    starMsr.SysretCsDs = umCsSelector;

    __writemsr(IA32_STAR, starMsr.Raw);

    LOG_TRACE_USERMODE("Successfully set STAR to 0x%X\n", starMsr.Raw);
}

// SyscallIdIdentifyVersion
STATUS
SyscallValidateInterface(
    IN  SYSCALL_IF_VERSION          InterfaceVersion
)
{
    LOG_TRACE_USERMODE("Will check interface version 0x%x from UM against 0x%x from KM\n",
        InterfaceVersion, SYSCALL_IF_VERSION_KM);

    if (InterfaceVersion != SYSCALL_IF_VERSION_KM)
    {
        LOG_ERROR("Usermode interface 0x%x incompatible with KM!\n", InterfaceVersion);
        return STATUS_INCOMPATIBLE_INTERFACE;
    }

    return STATUS_SUCCESS;
}

STATUS
SyscallThreadExit(
    IN  STATUS                      ExitStatus
)
{
    ThreadExit(ExitStatus);
    return STATUS_SUCCESS;
}

STATUS
SyscallThreadCreate(
    IN      PFUNC_ThreadStart       StartFunction,
    IN_OPT  PVOID                   Context,
    OUT     UM_HANDLE* ThreadHandle
)
{
    if (StartFunction == NULL || MmuIsKernelSpace((PVOID)StartFunction)) {
        return STATUS_INVALID_PARAMETER1;
    }

    if (Context != NULL && MmuIsKernelSpace((PVOID)Context)) {
        return STATUS_INVALID_PARAMETER2;
    }

    if (ThreadHandle == NULL) {
        return STATUS_INVALID_PARAMETER3;
    }

    PPROCESS pProcess = GetCurrentProcess();
    PTHREAD pThread;

    STATUS status = ThreadCreateEx("Thread from Syscall", ThreadPriorityDefault, StartFunction, Context, &pThread, pProcess);

    if (status != STATUS_SUCCESS) {
        return STATUS_UNSUCCESSFUL;
    }

    pProcess->CurrentMaximumHandle++;
    pThread->ThreadHandle = pProcess->CurrentMaximumHandle;

    INTR_STATE dummyState;
    LockAcquire(&pProcess->ThreadTableLock, &dummyState);
    HashTableInsert(&pProcess->ThreadTable, &pThread->ThreadTableEntry);
    LockRelease(&pProcess->ThreadTableLock, dummyState);

    *ThreadHandle = pThread->ThreadHandle;

    return STATUS_SUCCESS;
}

STATUS
SyscallThreadGetTid(
    IN_OPT  UM_HANDLE               ThreadHandle,
    OUT     TID* ThreadId
)
{
    PTHREAD pThread;
    if (ThreadHandle == UM_INVALID_HANDLE_VALUE) {
        pThread = GetCurrentThread();

        if (pThread == NULL) {
            return STATUS_UNSUCCESSFUL;
        }

        *ThreadId = pThread->Id;
        return STATUS_SUCCESS;
    }

    if (ThreadId == NULL || MmuIsKernelSpace((PVOID)ThreadId)) {
        return STATUS_INVALID_PARAMETER2;
    }

    PPROCESS pProcess = GetCurrentProcess();

    INTR_STATE dummyState;
    LockAcquire(&pProcess->ThreadTableLock, &dummyState);
    PHASH_ENTRY threadEntry = HashTableLookup(&pProcess->ThreadTable, (PHASH_KEY)&ThreadHandle);
    LockRelease(&pProcess->ThreadTableLock, dummyState);

    if (threadEntry == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    pThread = CONTAINING_RECORD(threadEntry, THREAD, ThreadTableEntry);
    *ThreadId = pThread->Id;

    return STATUS_SUCCESS;
}

STATUS
SyscallThreadWaitForTermination(
    IN      UM_HANDLE               ThreadHandle,
    OUT     STATUS* TerminationStatus
)
{
    if (ThreadHandle == UM_INVALID_HANDLE_VALUE) {
        return STATUS_INVALID_PARAMETER1;
    }

    if (TerminationStatus == NULL || MmuIsKernelSpace((PVOID) TerminationStatus)) {
        return STATUS_INVALID_PARAMETER2;
    }

    PPROCESS pProcess = GetCurrentProcess();

    INTR_STATE dummyState;
    LockAcquire(&pProcess->ThreadTableLock, &dummyState);
    PHASH_ENTRY threadEntry = HashTableLookup(&pProcess->ThreadTable, (PHASH_KEY)&ThreadHandle);
    LockRelease(&pProcess->ThreadTableLock, dummyState);

    if (threadEntry == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    PTHREAD pThread = CONTAINING_RECORD(threadEntry, THREAD, ThreadTableEntry);

    if (pThread->State == ThreadStateDying) {
        return STATUS_UNSUCCESSFUL;
    }

    LOGL("Termination %X", TerminationStatus);

    ThreadWaitForTermination(pThread, TerminationStatus);

    return STATUS_SUCCESS;
}

STATUS
SyscallThreadCloseHandle(
    IN      UM_HANDLE               ThreadHandle
)
{
    if (ThreadHandle == UM_INVALID_HANDLE_VALUE) {
        return STATUS_INVALID_PARAMETER1;
    }

    PPROCESS pProcess = GetCurrentProcess();

    INTR_STATE dummyState;
    LockAcquire(&pProcess->ThreadTableLock, &dummyState);
    PHASH_ENTRY threadEntry = HashTableLookup(&pProcess->ThreadTable, (PHASH_KEY)&ThreadHandle);
    LockRelease(&pProcess->ThreadTableLock, dummyState);

    if (threadEntry == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    PTHREAD pThread = CONTAINING_RECORD(threadEntry, THREAD, ThreadTableEntry);

    LockAcquire(&pProcess->ThreadTableLock, &dummyState);
    HashTableRemove(&pProcess->ThreadTable, (PHASH_KEY)&ThreadHandle);
    LockRelease(&pProcess->ThreadTableLock, dummyState);

    ThreadCloseHandle(pThread);

    return STATUS_SUCCESS;
}

STATUS
SyscallProcessExit(
    IN      STATUS                  ExitStatus
    )
{
    PPROCESS PProcess = GetCurrentProcess();
    PProcess->TerminationStatus = ExitStatus;
    ProcessTerminate(PProcess);
    return PProcess->TerminationStatus;
}

//
//STATUS
//SyscallProcessGetPid(
//    IN_OPT  UM_HANDLE               ProcessHandle,
//    OUT     PID*                    ProcessId
//)
//{
//    return STATUS_SUCCESS;
//}

//STATUS
//SyscallFileCreate(
//    IN_READS_Z(PathLength)
//    char* Path,
//    IN          QWORD                   PathLength,
//    IN          BOOLEAN                 Directory,
//    IN          BOOLEAN                 Create,
//    OUT         UM_HANDLE* FileHandle
//)
//{
//    return SyscallEntry(SyscallIdFileCreate, Path, PathLength, Directory, Create, FileHandle);
//}

STATUS
SyscallProcessCreate(
    IN_READS_Z(PathLength)
    char* ProcessPath,
    IN          QWORD               PathLength,
    IN_READS_OPT_Z(ArgLength)
    char* Arguments,
    IN          QWORD               ArgLength,
    OUT         UM_HANDLE* ProcessHandle
)
{
    UNREFERENCED_PARAMETER(ArgLength);
    char absolutePath[MAX_PATH];
    PPROCESS pCurrentProcess = GetCurrentProcess();
    PPROCESS pChildProcess;

    if (ProcessPath == NULL) {
        return STATUS_INVALID_PARAMETER1;
    }

    if (PathLength <= 0) {
        return STATUS_INVALID_PARAMETER2;
    }

    if (ProcessPath == strrchr(ProcessPath, '\\')) {
        sprintf(absolutePath, "C:\\Applications\\%s", ProcessPath);
    }
    else {
        strcpy(absolutePath, ProcessPath);
    }

    LOG("gsfsdfsf\n");
    STATUS processCreateStatus = ProcessCreate(absolutePath, Arguments, &pChildProcess);

    if (SUCCEEDED(processCreateStatus))
    {

        pCurrentProcess->CurrentMaximumHandle++;
        pChildProcess->ProcessHandle = pCurrentProcess->CurrentMaximumHandle;
        *ProcessHandle = pCurrentProcess->CurrentMaximumHandle;

        LOG("CURRENT PROCESS STATUS %U\n", pCurrentProcess->TerminationStatus);
        LOG("CHILD PROCESS STATUS %U\n", pChildProcess->TerminationStatus);
        HashTableInsert(&pCurrentProcess->ChildProcessTable, &pChildProcess->ChildProcessEntry);
        LOG("CURRENT PROCESS STATUS %U\n", pCurrentProcess->TerminationStatus);
        LOG("CHILD PROCESS STATUS %U\n", pChildProcess->TerminationStatus);
        return STATUS_SUCCESS;
    }
    return processCreateStatus;
}


STATUS
SyscallFileWrite(
    IN  UM_HANDLE                   FileHandle,
    IN_READS_BYTES(BytesToWrite)
    PVOID                           Buffer,
    IN  QWORD                       BytesToWrite,
    OUT QWORD* BytesWritten
)
{
    UNREFERENCED_PARAMETER(BytesWritten);
    UNREFERENCED_PARAMETER(BytesToWrite);
    UNREFERENCED_PARAMETER(Buffer);
    *BytesWritten = strlen((char*)Buffer) + 1;
    //(char*)Buffer[*BytesToWrite]  
    //    strlen == bytetowrite
    //    bufer not null
    //    poz BytesToWrite sa fie 0
    if (FileHandle == UM_FILE_HANDLE_STDOUT) {
        LOG("[%s]:[%s]\n", ProcessGetName(NULL), Buffer);
        return STATUS_SUCCESS;
    }

    return STATUS_NO_HANDLING_REQUIRED;
}