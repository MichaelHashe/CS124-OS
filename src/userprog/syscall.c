#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <stdbool.h>


#include "devices/input.h"    /* For inpute_getc. */
#include "devices/shutdown.h" /* For halt. */
#include "filesys/file.h"     /* For file ops. */
#include "filesys/filesys.h"  /* For filesys ops. */
#include "threads/malloc.h"   /* For malloc. */
#include "filesys/off_t.h"    /* For off_t. */
#include "threads/synch.h"    /* For locks. */


/* Handler function. */
static void syscall_handler(struct intr_frame *);


/* Helper functions. */
static uint32_t get_arg(struct intr_frame *f, int offset);
static struct file_des *file_from_fd(int fd);


/* Handlers for Project 4. */
static void     halt(struct intr_frame *f);
static void     exec(struct intr_frame *f);
static void     wait(struct intr_frame *f);
static void   create(struct intr_frame *f);
static void   remove(struct intr_frame *f);
static void     open(struct intr_frame *f);
static void filesize(struct intr_frame *f);
static void     read(struct intr_frame *f);
static void    write(struct intr_frame *f);
static void     seek(struct intr_frame *f);
static void     tell(struct intr_frame *f);
static void    close(struct intr_frame *f);


/* Handlers for Project 5. */
static void     mmap(struct intr_frame *f);
static void   munmap(struct intr_frame *f);


/* Max of two numbers. */
#define MAX(a,b) (((a) > (b)) ? (a) : (b))


/*! Lock for performing filesystems operations. */
static struct lock filesys_io;


/* Module init function. */
void syscall_init(void) {
    /* Register syscall handler. */
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");

    /* Initialize lock. */
    lock_init(&filesys_io);
}


/* Checks if given pointer is in user space and in a mapped address. */
uint32_t* verify_pointer(uint32_t* p) {
    if (is_user_vaddr(p) && pagedir_get_page(thread_current()->pagedir, p)) {
        /* Valid pointer, continue. */
        return p;
    }
    /* Invalid pointer, exit. */
    thread_exit();
}


/* Entry point for syscalls. */
static void syscall_handler(struct intr_frame *f) {
    /* Make sure top of variable is valid. */
    verify_pointer(((uint32_t*)f->esp) + 1);
    uint32_t *stack = (uint32_t*)f->esp;
    if (!stack)  {
        thread_exit();
    }

    /* Dispatch syscall to appropriate handler. */
    int syscall_num =  *(stack);
    switch(syscall_num) {
        /* Syscalls for Project 4. */
        case SYS_HALT :     halt(f);     break;     /* 0 */
        case SYS_EXIT :     exit(f);     break;     /* 1 */
        case SYS_EXEC :     exec(f);     break;     /* 2 */
        case SYS_WAIT :     wait(f);     break;     /* 3 */
        case SYS_CREATE :   create(f);   break;     /* 4 */
        case SYS_REMOVE :   remove(f);   break;     /* 5 */
        case SYS_OPEN :     open(f);     break;     /* 6 */
        case SYS_FILESIZE : filesize(f); break;     /* 7 */
        case SYS_READ :     read(f);     break;     /* 8 */
        case SYS_WRITE :    write(f);    break;     /* 9 */
        case SYS_SEEK :     seek(f);     break;     /* 10 */
        case SYS_TELL :     tell(f);     break;     /* 11 */
        case SYS_CLOSE :    close(f);    break;     /* 12 */

        /* Syscalls for Project 5. */
        case SYS_MMAP :     mmap(f);     break;     /* 13 */
        case SYS_MUNMAP :   munmap(f);   break;     /* 14 */

        /* Invalid syscall. */
        default : thread_exit();
    }
}


/* Gets an argument off the stack. Verifies access. Offset is the arg number. */
static uint32_t get_arg(struct intr_frame *f, int offset) {
    /* We only handle syscalls with <= 3 arguments. */
    ASSERT(offset <= 3);
    ASSERT(offset >= 0);
    ASSERT(f);

    /* Obtain stack pointer. */
    uint32_t *stack = (uint32_t*)f->esp;

    /* Find argument and ensure validity. */
    stack += offset;
    verify_pointer(stack + 1);

    /* Return objectas uint32_t. */
    return *stack;
}


/* Each thread maintains a list of its file descriptors. Get the file object 
   associated with the file descriptor. */
static struct file_des *file_from_fd(int fd) {
    /* 0, 1 reserved for STDIN, STDOUT. */
    ASSERT(fd > STDIN_FILENO);
    ASSERT(fd > STDOUT_FILENO);

    /* Iterate over file descriptors, return if one matches. */
    struct list_elem *e;
    struct list *lst = &thread_current()->fds;
    struct file_des* fd_s;

