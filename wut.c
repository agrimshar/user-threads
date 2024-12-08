#include "wut.h"

#include <assert.h> // assert
#include <errno.h> // errno
#include <stddef.h> // NULL
#include <stdio.h> // perror
#include <stdlib.h> // reallocarray
#include <sys/mman.h> // mmap, munmap
#include <sys/signal.h> // SIGSTKSZ
#include <sys/queue.h> // TAILQ_*
#include <sys/ucontext.h>
#include <ucontext.h> // getcontext, makecontext, setcontext, swapcontext
#include <valgrind/valgrind.h> // VALGRIND_STACK_REGISTER
#include <string.h> // memset

// The thread entry structure.
struct wut_entry {
    int id;
    int exited; // 0 = not exited, 1 = exited
    int status; // 0 = running, 1 = ready, 2 = blocked, 128 = cancelled
    int blocking; // The id of the  thread that is being blocked by this thread, -1 if no thread is blocked
    int blocked_by; // The id of the thread that is blocking this thread, -1 if no thread is blocking this thread
    ucontext_t *context; 
    TAILQ_ENTRY(wut_entry) pointers;
};
// Initialize the priority queue.
TAILQ_HEAD(wut_head, wut_entry);

// The head of the thread list.
static struct wut_head wut_head;

static void die(const char* message) {
    int err = errno;
    perror(message);
    exit(err);
}

// The thread id counter.
static int wut_thread_counter = 0;

// Initial array size
int wut_list_size = 5;

// Create a dyanmic array of thread entries.
struct wut_entry **wut_list = NULL;

static char* new_stack(void) {
    char* stack = mmap(
        NULL,
        SIGSTKSZ,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_ANONYMOUS | MAP_PRIVATE,
        -1,
        0
    );
    if (stack == MAP_FAILED) {
        die("mmap stack failed");
    }
    VALGRIND_STACK_REGISTER(stack, stack + SIGSTKSZ);
    return stack;
}

static void delete_stack(char* stack) {
    if (munmap(stack, SIGSTKSZ) == -1) {
        die("munmap stack failed");
    }
}

void wut_init() {
    // Allocate the wut_list
    wut_list = (struct wut_entry**)calloc(wut_list_size, sizeof(struct wut_entry*));
    if (wut_list == NULL) {
        die("malloc wut_list failed");
    }

    // Initialize the thread list
    TAILQ_INIT(&wut_head);

    // Check if the list is empty
    if (!TAILQ_EMPTY(&wut_head)) {
        die("Queue is not empty");
    }

    // Create the main thread
    struct wut_entry* main_thread = (struct wut_entry*)malloc(sizeof(struct wut_entry));
    if (main_thread == NULL) {
        die("malloc main thread failed");
    }

    // Set the main thread id
    main_thread->id = 0;

    // Set the main thread status
    main_thread->status = 0;

    // Set the main thread blocked thread id
    main_thread->blocking = -1;
    main_thread->blocked_by = -1;

    // Set the exited status
    main_thread->exited = 0;

    // Set the main thread context
    main_thread->context = (ucontext_t*)malloc(sizeof(ucontext_t));
    if (main_thread->context == NULL) {
        die("malloc main thread context failed");
    }

    // Get the current context
    if (getcontext(main_thread->context) == -1) {
        die("getcontext main thread failed");
    }
    main_thread->context->uc_stack.ss_sp = new_stack();
    main_thread->context->uc_stack.ss_size = SIGSTKSZ;
    main_thread->context->uc_link = NULL;
    makecontext(main_thread->context, NULL, 0);

    // Add main thread to wut_list
    wut_list[0] = main_thread;

    // Update the thread id counter
    wut_thread_counter++;

    // Add the main thread to the list
    TAILQ_INSERT_TAIL(&wut_head, main_thread, pointers);
}

int wut_id() {
    // Get the current thread
    struct wut_entry* current_thread = TAILQ_FIRST(&wut_head);

    // Check if the current thread is not NULL
    if (current_thread == NULL) {
        return -1;
    }

    // Return the current thread id
    return current_thread->id;
}

