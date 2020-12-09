#include <cpu/hal.h>
#include <cpu/idt.h>
#include <cpu/pic.h>
#include <cpu/tss.h>
#include <fs/poll.h>
#include <include/limits.h>
#include <ipc/signal.h>
#include <memory/vmm.h>
#include <system/time.h>
#include <utils/debug.h>

#include "task.h"

extern void irq_task_handler();
extern void do_switch(uint32_t *addr_current_kernel_esp, uint32_t next_kernel_esp, uint32_t cr3);

struct plist_head terminated_list, waiting_list;
struct plist_head kernel_ready_list, system_ready_list, app_ready_list;
uint32_t volatile scheduler_lock_counter = 0;

void lock_scheduler()
{
	disable_interrupts();
	scheduler_lock_counter++;
}

void unlock_scheduler()
{
	scheduler_lock_counter--;
	if (scheduler_lock_counter == 0)
		enable_interrupts();
}

static struct thread *get_next_thread_from_list(struct plist_head *list)
{
	if (plist_head_empty(list))
		return NULL;

	return plist_first_entry(list, struct thread, sched_sibling);
}

static struct thread *get_next_thread_to_run()
{
	struct thread *nt = get_next_thread_from_list(&kernel_ready_list);
	if (!nt)
		nt = get_next_thread_from_list(&system_ready_list);
	if (!nt)
		nt = get_next_thread_from_list(&app_ready_list);

	return nt;
}

static struct thread *pop_next_thread_from_list(struct plist_head *list)
{
	if (plist_head_empty(list))
		return NULL;

	struct thread *th = plist_first_entry(list, struct thread, sched_sibling);
	plist_del(&th->sched_sibling, list);
	return th;
}

static struct thread *pop_next_thread_to_run()
{
	struct thread *nt = pop_next_thread_from_list(&kernel_ready_list);
	if (!nt)
		nt = pop_next_thread_from_list(&system_ready_list);
	if (!nt)
		nt = pop_next_thread_from_list(&app_ready_list);

	return nt;
}

static struct plist_head *get_list_from_thread(enum thread_state state, enum thread_policy policy)
{
	if (state == THREAD_READY)
	{
		if (policy == THREAD_KERNEL_POLICY)
			return &kernel_ready_list;
		else if (policy == THREAD_SYSTEM_POLICY)
			return &system_ready_list;
		else
			return &app_ready_list;
	}
	else if (state == THREAD_WAITING)
		return &waiting_list;
	else if (state == THREAD_TERMINATED)
		return &terminated_list;
	return NULL;
}

int get_top_priority_from_list(enum thread_state state, enum thread_policy policy)
{
	struct plist_head *h = get_list_from_thread(state, policy);

	if (!plist_head_empty(h))
	{
		struct plist_node *node = plist_first(h);
		if (node)
			return node->prio;
	}
	return INT_MAX;
}

void queue_thread(struct thread *th)
{
	struct plist_head *h = get_list_from_thread(th->state, th->policy);

	if (h)
		plist_add(&th->sched_sibling, h);
}

static void remove_thread(struct thread *th)
{
	struct plist_head *h = get_list_from_thread(th->state, th->policy);

	if (h)
		plist_del(&th->sched_sibling, h);
}

void update_thread(struct thread *th, uint8_t state)
{
	if (th->state == state)
		return;

	lock_scheduler();

	remove_thread(th);
	th->state = state;
	queue_thread(th);

	unlock_scheduler();
}

static void switch_thread(struct thread *nt)
{
	if (current_thread == nt)
	{
		current_thread->time_slice = 0;
		update_thread(current_thread, THREAD_RUNNING);
		return;
	}

	struct thread *pt = current_thread;

	current_thread = nt;
	current_thread->time_slice = 0;
	update_thread(current_thread, THREAD_RUNNING);
	current_process = current_thread->parent;

	uint32_t paddr_cr3 = vmm_get_physical_address((uint32_t)current_thread->parent->pdir, true);
	tss_set_stack(0x10, current_thread->kernel_stack);
	do_switch(&pt->esp, current_thread->esp, paddr_cr3);
}