    for (e = list_begin(lst); e != list_end(lst); e = list_next(e)) {
        /* File descriptor object. */
        fd_s = list_entry(e, struct file_des, elem);

        /* If a match, return. */
        if (fd_s->fd == fd) {
            return fd_s;
        }
    }

    /* Invalid file descriptor; terminate offending process. */
    thread_exit();
}

/* Terminates Pintos by calling shutdown_power_off(). */
static void halt(struct intr_frame *f UNUSED) {
    /* Terminate Pintos. */
    shutdown_power_off();
}

/* Terminates the current user program, returning status to the kernel. If the
   process's parent waits for it (see below), this is the status that will be
   returned. */
static void exit(struct intr_frame *f) {
    /* Parse arguments. */
    int status = get_arg(f, 1);

    /* If process has no parent, just exit. */
    struct thread *parent = thread_get_from_tid(thread_current()->parent_tid);
    if (!parent) {
        thread_exit();
    }

    /* Parent keeps track of object representing child thread. */
    struct child *c = thread_get_child_elem(&parent->children, 
                                            thread_current()->tid);
    ASSERT(c);

    /* Let parent know of our exit status. */
    c->exit_code = status;

    /* Keep track of our own exit status. */
    thread_current()->exit_code = status;

    /* Terminate program. */
    thread_exit();
}


/* Runs the executable whose name is given in cmd_line, passing any given
   arguments, and returns the new process's program id (pid). Returns -1
   in case of error. */
static void exec(struct intr_frame *f) {
    /* Parse arguments. */
    const char* cmd_line = (const char*) get_arg(f, 1);

    /* Verify arguments. */
    verify_pointer((uint32_t *) cmd_line);

    /* Exec program. */
    f->eax = process_execute(cmd_line);

    // Very unusual behavior...
    // process_wait(f->eax); // Comment in if VM has multiple processors.

    /* Check load_success flag. */
    sema_down(&thread_current()->success_sema);
    struct child *c = thread_get_child_elem(&thread_current()->children, 
                                            f->eax);
    if (!c->load_success) {
        f->eax = -1;
    }

}

/* Waits for a child process pid and retrieves the child's exit status. If pid 
   is still alive, waits until it terminates. Then, returns the status that pid
   passed to exit. If pid did not call exit(), but was terminated by the kernel
   (e.g. killed due to an exception), wait(pid) must return -1. It is perfectly
   legal for a parent process to wait for child processes that have already
   terminated by the time the parent calls wait, but the kernel must still allow
   the parent to retrieve its child's exit status, or learn that the child was
   terminated by the kernel.

   wait must fail and return -1 immediately if any of the following conditions
   is true:

   pid does not refer to a direct child of the calling process. pid is a direct
   child of the calling process if and only if the calling process received pid
   as a return value from a successful call to exec.

  Note that children are not inherited: if A spawns child B and B spawns child
  process C, then A cannot wait for C, even if B is dead. A call to wait(C) by
  process A must fail. Similarly, orphaned processes are not assigned to a new
  parent if their parent process exits before they do.

  Note that children are not inherited: if A spawns child B and B spawns child
  process C, then A cannot wait for C, even if B is dead. A call to wait(C) by
  process A must fail. Similarly, orphaned processes are not assigned to a new
  parent if their parent process exits before they do.

  The process that calls wait has already called wait on pid. That is, a process
  may wait for any given child at most once.

   You must ensure that Pintos does not terminate until the initial process
   exits. The supplied Pintos code tries to do this by calling process_wait()
   (in userprog/process.c) from main() (in threads/init.c). We suggest that you
   implement process_wait() according to the comment at the top of the function
   and then implement the wait system call in terms of process_wait(). */
static void wait(struct intr_frame *f) {
    /* Parse arguments. */
    pid_t pid = get_arg(f, 1);
    f->eax = process_wait(pid);
}

/* Creates a new file called file initially initial_size bytes in size. Returns
   true if successful, false otherwise. Creating a new file does not open it:
   opening the new file is a separate operation which would require a open
   system call. */
static void create(struct intr_frame *f) {
    /* Parse arguments, ensure valid pointer. */
    const char* file = (const char*) get_arg(f, 1);
    unsigned initial_size = get_arg(f,2);

    /* Verify arguments. */
    verify_pointer((uint32_t *) file);

    /* Create file, return boolean value. */
    f->eax = filesys_create(file, initial_size);
}

/* Deletes the file called file. Returns true if successful, false otherwise. A
   file may be removed regardless of whether it is open or closed, and removing
   an open file does not close it. */
