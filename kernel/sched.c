#include <kernel/sched.h>

#include <arch/x86_64/cpu/percpu.h>
#include <arch/x86_64/cpu/apic.h>
#include <arch/x86_64/gdt.h>

#include <include/printk.h>
#include <include/spinlock.h>
#include <include/kmalloc.h>
#include <mm/vmalloc.h>

#include <string.h>
#include <stddef.h>

#define SCHED_STACK_SIZE (16ULL * 1024ULL)
#define SCHED_LAPIC_FALLBACK_COUNT 1000000U

typedef enum {
    TASK_RUNNABLE = 0,
    TASK_ZOMBIE,
} task_state_t;

typedef struct sched_task {
    interrupt_frame *frame;
    void *stack_base;
    size_t stack_size;
    task_state_t state;
    struct sched_task *next;
} sched_task_t;

typedef struct {
    sched_task_t bootstrap;
    sched_task_t *current;
    sched_task_t *runq_head;
    sched_task_t *runq_tail;
    uint64_t ticks;
} sched_cpu_t;

static sched_cpu_t g_sched_cpus[MAX_CPUS];
static spinlock_t g_sched_lock = SPINLOCK_INIT;
static uint32_t g_sched_lapic_init_count;
static volatile int g_sched_ready;

static void runq_push(sched_cpu_t *cpu, sched_task_t *task) {
    task->next = NULL;
    if (!cpu->runq_head) {
        cpu->runq_head = task;
        cpu->runq_tail = task;
        return;
    }
    cpu->runq_tail->next = task;
    cpu->runq_tail = task;
}

static sched_task_t *runq_pop(sched_cpu_t *cpu) {
    sched_task_t *task = cpu->runq_head;
    if (!task) {
        return NULL;
    }
    cpu->runq_head = task->next;
    if (!cpu->runq_head) {
        cpu->runq_tail = NULL;
    }
    task->next = NULL;
    return task;
}

__attribute__((noreturn))
static void sched_thread_trampoline(sched_entry_t entry, void *arg) {
    __asm__ volatile("sti");
    entry(arg);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void sched_init_cpu(uint32_t cpu_id) {
    sched_cpu_t *scpu = &g_sched_cpus[cpu_id];
    memset(scpu, 0, sizeof(*scpu));
    scpu->bootstrap.state = TASK_RUNNABLE;
    scpu->current = &scpu->bootstrap;
}

int sched_is_ready(void) {
    return g_sched_ready != 0;
}

void sched_init(void) {
    if (g_sched_ready) {
        return;
    }

    if (g_cpu_count == 0) {
        g_cpu_count = 1;
    }

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        sched_init_cpu(i);
    }

    uint32_t init_count = 0;
    if (!x86_lapic_timer_calibrate(SCHED_TICK_HZ, &init_count)) {
        init_count = SCHED_LAPIC_FALLBACK_COUNT;
        safe_printk("[sched] LAPIC timer calibration failed; using fallback count=%u\n",
               init_count);
    }
    g_sched_lapic_init_count = init_count;

    if (!x86_lapic_timer_start(SCHED_TICK_VECTOR, g_sched_lapic_init_count, true)) {
        safe_printk("[sched] LAPIC timer start failed on BSP\n");
    }

    g_sched_ready = 1;
}

void sched_ap_init(void) {
    cpu_t *cpu = cpu_self();
    if (cpu->cpu_id < g_cpu_count) {
        if (!g_sched_cpus[cpu->cpu_id].current) {
            sched_init_cpu(cpu->cpu_id);
        }
    }

    if (g_sched_lapic_init_count == 0) {
        g_sched_lapic_init_count = SCHED_LAPIC_FALLBACK_COUNT;
    }

    if (!x86_lapic_timer_start(SCHED_TICK_VECTOR, g_sched_lapic_init_count, true)) {
        safe_printk("[sched] LAPIC timer start failed on AP %u\n", cpu->cpu_id);
    }
}

interrupt_frame *sched_on_tick(interrupt_frame *frame) {
    cpu_t *cpu = cpu_self();
    if (cpu->cpu_id >= MAX_CPUS) {
        return frame;
    }

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    sched_cpu_t *scpu = &g_sched_cpus[cpu->cpu_id];
    scpu->ticks++;

    if (!scpu->current) {
        scpu->current = &scpu->bootstrap;
    }

    scpu->current->frame = frame;

    if (!scpu->runq_head) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return frame;
    }

    sched_task_t *next = runq_pop(scpu);
    if (!next || next == scpu->current || next->frame == NULL) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return frame;
    }

    if (scpu->current->state == TASK_RUNNABLE) {
        runq_push(scpu, scpu->current);
    }

    scpu->current = next;
    interrupt_frame *next_frame = next->frame;
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return next_frame;
}

int sched_spawn(sched_entry_t entry, void *arg) {
    if (!entry) {
        return -1;
    }

    sched_task_t *task = (sched_task_t *)kmalloc(sizeof(*task));
    if (!task) {
        return -1;
    }

    void *stack = vmalloc(SCHED_STACK_SIZE, VMALLOC_DEFAULT_FLAGS);
    if (!stack) {
        kfree(task);
        return -1;
    }

    uintptr_t top = (uintptr_t)stack + SCHED_STACK_SIZE;
    interrupt_frame *frame = (interrupt_frame *)(top - sizeof(interrupt_frame));
    memset(frame, 0, sizeof(*frame));

    frame->rip = (uint64_t)(uintptr_t)sched_thread_trampoline;
    frame->cs = X86_GDT_CODE_SEGMENT;
    frame->rflags = 0x202ULL;
    frame->rdi = (uint64_t)(uintptr_t)entry;
    frame->rsi = (uint64_t)(uintptr_t)arg;

    task->frame = frame;
    task->stack_base = stack;
    task->stack_size = SCHED_STACK_SIZE;
    task->state = TASK_RUNNABLE;
    task->next = NULL;

    cpu_t *cpu = cpu_self();
    if (cpu->cpu_id >= MAX_CPUS) {
        vfree(stack, SCHED_STACK_SIZE);
        kfree(task);
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    runq_push(&g_sched_cpus[cpu->cpu_id], task);
    spin_unlock_irqrestore(&g_sched_lock, flags);

    return 0;
}
