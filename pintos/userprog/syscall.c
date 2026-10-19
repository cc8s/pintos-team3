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
//헤더 파일 추가. by 종건.
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "devices/input.h"

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

//lock 선언. by 종건.
static struct lock filesys_lock;

void
syscall_init (void) {
	lock_init(&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

static void
halt (void) {
	power_off();
}

static void
exit (int status) {
	thread_current ()->exit_status = status; 
	thread_exit ();
}

static int
write (int fd, const void *buffer, unsigned size){
	if (fd == 1) {
		putbuf (buffer, size);
		return size;
	} 
	else {
		thread_exit ();
	}
}
//파일명 체크용 함수. by 종건.
static bool check_user_string (uint64_t adr) {
	struct thread *cur = thread_current();
	while (is_user_vaddr (adr)) {
		const char *p = (const char *) adr;
		if (pml4_get_page (cur->pml4, p) == NULL) {
			return false;
		}
		char ch = *p;
		if (ch == '\0') {
			return true;
		}
		adr++;
	}
	return false;
}

//buffer page 확인. 
void
check_user_buffer (uint64_t buffer, uint64_t size) {
	struct thread *curr = thread_current ();

	if (size == 0) {
		return;
	}

	int i, rest;
	void *check_buffer = (void *) buffer;
	unsigned check_size = (unsigned) size;

	unsigned bytes_left_in_page;
	unsigned pages_remaining = check_size;
	unsigned pages_count;

	bytes_left_in_page = PGSIZE - pg_ofs (check_buffer);
	if (bytes_left_in_page < pages_remaining) {
		pages_remaining -= bytes_left_in_page;
	}
	else {
		pages_remaining = 0;
	}

	if ((pages_remaining % 4096) != 0) {
		rest = 1;
	}
	else {
		rest = 0;
	}

	pages_count = (pages_remaining / 4096) + rest;

	for (i=0; i<=pages_count; i++) {
		uint64_t user_buffer = (uint64_t) check_buffer;
		uint64_t *pte = pml4e_walk (curr->pml4, user_buffer, 0);
		if (!is_user_vaddr (check_buffer) || !pml4_get_page (curr->pml4, check_buffer) || (is_writable (pte) == 0)) {
			exit (-1);
		}
		check_buffer = (char *) pg_round_down (check_buffer) + PGSIZE;
	}

}

//fd_table 연결.
void
syscall_close_files (void) {
	struct thread *cur = thread_current ();
	if (cur->fd == NULL)
		return;

	lock_acquire (&filesys_lock);
	for (int i = 2; i < FD_TABLE_SIZE; i++) {
		if (cur->fd[i] != NULL)
			file_close (cur->fd[i]);
	}
	lock_release (&filesys_lock);

	free (cur->fd);
	cur->fd = NULL;
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
    /* 시스템 콜 번호: rax
	 * 인자 순서 rdi, rsi, rdx, r10, r8, r9 
     */

    switch (f->R.rax) {
        case SYS_HALT:
            halt ();
            break;

        case SYS_EXIT: 
			/* f->R.rdi == status */
			exit((int) f->R.rdi);
            break;

        case SYS_WRITE: 
			/* f->R.rdi == fd,	f->R.rsi == buffer,  f->R.rdx == size */
			f->R.rax = write((int) f->R.rdi, (char *) f->R.rsi, (unsigned) f->R.rdx);
            break;

		case SYS_CREATE: {
			//사용자 주소 검사.
			if (!is_user_vaddr (f->R.rdi)) {
				exit (-1);
			}
			
			struct thread *cur = thread_current();
			if (!pml4_get_page (cur->pml4, (const void *) f->R.rdi)) {
				exit (-1);
			}

			if (check_user_string (f->R.rdi) == false) {
				exit (-1);
			}

			//파일 생성.
			lock_acquire (&filesys_lock);
			f->R.rax = filesys_create ((const char *) f->R.rdi, (unsigned) f->R.rsi);
			lock_release (&filesys_lock);
			break;
		}

		case SYS_OPEN: {
			//사용자 주소 검사. 
			if (!is_user_vaddr (f->R.rdi)) {
				exit (-1);
			}

			struct thread *cur = thread_current();
			if (!pml4_get_page (cur->pml4, (const void *) f->R.rdi)) {
				exit (-1);
			}

			if (check_user_string (f->R.rdi) == false) {
				exit (-1);
			}

			//FD table 연결
			struct file *file;
			int i = 2;
			/* Allocate once per process; later open calls reuse this table. */
			if (cur->fd == NULL) {
				cur->fd = calloc (FD_TABLE_SIZE, sizeof *cur->fd);
				if (cur->fd == NULL) {
					f->R.rax = -1;
					break;
				}
			}

			lock_acquire (&filesys_lock);
			const char *p = (const char *) f->R.rdi;
			file = filesys_open (p);
			lock_release (&filesys_lock);

			if (file == NULL) {
				f->R.rax = -1;
			} else {
				while (i < FD_TABLE_SIZE && cur->fd[i] != NULL) {
					i++;
				}

				if (i < FD_TABLE_SIZE) {
					cur->fd[i] = file;
					f->R.rax = i;
				} else {
					lock_acquire (&filesys_lock);
					file_close (file);
					lock_release (&filesys_lock);
					f->R.rax = -1;
				}
			}
			break;
		}

		case SYS_CLOSE: {
			struct thread *cur = thread_current ();
			
			if (cur->fd == NULL || f->R.rdi >= FD_TABLE_SIZE || cur->fd[f->R.rdi] == NULL) {
				exit (-1);
			}

			lock_acquire (&filesys_lock);
			file_close (cur->fd[f->R.rdi]);
			cur->fd[f->R.rdi] = NULL;
			lock_release (&filesys_lock);
			break;
		}

		case SYS_READ: {
			struct thread *cur = thread_current ();
			
			if (cur->fd == NULL || f->R.rdi >= FD_TABLE_SIZE || cur->fd[f->R.rdi] == NULL) {
				exit (-1);
			}

			struct file *file = cur->fd[f->R.rdi];
			if (cur->fd == NULL
				|| f->R.rdi >= FD_TABLE_SIZE
				|| cur->fd[f->R.rdi] == NULL
				|| f->R.rdi <= 1) {
					exit (-1);
				}

			check_user_buffer (f->R.rsi, f->R.rdx);
			
			lock_acquire (&filesys_lock);
			void *buffer = (void *)f->R.rsi;
			f->R.rax = file_read (file, buffer, f->R.rdx);
			lock_release (&filesys_lock);

			break;
		}

		case SYS_FILESIZE: {

			struct thread *cur = thread_current ();

			if (cur->fd == NULL || f->R.rdi >= FD_TABLE_SIZE || cur->fd[f->R.rdi] == NULL) {
				exit (-1);
			}

			struct file *file = cur->fd[f->R.rdi];

			lock_acquire (&filesys_lock);
			off_t size = file_length (file);
			f->R.rax = size;
			lock_release (&filesys_lock);

			break;
		}

        default:
            exit(-1);
            break;
    }
}
