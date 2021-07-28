#include <brutal/host/log.h>
#include <brutal/log.h>
#include <syscalls/syscalls.h>
#include "kernel/domain.h"
#include "kernel/memory.h"
#include "kernel/sched.h"
#include "kernel/syscalls.h"
#include "kernel/task.h"

BrResult sys_log(BrLogArgs *args)
{
    host_log_lock();
    print(host_log_writer(), "{}({}) ", str_cast(&task_self()->name), task_self()->handle);
    io_write(host_log_writer(), args->message, args->size);
    host_log_unlock();

    return BR_SUCCESS;
}

BrResult sys_debug(BrDebugArgs *args)
{
    log("{}", args->val);
    return BR_SUCCESS;
}

BrResult sys_map(BrMapArgs *args)
{
    Space *space = nullptr;
    MemObj *mem_obj = nullptr;
    BrResult result = BR_SUCCESS;

    if (args->space == BR_SPACE_SELF)
    {
        space_ref(task_self()->space);
        space = task_self()->space;
    }
    else
    {
        space = (Space *)domain_lookup(task_self()->domain, args->space, OBJECT_SPACE);
    }

    if (space == nullptr)
    {
        result = BR_BAD_HANDLE;
        goto cleanup_and_return;
    }

    mem_obj = (MemObj *)domain_lookup(task_self()->domain, args->mem_obj, OBJECT_MEMORY);

    if (mem_obj == nullptr)
    {
        result = BR_BAD_HANDLE;
        goto cleanup_and_return;
    }

    auto map_result = space_map(space, mem_obj, args->offset, args->size, args->vaddr);

    if (!map_result.success)
    {
        result = map_result._error;
        goto cleanup_and_return;
    }

    args->vaddr = UNWRAP(map_result).base;

cleanup_and_return:
    if (space)
    {
        space_deref(space);
    }

    if (mem_obj)
    {
        mem_obj_deref(mem_obj);
    }

    return result;
}

BrResult sys_unmap(BrUnmapArgs *args)
{
    Space *space = nullptr;

    if (args->space == BR_SPACE_SELF)
    {
        space_ref(task_self()->space);
        space = task_self()->space;
    }
    else
    {
        space = (Space *)domain_lookup(task_self()->domain, args->space, OBJECT_SPACE);
    }

    if (space == nullptr)
    {
        return BR_BAD_HANDLE;
    }

    space_unmap(space, (VmmRange){args->vaddr, args->size});

    space_deref(space);

    return BR_SUCCESS;
}

BrResult sys_create_task(BrTask *handle, BrCreateTaskArgs *args)
{
    Space *space = nullptr;

    if (args->space == BR_SPACE_SELF)
    {
        space_ref(task_self()->space);
        space = task_self()->space;
    }
    else
    {
        space = (Space *)domain_lookup(task_self()->domain, args->space, OBJECT_SPACE);
    }

    if (space == nullptr)
    {
        return BR_BAD_HANDLE;
    }

    auto task = UNWRAP(task_create(
        str_cast(&args->name),
        space,
        args->caps & task_self()->caps,
        args->flags | BR_TASK_USER));

    domain_add(task_self()->domain, base_cast(task));

    space_deref(space);

    *handle = task->handle;

    task_deref(task);

    return BR_SUCCESS;
}

BrResult sys_create_mem_obj(BrMemObj *handle, BrCreateMemObjArgs *args)
{
    MemObj *mem_obj = nullptr;

    if (args->flags & BR_MEM_OBJ_PMM)
    {
        if (!(task_self()->caps & BR_CAP_PMM))
        {
            return BR_BAD_CAPABILITY;
        }

        mem_obj = mem_obj_pmm((PmmRange){args->addr, args->size}, MEM_OBJ_NONE);
    }
    else
    {
        auto pmm_result = pmm_alloc(args->size);

        if (!pmm_result.success)
        {
            return pmm_result._error;
        }

        mem_obj = mem_obj_pmm(UNWRAP(pmm_result), MEM_OBJ_OWNING);
    }

    domain_add(task_self()->domain, base_cast(mem_obj));
    *handle = mem_obj->handle;
    mem_obj_deref(mem_obj);

    return BR_SUCCESS;
}

