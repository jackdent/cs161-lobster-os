#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <copyinout.h>
#include <array.h>

/* Returns 0 if we successfully copied the user's arguments into argv;
   otherwise, returns error code */
static
int
extract_args(char *const *args, char *buf, struct array *argv, struct array *argv_lens, int *argc)
{
        int pos, arg, rem, copy;
        size_t copied;

        if (args == NULL) {
                return 0;
        }

        pos = 0;
        arg = 0;
        while (args[arg] != NULL) {
                rem = ARG_MAX - pos;
                copy = copyinstr(args[arg], &buf[pos], rem, &copied);

                switch (copy) {
                case EFAULT:
                        return EFAULT;
                case ENAMETOOLONG:
                        return E2BIG;
                default:
                        array_add(argv, &buf[pos], NULL);
                        array_add(argv_lens, (int) copied, NULL);
                        pos += (int) copied;
                }
        }

        *argc = arg;
        array_add(argv, NULL, NULL); // NULL terminate the argv array
        return 0;
}

// TODO: what happens if we exceed the maximum stack size/reach the end of the user address space
static
int
copy_args_to_stack(struct addrspace *as, vaddr_t *stackptr, struct array *argv, struct array *argv_lens, int *argc)
{
        int padding, i, len;

        for (i = argc - 1; i >= 0; --i) {
                len = array_get(argv_lens, i);
                padding = len % 4 == 0 ? 0 : 4 - (len % 4);
                *stackptr -= len + padding;
                copyoutstr(argv[i, *stackptr, argv_lens[i], NULL);

                // Rather than allocating an new array, we reuse the argv_lens array to store pointers
                // to the start of each argument on the stack
                array_set(argv_lens, i, *stackptr);
        }

        for (i = argc - 1; i >= 0; --i) {
                *stackptr -= 4;
                userptr_t arg = array_get(argv_lens, i);
                copyout(&arg, *stackptr, 4);
        }
}

int
execv(const char *prog, char *const *args)
{
        KASSERT(prog != NULL);

        int argc, result;
        char *arg_buf;
        struct array *argv, array *argv_lens;
        struct addrspace *as, vnode *v;
        vaddr_t entrypoint, stackptr;

        arg_buf = kmalloc(ARG_MAX);
        argv = array_create();
        argv_lens = array_create();
        result = extract_args(args, arg_buf, argv, argv_lens, &argc);
        if (result != 0) {
                goto cleanup_args;
        }

        /* Open the file. */
        result = vfs_open(progname, O_RDONLY, 0, &v);
        if (result) {
                goto cleanup_prog_read;
        }

        /* We should be a new process. */
        KASSERT(proc_getas() == NULL);

        /* Create a new address space. */
        as = as_create();
        if (as == NULL) {
                result = ENOMEM;
                goto cleanup_as_create;
        }

        /* Switch to it and activate it. */
        proc_setas(as);
        /* p_addrspace will go away when curproc is destroyed */
        as_activate();

        /* Load the executable. */
        result = load_elf(v, &entrypoint);
        if (result) {
                goto cleanup_load_elf;
        }

        /* Done with the file now. */
        vfs_close(v);

        /* Define the user stack in the address space */
        result = as_define_stack(as, &stackptr);
        if (result) {
                goto cleanup_define_stack;
        }

        result = copy_args_to_stack(as, &stackptr, argv, argv_lens, argc);
        if (result) {
                // TODO
        }

        /* Warp to user mode. */
        enter_new_process(argc, stackptr, NULL, stackptr, entrypoint);

        /* enter_new_process does not return. */
        panic("enter_new_process returned\n");
        return EINVAL;

cleanup_load_elf:
cleanup_as_create:
        vfs_close(v);
cleanup_define_stack:
cleanup_prog_read:
cleanup_args:
        kfree(arg_buf);
        array_destroy(argv);
        array_destroy(argv_lens);
        return result;
}
