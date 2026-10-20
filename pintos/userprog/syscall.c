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
#include "threads/synch.h" 
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "threads/palloc.h"

#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/malloc.h"
struct lock file_lock;
void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void exit (int status);

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

/* 주소 == NULL or 유저영역 X or 페이지 테이블(pml4)연결 확인  */
static void 
check_address (void *addr) {
    if (addr == NULL || !is_user_vaddr (addr) || pml4_get_page (thread_current ()->pml4, addr) == NULL) {
        exit (-1); 
    }
}

/* 현재 스레드에 파일 추가 */
static int 
process_add_file (struct file *f) {
    struct thread *curr = thread_current ();
    struct file **fl = curr->file_list;
    /* 첫 할당 시 */
    if (curr->file_list == NULL) {
        curr->fd_capacity = 16;
        curr->file_list = malloc (sizeof(struct file *) * curr->fd_capacity);
        
        if (curr->file_list == NULL) {
            return -1; 
        }
        /* 초기화 */
        for (int i = 0; i < curr->fd_capacity; i++) {
            curr->file_list[i] = NULL;
        }
    }

    /* 빈자리 탐색 */
    for (int i = 2; i < curr->fd_capacity; i++) {
        if (curr->file_list[i] == NULL) {
            curr->file_list[i] = f;
            return i;
        }
    }
    /* 꽉차면 (배열 크기 * 2) */
    int old_cap = curr->fd_capacity;
    int new_cap = old_cap * 2;

    struct file **new_list = realloc (curr->file_list, sizeof(struct file *) * new_cap);
    if (new_list == NULL) {
        return -1; 
    }

    /* 초기화 */
    for (int i = old_cap; i < new_cap; i++) {
        new_list[i] = NULL;
    }

    curr->file_list = new_list;
    curr->fd_capacity = new_cap;

    /* 새로 받은자리에 할당 */
    curr->file_list[old_cap] = f;
    return old_cap;
}