void thread_wrapper(void (*run)(void)) {
    run(); // Run the user-defined function
    wut_exit(0); // Exit the thread implicitly if `run` returns
}

int wut_create(void (*run)(void)) {
    // Create the new thread
    struct wut_entry* new_thread;

    // Loop through the wut_list to find an empty spot
    for (int i = 0; i < wut_list_size; i++) {
        // Check if the current thread is NULL
        if ((wut_list[i] == NULL) || (wut_list[i] != NULL && wut_list[i]->context == NULL)) {
            // Create the new thread
            new_thread = (struct wut_entry*)malloc(sizeof(struct wut_entry));
            
            // Check if the new thread is NULL
            if (new_thread == NULL) {
                die("malloc new thread failed");
            }

            // Set the new thread context
            new_thread->context = (ucontext_t*)malloc(sizeof(ucontext_t));
            if (new_thread->context == NULL) {
                die("malloc new thread context failed");
            }

            // Get the current context
            if (getcontext(new_thread->context) == -1) {
                die("getcontext new thread failed");
            }
            new_thread->context->uc_stack.ss_sp = new_stack();
            new_thread->context->uc_stack.ss_size = SIGSTKSZ;
            new_thread->context->uc_link = NULL;
            makecontext(new_thread->context, (void (*)(void))thread_wrapper, 1, run);

            // Set the new thread id
            new_thread->id = i;

            // Set the new thread status
            new_thread->status = 1;

            // Set blocked thread id
            new_thread->blocking = -1;
            new_thread->blocked_by = -1;

            // Set the exited status
            new_thread->exited = 0;

            // Add new thread to wut_list
            wut_list[i] = new_thread;

            // Add the new thread to the list
            TAILQ_INSERT_TAIL(&wut_head, new_thread, pointers);

            // Update the thread id counter
            wut_thread_counter++;

            // Return the new thread id
            return new_thread->id;
        }
    }

    // If we reach this point, we have to reallocate the wut_list
    struct wut_entry** new_wut_list = (struct wut_entry**)reallocarray(wut_list, 2 * wut_list_size, sizeof(struct wut_entry*));
    if (new_wut_list == NULL) {
        die("reallocarray wut_list failed");
    }

    // Zero out the newly allocated portion
    memset(new_wut_list + wut_list_size, 0, wut_list_size * sizeof(struct wut_entry*));

    // Set the new wut_list
    wut_list = new_wut_list;
    wut_list_size *= 2;

    // Create the new thread
    new_thread = (struct wut_entry*)malloc(sizeof(struct wut_entry));

    // Check if the new thread is NULL
    if (new_thread == NULL) {
        die("malloc new thread failed");
    }

    // Set the new thread context
    new_thread->context = (ucontext_t*)malloc(sizeof(ucontext_t));
    if (new_thread->context == NULL) {
        die("malloc new thread context failed");
    }

    // Get the current context
    if (getcontext(new_thread->context) == -1) {
        die("getcontext new thread failed");
    }
    new_thread->context->uc_stack.ss_sp = new_stack();
    new_thread->context->uc_stack.ss_size = SIGSTKSZ;
    new_thread->context->uc_link = NULL;
    makecontext(new_thread->context, (void (*)(void))thread_wrapper, 1, run);

    // Set the new thread id
    new_thread->id = wut_thread_counter;

    // Set the new thread status
    new_thread->status = 1;

    // Set blocked thread id
    new_thread->blocking = -1;
    new_thread->blocked_by = -1;

    // Add new thread to wut_list
    wut_list[wut_thread_counter] = new_thread;

    // Add the new thread to the list
    TAILQ_INSERT_TAIL(&wut_head, new_thread, pointers);

    // Update the thread id counter
    wut_thread_counter++;

    // Return the new thread id
    return new_thread->id;
}