static void remove(struct intr_frame *f) {
    /* Parse arguments. */
    const char* file = (const char*) get_arg(f, 1);

    /* Verify arguments. */
    verify_pointer((uint32_t *) file);

    /* Remove file. */
    f->eax = filesys_remove(file);
}

/* Opens the file called file. Returns a nonnegative integer handle called a 
   "file descriptor" (fd), or -1 if the file could not be opened.

   File descriptors numbered 0 and 1 are reserved for the console: fd 0
   (STDIN_FILENO) is standard input, fd 1 (STDOUT_FILENO) is standard output.
   The open system call will never return either of these file descriptors,
   which are valid as system call arguments only as explicitly described below.

   Each process has an independent set of file descriptors. File descriptors are
   not inherited by child processes.

   When a single file is opened more than once, whether by a single process or
   different processes, each open returns a new file descriptor. Different file
   descriptors for a single file are closed independently in separate calls to
   close and they do not share a file position. */
static void open(struct intr_frame *f) {
    /* Parse arguments. */
    const char* file_name = (const char*) get_arg(f, 1);

    /* Verify arguments. */
    verify_pointer((uint32_t *) file_name);

    /* Count of file descriptors for error checking. */
    size_t count_start = list_size(&thread_current()->fds);

    if (file_name) {
        /* Try to open file. */
        lock_acquire(&filesys_io);
        struct file* file = filesys_open(file_name);
        lock_release(&filesys_io);

        if (file) {
            /* File opened! Create a file descriptor, add to list. */
            int fd = 2;

            /* If we already have fds, set fd = max(fds) + 1.
               This ensures unique file descriptors for a thread. */
            if (!list_empty(&thread_current()->fds)) {

                struct list_elem* last = list_rbegin(&thread_current()->fds);
                struct file_des* last_fd = list_entry(last, 
                                                      struct file_des, elem);
                fd = MAX(fd, last_fd->fd+1);
            }

            /* If either of these numbers are assigned, something went horribly 
               wrong.*/
            ASSERT(fd != STDIN_FILENO || fd != STDOUT_FILENO);

            /* Add new file descriptor object. */
            struct file_des* new_fd = malloc(sizeof(struct file_des));
            new_fd->fd = fd;
            new_fd->file = file;

            list_push_back(&thread_current()->fds, &new_fd->elem);

            /* Return file descriptor. */
            f->eax = fd;

            /* List should have increased in size. */
            ASSERT(list_size(&thread_current()->fds) > count_start);

        } else {
            /* Couldn't open file. */
            f->eax = -1;
        }
    } else {
        /* Invalid file name. */
        thread_exit();
    }

    /* List should have increased, or at least stayed the same size. */
    ASSERT(list_size(&thread_current()->fds) >= count_start);
}


/* Returns the size, in bytes, of the file open as fd. */
static void filesize(struct intr_frame *f) {
    /* Parse arguments. */
    int fd = get_arg(f, 1);

    /* Special cases. */
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO) {
        thread_exit();
    }

    /* Return file size. */
    struct file* file = file_from_fd(fd)->file;
    if (file) {
        lock_acquire(&filesys_io);
        f->eax = file_length(file);
        lock_release(&filesys_io);
    } else {
        /* Invalid file has no size. */
        thread_exit();
    }
}


/* Reads size bytes from the file open as fd into buffer. Returns the number of
   bytes actually read (0 at end of file), or -1 if the file could not be read
   (due to a condition other than end of file). Fd 0 reads from the keyboard
   using input_getc(). */
static void read(struct intr_frame *f) {
    /* Parse arguments. */
    int fd = get_arg(f, 1);
    void* buffer = (void *) get_arg(f, 2);
    unsigned size = get_arg(f, 3);

    /* Verify arguments. */
    verify_pointer((uint32_t *) buffer);
    verify_pointer((uint32_t *) (buffer + size));

    /* Special cases. */
    if (fd == STDIN_FILENO) {
        /* Wait for keys to be pressed. */
        // TODO: Check for EOF key?
        for (unsigned i = 0; i < size; i++) {
            uint8_t key = input_getc();
            *((char *) buffer + i) = key;
        }
        f->eax = size;
        return;
    } 
    if (fd == STDOUT_FILENO) {
        /* Can't read from stdout... */
        f->eax = -1;
        return;
    }

    /* Return number of bytes read. */
    struct file* file = file_from_fd(fd)->file;
    if (file) {
        lock_acquire(&filesys_io);
        f->eax = file_read(file, buffer, size);
        lock_release(&filesys_io);
    } else {
        /* Can't read invalid file. */
        f->eax = -1;
    }
}