BrResult sys_create_space(BrSpace *handle, BrCreateSpaceArgs *args)
{
    auto space = space_create(args->flags);

    domain_add(task_self()->domain, base_cast(space));
    *handle = space->handle;
    space_deref(space);

    return BR_SUCCESS;
}

BrResult sys_create(BrCreateArgs *args)
{
    if (!(task_self()->caps & BR_CAP_TASK))
    {
        return BR_BAD_CAPABILITY;
    }

    switch (args->type)
    {
    case BR_OBJECT_TASK:
        return sys_create_task(&args->task_handle, &args->task);

    case BR_OBJECT_SPACE:
        return sys_create_space(&args->space_handle, &args->space);

    case BR_OBJECT_MEMORY:
        return sys_create_mem_obj(&args->mem_obj_handle, &args->mem_obj);

    default:
        return BR_BAD_ARGUMENTS;
    }
}

BrResult sys_start(BrStartArgs *args)
{
    Task *task = (Task *)domain_lookup(task_self()->domain, args->task, OBJECT_TASK);

    if (!task)
    {
        return BR_BAD_HANDLE;
    }

    sched_start(task, args->ip, args->sp, args->args);

    task_deref(task);

    return BR_SUCCESS;
}

BrResult sys_exit(BrExitArgs *args)
{
    Task *task = nullptr;

    if (args->task == BR_TASK_SELF)
    {
        task = task_self();
        task_ref(task);
    }
    else
    {
        task = (Task *)domain_lookup(task_self()->domain, args->task, OBJECT_TASK);
    }

    if (!task)
    {
        return BR_BAD_HANDLE;
    }

    sched_stop(task, args->exit_value);

    task_deref(task);

    return BR_SUCCESS;
}

BrResult sys_ipc(MAYBE_UNUSED BrIpcArgs *args)
{
    return BR_NOT_IMPLEMENTED;
}

BrResult sys_irq(MAYBE_UNUSED BrIrqArgs *args)
{
    if (!(task_self()->caps & BR_CAP_IRQ))
    {
        return BR_BAD_CAPABILITY;
    }

    return BR_NOT_IMPLEMENTED;
}

BrResult sys_drop(BrDropArgs *args)
{
    Task *task = nullptr;

    if (args->task == BR_TASK_SELF)
    {
        task_ref(task_self());
        task = task_self();
    }
    else
    {
        task = (Task *)domain_lookup(task_self()->domain, args->task, OBJECT_TASK);
    }

    if (!task)
    {
        return BR_BAD_HANDLE;
    }

    if (!(task->caps & args->cap))
    {
        task_deref(task);
        return BR_BAD_CAPABILITY;
    }

    task->caps = task->caps & ~args->cap;

    task_deref(task);

    return BR_SUCCESS;
}

BrResult sys_close(BrCloseArgs *args)
{
    domain_remove(task_self()->domain, args->handle);
    return BR_SUCCESS;
}

typedef BrResult BrSyscallFn();

BrSyscallFn *syscalls[BR_SYSCALL_COUNT] = {
    [BR_SC_LOG] = sys_log,
    [BR_SC_DEBUG] = sys_debug,
    [BR_SC_MAP] = sys_map,
    [BR_SC_UNMAP] = sys_unmap,
    [BR_SC_CREATE] = sys_create,
    [BR_SC_START] = sys_start,
    [BR_SC_EXIT] = sys_exit,
    [BR_SC_IPC] = sys_ipc,
    [BR_SC_IRQ] = sys_irq,
    [BR_SC_DROP] = sys_drop,
    [BR_SC_CLOSE] = sys_close,
};

BrResult syscall_dispatch(BrSyscall syscall, BrArg args)
{
    /*
        log("Syscall: {}({}): {}({#p})",
            str_cast(&task_self()->name),
            task_self()->handle,
            str_cast(br_syscall_to_string(syscall)),
            args);
    */

    if (syscall >= BR_SYSCALL_COUNT)
    {
        return BR_BAD_SYSCALL;
    }

    task_begin_syscall();

    auto result = syscalls[syscall](args);

    if (result != BR_SUCCESS)
    {
        log("Syscall: {}({}): {}({#p}) -> {}",
            str_cast(&task_self()->name),
            task_self()->handle,
            str_cast(br_syscall_to_string(syscall)),
            args,
            str_cast(br_result_to_string(result)));
    }

    task_end_syscall();

    return result;
}