void schedule()
{
	if (current_thread->state == THREAD_RUNNING)
		return;

	lock_scheduler();

	struct thread *nt = pop_next_thread_to_run();
	if (!nt)
	{
		do
		{
			unlock_scheduler();
			halt();
			lock_scheduler();
			nt = pop_next_thread_to_run();
			// NOTE: MQ 2020-06-14
			// Normally, current_thread shouldn't be running because we update state before calling schedule
			// If current thread is running and no next thread
			// -> it get interrupted by network which switch to net_thread, in net_rx_loop we switch back
			if (!nt && current_thread->state == THREAD_RUNNING)
				nt = current_thread;
		} while (!nt);
	}
	switch_thread(nt);

	if (current_thread->pending && !(current_thread->flags & TIF_SIGNAL_MANUAL))
	{
		struct interrupt_registers *regs = (struct interrupt_registers *)(current_thread->kernel_stack - sizeof(struct interrupt_registers));
		handle_signal(regs, current_thread->blocked);
	}
	unlock_scheduler();
}

#define SLICE_THRESHOLD 8
int32_t irq_schedule_handler(struct interrupt_registers *regs)
{
	if (current_thread->policy != THREAD_APP_POLICY || current_thread->state != THREAD_RUNNING)
		return IRQ_HANDLER_CONTINUE;

	lock_scheduler();

	bool is_schedulable = false;
	current_thread->time_slice++;

	if (current_thread->time_slice >= SLICE_THRESHOLD)
	{
		struct thread *nt = get_next_thread_to_run();

		if (nt)
		{
			// 1. if next thread to run is not app policy -> step 4
			// 2. scale them to zero and set current thead to last + 1
			// 3. update thread
			if (nt->policy == THREAD_APP_POLICY)
			{
				struct thread *first_thd = plist_first_entry(&app_ready_list, struct thread, sched_sibling);
				struct thread *last_thd = plist_last_entry(&app_ready_list, struct thread, sched_sibling);
				int scale = first_thd->sched_sibling.prio;

				struct thread *iter;
				plist_for_each_entry(iter, &app_ready_list, sched_sibling)
				{
					iter->sched_sibling.prio -= scale;
				}
				current_thread->sched_sibling.prio = last_thd->sched_sibling.prio + 1;
			}
			update_thread(current_thread, THREAD_READY);
			is_schedulable = true;
		}
	}

	unlock_scheduler();

	// NOTE: MQ 2019-10-15 If counter is 1, it means that there is not running scheduler
	if (is_schedulable && !scheduler_lock_counter)
	{
		log("Scheduler: Round-robin for %d", current_thread->tid);
		schedule();
	}

	return IRQ_HANDLER_CONTINUE;
}

int32_t thread_page_fault(struct interrupt_registers *regs)
{
	uint32_t faultAddr = 0;
	__asm__ __volatile__("mov %%cr2, %0"
						 : "=r"(faultAddr));

	if (regs->cs == 0x1B)
	{
		log("Page Fault: From userspace at 0x%x", faultAddr);
		if (faultAddr == PROCESS_TRAPPED_PAGE_FAULT)
			do_exit(regs->eax);
		else if (faultAddr == (uint32_t)sigreturn)
			sigreturn(regs);

		return IRQ_HANDLER_STOP;
	}

	assert_not_reached();
	return IRQ_HANDLER_CONTINUE;
}

void wake_up(struct wait_queue_head *hq)
{
	struct wait_queue_entry *iter, *next;
	list_for_each_entry_safe(iter, next, &hq->list, sibling)
	{
		iter->func(iter->thread);
	}
}

void sched_init()
{
	plist_head_init(&kernel_ready_list);
	plist_head_init(&system_ready_list);
	plist_head_init(&app_ready_list);
	plist_head_init(&waiting_list);
	plist_head_init(&terminated_list);
}