int wut_cancel(int id) {
    // Check if the id is valid
    if (id < 0 || id >= wut_list_size || id == wut_id()) {
        return -1;
    }
    // Get the thread to cancel
    struct wut_entry* thread_to_cancel = wut_list[id];

    // Check if the thread to cancel is NULL
    if (thread_to_cancel == NULL || thread_to_cancel->context == NULL) {
        return -1;
    }

    // If the thread is blocked by another thread, set the blocking thread status to -1
    if (thread_to_cancel->blocked_by != -1) {
        struct wut_entry* blocking_thread = wut_list[thread_to_cancel->blocked_by];
        if (blocking_thread != NULL) {
            blocking_thread->blocking = -1;
        }
        thread_to_cancel->blocked_by = -1;
    }

    // If the thread is blocking another thread, set the blocked thread status to -1
    if (thread_to_cancel->blocking != -1) {
        struct wut_entry* blocked_thread = wut_list[thread_to_cancel->blocking];
        if (blocked_thread != NULL) {
            // Set the blocked thread status
            blocked_thread->status = 1;
            // Insert the blocked thread to the end of the queue
            TAILQ_INSERT_TAIL(&wut_head, blocked_thread, pointers);
            blocked_thread->blocked_by = -1;
        }
        thread_to_cancel->blocking = -1;

        thread_to_cancel->status = 128;

        // Remove the thread to cancel from the list
        TAILQ_REMOVE(&wut_head, thread_to_cancel, pointers);

        // Need to return to the join because the thread is blocking
        return 0;
    }

    // Set the thread to cancel status
    thread_to_cancel->status = 128;

    // Remove the thread to cancel from the list
    TAILQ_REMOVE(&wut_head, thread_to_cancel, pointers);

    // // Clear the wut_list
    // wut_list[thread_to_cancel->id] = NULL;

    // Free the thread to cancel context
    delete_stack(thread_to_cancel->context->uc_stack.ss_sp);
    free(thread_to_cancel->context);
    thread_to_cancel->context = NULL;

    wut_thread_counter--;

    return 0;
}

int wut_join(int id) {
    // Check if the id is valid
    if (id < 0 || id >= wut_list_size || id == wut_id()) {
        return -1;
    }

    // If the thread hasn't been created
    if (wut_list[id] == NULL) {
        return -1;
    }

    // If the thread was cancelled, return the status
    if (wut_list[id]->status == 128) {
        return 128;
    }

    // Should not join a thread that is already blocked
    if (wut_list[id]->blocked_by != -1) {
        return -1;
    }

    // Shouldn't join a thread that is already freed
    if (wut_list[id]->context == NULL) {
        return -1;
    }

    // If the thread is already exited, return the status and free the memory
    if (wut_list[id]->exited == 1) {
        int return_status = wut_list[id]->status;

        // Free memory
        delete_stack(wut_list[id]->context->uc_stack.ss_sp);
        free(wut_list[id]->context);
        wut_list[id]->context = NULL;

        wut_thread_counter--;

        return return_status;
    }

    // If the thread is blocked by another thread, set the blocking thread status to -1
    if (wut_list[id]->blocked_by != -1) {
        struct wut_entry* blocking_thread = wut_list[wut_list[id]->blocked_by];
        if (blocking_thread != NULL) {
            blocking_thread->blocking = -1;
        }
        wut_list[id]->blocked_by = -1;
    }

    // If the thread is blocking another thread, set the blocked thread status to -1
    if (wut_list[id]->blocking != -1) {
        struct wut_entry* blocked_thread = wut_list[wut_list[id]->blocking];
        if (blocked_thread != NULL) {
            blocked_thread->blocked_by = -1;
        }
        wut_list[id]->blocking = -1;
    }

    // Get the current thread
    struct wut_entry* current_thread = TAILQ_FIRST(&wut_head);

    // Check if the current thread is not NULL
    if (current_thread == NULL) {
        return -1;
    }

    // Get the thread to join
    struct wut_entry* thread_to_join = wut_list[id];

    // Check if the thread to join is NULL
    if (thread_to_join == NULL) {
        return -1;
    }

    // Set the current thread status
    current_thread->status = 2;

    // Set the thread to join blocked thread id
    thread_to_join->blocking = current_thread->id;
    current_thread->blocked_by = thread_to_join->id;

    // Remove the current thread from the list
    TAILQ_REMOVE(&wut_head, current_thread, pointers);

    // Set the status of the first thread in the queue
    struct wut_entry* next_thread = TAILQ_FIRST(&wut_head);
    if (next_thread != NULL) {
        next_thread->status = 0;
    }

    // Loop through the queue to find the thread to join
    // If it's not there, add it to the end of the queue
    next_thread = NULL;
    TAILQ_FOREACH(next_thread, &wut_head, pointers) {
        if (next_thread->id == thread_to_join->id) {
            break;
        }
    }

    if (next_thread == NULL) {
        TAILQ_INSERT_TAIL(&wut_head, thread_to_join, pointers);
    }


    // Update the current threads context
    if (swapcontext(current_thread->context, TAILQ_FIRST(&wut_head)->context) == -1) {
        return -1;
    }

    current_thread->status = 0;

    int return_status = thread_to_join->status;

    // Free memory
    delete_stack(thread_to_join->context->uc_stack.ss_sp);
    free(thread_to_join->context);
    thread_to_join->context = NULL;

    wut_thread_counter--;

    // Return the status of the thread to join
    return return_status;
}

