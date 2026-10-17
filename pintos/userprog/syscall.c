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
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

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

		case SYS_CREATE:
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

        default:
            exit(-1);
            break;
    }
}
