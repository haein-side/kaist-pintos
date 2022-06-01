#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */


#define NICE_DEFAULT 0
#define RECENT_CPU_DEFAULT 0
#define LOAD_AVG_DEFAULT 0


/* --- project 2: system call --- */

#define FDT_PAGES 3
#define FDCOUNT_LIMIT FDT_PAGES *(1<<9) // limit fdidx


/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* 쓰레드 ID (Thread identifier.) */
	enum thread_status status;          /* 쓰레드 상태 (Thread state.) */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */
	int64_t wakeup_tick; 				/* 깨어나야 할 tick */

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */

	/* priority donation */
	int init_priority; 					/* 우선순위를 donation 받을 때, 자신의 원래 우선 순위를 저장할 수 있는 필드 */
	struct lock *wait_on_lock;			/* 현재 쓰레드가 필요한 lock을 들고 있는 쓰레드의 주소를 저장하는 필드 */
	
	/* multiple priority를 구조체 선언 */
	struct list donations; 				/* 자신에게 priority를 donate한 쓰레드의 리스트 */
	struct list_elem donation_elem;  	/* priority를 donate한 쓰레드들의 리스트를 관리하기 위한 element 
										이 element를 통해 자신이 우선 순위를 donate한 쓰레드의 donates 리스트에 연결*/

	int nice;
	int recent_cpu;
	
#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */

	/* User programs - system call */
	int exit_status; // _exit(), _wait() 구현 때 사용: exit에서 인자status에 exit_status를 넣어주고 thread_exit() 실행
	struct file **file_descriptor_table; // FDT 스레드마다 있는 파일 디스크립터를 관리하는 테이블 
										 // palloc으로 동적 메모리 할당받는데, 핀토스에는 힙 섹션이 없으므로 커널 메모리에 위치
										 // 최대 64개의 파일 객체 포인터를 가짐
	int fdidx; // fd index 파일에 대한 인덱스 값
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

void thread_sleep(int64_t ticks);				// 실행중인 쓰레드를 슬립으로 바꿈
void thread_awake(int64_t ticks);				// sleep_list에서 깨워야할 쓰레드를 깨움
void update_next_tick_to_awake(int64_t ticks); // 최소 tick을 가진 쓰레드 저장
int64_t get_next_tick_to_awake(void);		   // thread.c의 next_tick_to_awake 반환

/* project1 : prority scheduling */
void test_max_priority(void);
bool cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);


/* project1 : priority donation */
void donate_priority(void);
void remove_with_lock(struct lock *lock); 
void refresh_priority(void);


/* advanced scheduling */
void mlfqs_priority (struct thread *t);
void mlfqs_recent_cpu (struct thread *t);
void mlfqs_load_avg (void);
void mlfqs_increment_recent_cpu (void);
void mlfqs_recalc_recent_cpu (void);
void mlfqs_recalc_priority (void);
#endif /* threads/thread.h */