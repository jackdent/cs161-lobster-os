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
#include <limits.h>


/* Returns 0 if we successfully copied the user's arguments into buf;
   otherwise, returns error code. argv will contain pointers to the starts
   of each argument, and argv_lens will contain the lengths

   If copy is set to false, in the case of runprogram, extract_args will
   simply save the addresses and lengths and verify that we don'y exceed
   ARG_MAX, since the arguments already exist in the kernel space */
int
extract_args(userptr_t args, char *buf, struct array *argv, struct array *argv_lens, bool copy_args)
{
	int arg, result;
	size_t pos, copied, rem;
	char **args_p;

	if (args == NULL) {
		return 0;
	}

	// Cast args back into an array of (char *)s, so we can index into it
	args_p = (char **)args;

	pos = 0;
	arg = 0;
	while (args_p[arg] != NULL) {
		rem = ARG_MAX - pos;
		if (copy_args) {
			result = copyinstr((const_userptr_t) args_p[arg], &buf[pos], rem, &copied);

			switch (result) {
			case EFAULT:
				return EFAULT;
			case ENAMETOOLONG:
				return E2BIG;
			default:
				if (result != 0) {
					panic("Unexpected error from copyinstr in extract_args\n");
				}
			};
		}
		else {
			copied = strlen(args_p[arg]) + 1;

			if (rem < copied) {
				return E2BIG;
			}
		}

		result = array_add(argv, &buf[pos], NULL);
		if (result) {
			return result;
		}

		result = array_add(argv_lens, (void*)copied, NULL);
		if (result) {
			return result;
		}

		pos += copied;
		arg++;
	}

	return 0;
}

void
copy_args_to_stack(vaddr_t *stack_ptr, struct array *argv, struct array *argv_lens)
{
	int argc, i, len, padding;
	char* start_ptr;
	userptr_t arg_ptr;

	argc = argv->num;

	for (i = argc - 1; i >= 0; --i) {
		len = (int) array_get(argv_lens, i);
		start_ptr = array_get(argv, i);
		padding = len % 4 == 0 ? 0 : 4 - (len % 4);
		*stack_ptr -= len + padding;
		copyoutstr((const char*) start_ptr, (userptr_t) *stack_ptr, len, NULL);
		// TODO: we should always have enough space, but still
		// check the return value just in case

		// Rather than allocating an new array, we reuse the argv_lens array to store pointers
		// to the start of each argument on the stack
		array_set(argv_lens, i, (void*) *stack_ptr);
	}

	// NULL terminate the arg pointers
	*stack_ptr -= 4;
	arg_ptr = NULL;
	copyout((const void*) &arg_ptr, (userptr_t) *stack_ptr, 4);

	for (i = argc - 1; i >= 0; --i) {
		*stack_ptr -= 4;
		arg_ptr = (userptr_t) array_get(argv_lens, i);
		copyout((const void*) &arg_ptr, (userptr_t) *stack_ptr, 4);
	}
}

int
_launch_program(char *progname, vaddr_t *stack_ptr, vaddr_t *entry_point)
{
	struct addrspace *as, *old_as;
	struct vnode *v;
	int result;

	/* Save the old address space */
	old_as = proc_getas();

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		result = ENOENT;
		goto err1;
	}

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		result = ENOMEM;
		goto err2;
	}

	proc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, entry_point);
	if (result) {
		result = ENOEXEC;
		goto err3;
	}

	// Clean up
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, stack_ptr);
	if (result) {
		goto err3;
	}

	return 0;


	err3:
		proc_setas(old_as);
		as_activate();
		as_destroy(as);
	err2:
		vfs_close(v);
	err1:
		return result;
}

int
sys_execv(userptr_t progname, userptr_t args)
{
	int argc, result;
	char *arg_buf, *progname_buf, *old_progname;
	struct array *argv, *argv_lens;
	vaddr_t entry_point, stack_ptr;

	KASSERT(progname != NULL);

	arg_buf = kmalloc(ARG_MAX);
	if (!arg_buf) {
		result = ENOMEM;
		goto err1;
	}

	argv = array_create();
	if (!argv) {
		result = ENOMEM;
		goto err2;
	}

	argv_lens = array_create();
	if (!argv_lens) {
		result = ENOMEM;
		goto err3;
	}

	result = extract_args(args, arg_buf, argv, argv_lens, true);
	if (result != 0) {
		result = ENOMEM;
		goto err4;
	}
	argc = argv->num;

	// Check user's program path
	progname_buf = kmalloc(PATH_MAX);
	if (progname_buf == NULL) {
		result = ENOMEM;
		goto err4;
	}

	result = copyinstr(progname, progname_buf, PATH_MAX, NULL);
	if (result) {
		result = ENOMEM;
		goto err5;
	}

	old_progname = curproc->p_name;
	curproc->p_name = progname_buf;

	result = _launch_program(progname_buf, &stack_ptr, &entry_point);
	if (result) {
		goto err6;
	}

	copy_args_to_stack(&stack_ptr, argv, argv_lens);

	// Cleanup
	kfree(arg_buf);
	array_zero_out(argv, false);
	array_destroy(argv);
	array_zero_out(argv_lens, false);
	array_destroy(argv_lens);
	kfree(old_progname);

	/* Warp to user mode. */
	enter_new_process(argc, (userptr_t) stack_ptr, NULL, stack_ptr, entry_point);

	/* enter_new_process does not return. */
	panic("sys_execv should never return\n");
	return -1;


	err6:
		curproc->p_name = old_progname;
	err5:
		kfree(progname_buf);
	err4:
		array_zero_out(argv_lens, false);
		array_destroy(argv_lens);
	err3:
		array_zero_out(argv, false);
		array_destroy(argv);
	err2:
		kfree(arg_buf);
	err1:
		return result;
}