/* Writes size bytes from buffer to the open file fd. Returns the number of 
   bytes actually written, which may be less than size if some bytes could not 
   be written.

   Writing past end-of-file would normally extend the file, but file growth is
   not implemented by the basic file system. The expected behavior is to write
   as many bytes as possible up to end-of-file and return the actual number
   written, or 0 if no bytes could be written at all.

   Fd 1 writes to the console. Your code to write to the console should write
   all of buffer in one call to putbuf(), at least as long as size is not bigger
   than a few hundred bytes. (It is reasonable to break up larger buffers.)
   Otherwise, lines of text output by different processes may end up interleaved
   on the console, confusing both human readers and our grading scripts. */
static void write(struct intr_frame *f) {
    /* Parse arguments. */
    int fd = get_arg(f, 1);
    const void* buffer = (void *) get_arg(f, 2);
    uint32_t size = get_arg(f, 3);

    /* Verify arguments. */
    verify_pointer((uint32_t *) buffer);
    verify_pointer((uint32_t *) (buffer + size));

    /* Special cases. */
    if (fd == STDIN_FILENO) {
        /* Can't write to stdin... */
        f-> eax = 0;
        return;
    }
    if (fd == STDOUT_FILENO) {
        /* Write to terminal. */
        // TODO - Break up larger buffers into multiple calls.
        putbuf(buffer, size);
        f->eax = size;
        return;
    }

    /* Return number of bytes written. */
    struct file* file = file_from_fd(fd)->file;
    if (file) {
        lock_acquire(&filesys_io);
        int written = file_write(file, buffer, size);
        f->eax = written;
        lock_release(&filesys_io);
    } else {
        /* Can't write to file. */
        f->eax = 0;
    }
}

/* Changes the next byte to be read or written in open file fd to position,
   expressed in bytes from the beginning of the file. (Thus, a position of 0 is
   the file's start.)

   A seek past the current end of a file is not an error. A later read obtains 0
   bytes, indicating end of file. A later write extends the file, filling any
   unwritten gap with zeros. (However, in Pintos files have a fixed length until
   project 6 is complete, so writes past end of file will return an error.)
   These semantics are implemented in the file system and do not require any
   special effort in system call implementation. */
static void seek(struct intr_frame *f) {
    /* Parse arguments. */
    int fd = get_arg(f, 1);
    unsigned position = get_arg(f, 2);

    /* Special cases. */
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO) {
        thread_exit();
    }

    /* Seek file. */
    struct file* file = file_from_fd(fd)->file;
    if (file) {
        lock_acquire(&filesys_io);
        file_seek(file, position);
        lock_release(&filesys_io);
    } else {
        /* Can't seek invalid file. */
        thread_exit();
    }
}

/* Returns the position of the next byte to be read or written in open file fd,
   expressed in bytes from the beginning of the file. */
static void tell(struct intr_frame *f) {
    /* Parse arguments. */
    int fd = get_arg(f, 1);

    /* Special cases. */
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO) {
        thread_exit();
    }

    /* Tell file. */
    struct file* file = file_from_fd(fd)->file;
    if (file) {
        lock_acquire(&filesys_io);
        f->eax = file_tell(file);
        lock_release(&filesys_io);
    } else {
        /* Can't tell invalid file. */
        thread_exit();
    }
}

/* Closes file descriptor fd. Exiting or terminating a process implicitly closes
   all its open file descriptors, as if by calling this function for each one.*/
static void close(struct intr_frame *f) {
    /* Parse arguments. */
    int fd = get_arg(f, 1);

    /* Special cases. */
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO) {
        thread_exit();
    }

    /* Tell file. */
    struct file_des* file_des = file_from_fd(fd);
    if (file_des) {
        lock_acquire(&filesys_io);
        file_close(file_des->file);
        lock_release(&filesys_io);

        /* Free memory, remove from list. */
        list_remove(&file_des->elem);
        free(file_des);

    } else {
        /* Can't close invalid file. */
        thread_exit();
    }
}


static void mmap(struct intr_frame *f) {
    /* Parse arguments. */
    int fd = get_arg(f, 1);
    void* addr = (void *) get_arg(f, 2);

    /* Verify arguments. */
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO) {
        thread_exit();
    }
    verify_pointer((uint32_t *) addr);

    /* TODO: Function, more error checking. */

    /* TODO: return mapid_t */
    f->eax = -1;
}


static void munmap(struct intr_frame *f) {
    (void)f;
    /* Parse arguments. */
    // mapid_t mapid = get_arg(f, 1);

    /* TODO : include definition for mapid_t.
       Used in lib/user/syscall.c */

    /* Verify arguments. */
    /* TODO : what parameters can mapid_t take? */

    /* TODO : unmap. */

    /* no return. */
}

