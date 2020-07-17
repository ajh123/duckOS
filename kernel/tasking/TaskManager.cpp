/*
    This file is part of duckOS.

    duckOS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    duckOS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with duckOS.  If not, see <https://www.gnu.org/licenses/>.

    Copyright (c) Byteduck 2016-2020. All rights reserved.
*/

#include <kernel/tasking/TaskManager.h>
#include <kernel/kstddef.h>
#include <kernel/kstdio.h>
#include <kernel/kmain.h>
#include <kernel/interrupt/irq.h>
#include <kernel/pit.h>

TSS TaskManager::tss;

Process *current_proc;
Process *kidle_proc;

uint32_t __cpid__ = 0;
bool tasking_enabled = false;

void kidle(){
	tasking_enabled = true;
	while(1) {
		asm volatile("sti");
		asm volatile("hlt");
	}
}

Process* TaskManager::process_for_pid(pid_t pid){
	Process *current = kidle_proc;
	do{
		if(current->pid() == pid && current->state != PROCESS_DEAD) return current;
		current = current->next;
	} while(current != kidle_proc);
	return (Process *) nullptr;
}

void TaskManager::print_tasks(){
	Process *current = kidle_proc;
	printf("Running processes: ([PID] name state usermem)\n");
	do {
		printf("[%d] '%s' %d %dKiB\n", current->pid(), current->name().c_str(), current->state, current->ring == 3 ? current->page_directory->used_pmem() : 0);
		current = current->next;
	} while(current != kidle_proc);
}

bool& TaskManager::enabled(){
	return tasking_enabled;
}

pid_t TaskManager::get_new_pid(){
	return ++__cpid__;
}

void TaskManager::init(){
	//Create kidle process
	kidle_proc = Process::create_kernel("kidle", kidle);

	//Create kinit process
	auto* kinit_proc = Process::create_kernel("kinit", kmain_late);

	//Link processes together manually
	kidle_proc->next = kinit_proc;
	kidle_proc->prev = kinit_proc;
	kinit_proc->next = kidle_proc;
	kinit_proc->prev = kidle_proc;

	//Preempt
	current_proc = kidle_proc;
	preempt_init_asm(current_proc->registers.esp);
}

Process* TaskManager::current_process(){
	return current_proc;
}

uint32_t TaskManager::add_process(Process *p){
	ASSERT(tasking_enabled)
	tasking_enabled = false;
	p->next = current_proc->next;
	p->next->prev = p;
	p->prev = current_proc;
	current_proc->next = p;
	tasking_enabled = true;
	return p->pid();
}

void TaskManager::notify_current(uint32_t sig){
	current_proc->kill(sig);
}

void TaskManager::kill(Process* p){
	if(process_for_pid(p->pid()) != NULL){
		tasking_enabled = false;
		p->prev->next = p->next;
		p->next->prev = p->prev;
		p->state = PROCESS_DEAD;
		delete p;
		tasking_enabled = true;
	}
}

Process *TaskManager::next_process() {
	Process* next_proc = current_proc->next;

	//Don't switch to a yielding process
	while(next_proc->is_yielding()) {
		next_proc = next_proc->next;
	}

	//Cleanup dead processes
	while(next_proc->state == PROCESS_DEAD) {
		next_proc->prev->next = next_proc->next;
		next_proc->next->prev = next_proc->prev;
		Process* new_next_proc = next_proc->next;
		delete next_proc;
		do next_proc = new_next_proc; while(next_proc->state == PROCESS_DEAD); //Make sure not to choose a dead process
	}

	return next_proc;
}


static uint8_t quantum_counter = 0;

void TaskManager::yield() {
	ASSERT(!Interrupt::in_interrupt())
	quantum_counter = 0;
	preempt_now_asm();
}

void TaskManager::preempt(){
	if(!tasking_enabled) return;

	//Handle pending signal
	Process *current = kidle_proc->next;
	do {
		current->handle_pending_signal();
		current = current->next;
	} while(current != kidle_proc);

	//If it's time to switch, switch
	if(quantum_counter == 0) {
		//Pick a new process and decrease the quantum counter
		auto old_proc = current_proc;
		current_proc = next_process();
		quantum_counter = current_proc->quantum - 1;

		//If we were just in a signal handler, use the esp from signal_registers
		unsigned int* old_esp;
		if(old_proc->in_signal_handler()) {
			old_esp = &old_proc->signal_registers.esp;
		} else {
			old_esp = &old_proc->registers.esp;
		}

		//If we just finished handling a signal, set in_signal_handler to false.
		if(old_proc->just_finished_signal()) {
			old_proc->just_finished_signal() = false;
			old_proc->in_signal_handler() = false;
		}

		//If we're about to start handling a signal, set in_signal_handler to true.
		if(current_proc->ready_to_handle_signal()) {
			current_proc->in_signal_handler() = true;
			current_proc->ready_to_handle_signal() = false;
		}

		//If we're switching to a process in a signal handler, use the esp from signal_registers
		unsigned int* new_esp;
		if(current_proc->in_signal_handler()){
			new_esp = &current_proc->signal_registers.esp;
			tss.esp0 = (size_t) current_proc->signal_stack_top();
		} else {
			new_esp = &current_proc->registers.esp;
			tss.esp0 = (size_t) current_proc->kernel_stack_top();

		}

		//Switch the stacks.
		preempt_asm(old_esp, new_esp, current_proc->page_directory_loc);
	} else {
		quantum_counter--;
	}
}