void
syscall_init (void) {
	lock_init (&file_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 종료 */ 
static void
halt (void) {
	power_off();
}

/* exit_status에 종료 상태 저장 후 exit (static 아님)*/
void
exit (int status) {
	thread_current ()->exit_status = status; 
	thread_exit ();
}

/* process_fork후 자식 tid 리턴 */
static tid_t
fork (const char *thread_name, struct intr_frame *f) {
    check_address ((void *) thread_name);
    return process_fork (thread_name, f);
}

/* exec후 return 안함 실패시 return -1 */
static int
exec (const char *file) {
    check_address ((void *) file);
    /* 인자로 받은 파일명+인자(최대128byte)을 사용하기 전에 
     * process_exec에서 process_cleanup하는데 128바이트여도
     * 커널에 계속 쌓인다고 생각하면 배열로 저장하기에는 위험하니 malloc */
    char *temp = palloc_get_page (PAL_ZERO);
    if (temp == NULL) {
        exit (-1);
    }
    strlcpy (temp, file, PGSIZE);

    process_exec (temp);
     
    /* 여기까지 왔으면 return -1 */
    return -1;
}

/* pid종료 대기 */
static int
wait (int pid) {
	return process_wait(pid);
}

/* file create(lock) */
static bool
create (const char *file, unsigned initial_size) {
	check_address ((void *) file);
    lock_acquire (&file_lock); 
    bool success = filesys_create (file, initial_size);
    lock_release (&file_lock);

    return success;
}


/* file remove(lock) */
static bool
remove (const char *file) {
	check_address ((void *) file);
    lock_acquire (&file_lock); 
    bool success = filesys_remove (file);
    lock_release (&file_lock);

    return success;
}

/* file open(lock) */
static int
open (const char *file){
	check_address ((void *) file);
    lock_acquire(&file_lock); 
    struct file *curr_file = filesys_open (file);
    lock_release(&file_lock);

    if (curr_file == NULL) {
        return -1;
    }

    int fd = process_add_file (curr_file);

    if (fd == -1) {
        file_close (curr_file);
    }
	
    return fd;
}

/* file 바이트 반환(file_length) */
static int
filesize (int fd) {
    struct thread *curr = thread_current ();
    if (fd < 2 || curr->file_list == NULL || fd >= curr->fd_capacity) { 
        return -1;
    }
    
    struct file *f = thread_current ()->file_list[fd]; 
    if (f == NULL) {
        return -1;
    }
    
    return file_length (f);
}

/* file 데이터 read */
static int
read (int fd, void *buffer, unsigned size) {
	check_address ((void *) buffer);
    if (fd == 1) {
        return -1;
    }

    if (fd == 0) {
        unsigned int i;
        unsigned char *buf = (unsigned char *) buffer;
        for (i = 0; i < size; i++) {
            buf[i] = input_getc ();
            if (buf[i] == '\0') {
                break;
            }
        }
        return i;
    }

    struct thread *curr = thread_current ();
    if (fd < 2 || curr->file_list == NULL || fd >= curr->fd_capacity || curr->file_list[fd] == NULL) {
        return -1;
    }

    lock_acquire(&file_lock);
    int size_read = file_read (curr->file_list[fd], buffer, size);
    lock_release(&file_lock);

    return size_read;
}


/* file에 데이터 write */
static int
write (int fd, const void *buffer, unsigned size) {
	check_address ((void *) buffer);
    if (fd == 0) {
        return 0;
    }

    if (fd == 1) {
		if (size > 0){
			putbuf (buffer, size);
			return size;
		}
		return 0;
    }

    struct thread *curr = thread_current ();
    if (fd < 2 || curr->file_list == NULL || fd >= curr->fd_capacity || curr->file_list[fd] == NULL) {
        return 0;
    }

    lock_acquire(&file_lock);
    int size_write = file_write (curr->file_list[fd], buffer, size);
    lock_release(&file_lock);

    return size_write;
}

/* file close */
static void
close (int fd) {
    struct thread *curr = thread_current ();

    if (fd < 2 || curr->file_list == NULL || fd >= curr->fd_capacity) {
        return; 
    }

    struct file *curr_file = curr->file_list[fd];
    
    if (curr_file != NULL) {
        lock_acquire(&file_lock);
        file_close (curr_file);
        lock_release(&file_lock);

        curr->file_list[fd] = NULL; 
    }
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

        case SYS_FORK:
            /* f->R.rdi == thread_name, 레지스터 상태 f */
            f->R.rax = fork ((const char *) f->R.rdi, f);
            break;
            
        case SYS_EXEC:
            /* f->R.rdi == char *file */
            f->R.rax = exec ((const char *) f->R.rdi);
            break;
			
		case SYS_WAIT:
            /* f->R.rdi == pid */
            f->R.rax = wait ((int) f->R.rdi);
            break;

		case SYS_CREATE:
			/* f->R.rdi == file, f->R.rsi == initial_size */
			f->R.rax = create ((const char *)f->R.rdi, (unsigned int) f->R.rsi);
			break;

		case SYS_REMOVE:
			/* f->R.rdi == file */
			f->R.rax = remove ((const char *)f->R.rdi);
			break;

		case SYS_OPEN:
			/* f->R.rdi == file */
			f->R.rax = open ((const char *)f->R.rdi);
			break;

		case SYS_FILESIZE:
			/* f->R.rdi == fd */
			f->R.rax = filesize ((int) f->R.rdi);
			break;

		case SYS_READ:
			/* f->R.rdi == fd,	f->R.rsi == buffer,  f->R.rdx == size */
			f->R.rax = read ((int) f->R.rdi, (void *) f->R.rsi, (unsigned int) f->R.rdx);
			break;

        case SYS_WRITE: 
			/* f->R.rdi == fd,	f->R.rsi == buffer,  f->R.rdx == size */
			f->R.rax = write((int) f->R.rdi, (void *) f->R.rsi, (unsigned int) f->R.rdx);
            break;
		
		case SYS_CLOSE:
			/* f->R.rdi == fd */
			close ((int) f->R.rdi);
			break;

        default:
            exit(-1);
            break;
    }
}
