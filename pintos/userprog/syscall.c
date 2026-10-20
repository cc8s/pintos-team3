#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

void
exit_with_status (int status) {
	thread_current ()->exit_status = status;
	thread_exit ();
}

static void
check_address (const void *addr) {
	/* addr이 NULL이면 exit */
	if (addr == NULL) {
		exit_with_status (-1);
	}
	/* addr이 유저 영역이 아니면 exit */
	else if (!is_user_vaddr (addr)) {
		exit_with_status (-1);
	}
	/* addr이 매핑되지 않았으면 exit */
	else if (pml4_get_page (thread_current ()->pml4, addr) == NULL) {
		exit_with_status (-1);
	}
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	int syscall_no = f->R.rax; /* 시스템콜번호 꺼냄(if문으로 각각 매핑) */

	if (syscall_no == SYS_WRITE) {
		struct thread *cur = thread_current ();
		int fd = f->R.rdi;
		const char *buffer = (const char *) f->R.rsi;
		int size = f->R.rdx;
		check_address (buffer);

		if (fd == 1) {
			putbuf (buffer, size);
		}
		if (fd < 2 || fd >= 64 || cur->fd_table[fd] == NULL) {
			f->R.rax = -1;
			return;
		}
		f->R.rax = file_write (cur->fd_table[fd], buffer, size);
	} 
	else if (syscall_no == SYS_EXIT) {
		exit_with_status((int) f->R.rdi); /* rdi에서 종료 코드를 꺼내 현재 스레드의 필드에 저장 */
	}
	else if (syscall_no == SYS_HALT) {
		power_off ();
	} 
	else if (syscall_no == SYS_CREATE) {
	
	const char *file = (const char *) f->R.rdi;
	unsigned initial_size = (unsigned) f->R.rsi;
	/* 유저사이드의 잘못된 요청 check */
	check_address (file);
	f->R.rax = filesys_create (file, initial_size);
	} 
	else if (syscall_no == SYS_OPEN) {
		const char *file_name = (const char *) f->R.rdi;
		check_address (file_name);

		struct file *opened = filesys_open (file_name);
		if (opened == NULL) {
			f->R.rax = -1;
			return;
		}

		struct thread *cur = thread_current ();
		for (int i = 2; i < 64; i++) {
			if (cur->fd_table[i] == NULL) {
				cur->fd_table[i] = opened;;
				f->R.rax = i;
				return;
			}
		}
		file_close (opened);
		f->R.rax = -1;
	}
	else if (syscall_no == SYS_CLOSE) {
		int fd = (int) f->R.rdi;
		struct thread *cur = thread_current ();

		if (fd < 2 || fd >= 64) {
			return;
		}
		else if (cur->fd_table[fd] == NULL) {
			return;
		}
		file_close (cur->fd_table[fd]);
		cur->fd_table[fd] = NULL;
	}
	else if (syscall_no == SYS_READ) {
		struct thread *cur = thread_current ();
		int fd = f->R.rdi;
		const char *buffer = (const char *) f->R.rsi;
		int size = f->R.rdx;
		
		if (fd < 2 || fd >= 64 || cur->fd_table[fd] == NULL) {
			f->R.rax = -1;
			return;
		}
		check_address (buffer);

		f->R.rax = file_read (cur->fd_table[fd], buffer, size);
	}
	else if (syscall_no == SYS_FILESIZE) {
	struct thread *cur = thread_current ();
	int fd = (int) f->R.rdi;

	if (fd < 2 || fd >= 64 || cur->fd_table[fd] == NULL) {
		f->R.rax = -1;
		return;
	}
	f->R.rax = file_length (cur->fd_table[fd]);
	}
	else {
		thread_exit ();
	}
}
