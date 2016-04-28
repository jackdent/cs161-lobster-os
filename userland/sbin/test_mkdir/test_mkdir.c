#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <sys/stat.h>


int main(void)
{
	assert(mkdir("mydir", 0) == 0);
}