int wut_yield() {
    // Get the current thread
    struct wut_entry* current_thread = TAILQ_FIRST(&wut_head);

    // Check if the current thread is not NULL
    if (current_thread == NULL) {
        return -1;
    }

    // Get the next thread
    struct wut_entry* next_thread = TAILQ_NEXT(current_thread, pointers);

    // Check if the next thread is NULL
    if (next_thread == NULL) {
        next_thread = TAILQ_FIRST(&wut_head);
    }

    if (next_thread == current_thread) {
        return -1; // should not happen
    }

    // Set the current threads status
    current_thread->status = 1;

    // Remove the current thread from the list
    TAILQ_REMOVE(&wut_head, current_thread, pointers);

    // Add the current thread to the list
    TAILQ_INSERT_TAIL(&wut_head, current_thread, pointers);

    // Update the nesxt threads status
    next_thread->status = 0;

    // Update the current threads context
    if (swapcontext(current_thread->context, next_thread->context) == -1) {
        return -1;
    }

    current_thread->status = 0;

    return 0;
}

void wut_exit(int status) {
    // Store the lower byte of the status
    status &= 0xFF;

    // Get the current thread
    struct wut_entry* current_thread = TAILQ_FIRST(&wut_head);
    struct wut_entry* next_thread = NULL;

    // Check if the current threads status
    if (current_thread->status == 0) {
        // Set the current threads status
        current_thread->status = status;

        // If the current thread is blocked, unblock the thread
        if (current_thread->blocking != -1) {
            // Get the blocked thread
            struct wut_entry* blocked_thread = wut_list[current_thread->blocking];
            
            // Reset the current threads blocked thread id
            current_thread->blocking = -1;

            // Check if the blocked thread is not NULL
            if (blocked_thread != NULL) {
                // Set the blocked threads status
                blocked_thread->status = 1;

                // Set the blocked threads blocked thread id
                blocked_thread->blocked_by = -1;

                // Add the blocked thread to the list
                TAILQ_INSERT_TAIL(&wut_head, blocked_thread, pointers);
            }
        }

        // Set the exited status
        current_thread->exited = 1;

        // Set the next thread
        next_thread = TAILQ_NEXT(current_thread, pointers);

        // Remove the current thread from the list
        TAILQ_REMOVE(&wut_head, current_thread, pointers);

        // Check if the next thread is not NULL
        if (next_thread != NULL) {
            // Set the next threads status
            next_thread->status = 0;
            // Set the next threads context
            swapcontext(current_thread->context, next_thread->context);
        }
    }

    // If the list is empty, exit the program
    if (TAILQ_EMPTY(&wut_head)) {
        // Free all memory
        for (int i = 0; i < wut_list_size; i++) {
            if (wut_list[i] != NULL) {
                free(wut_list[i]);
            }
        }
        free(wut_list);
        exit(0);
    }
}
