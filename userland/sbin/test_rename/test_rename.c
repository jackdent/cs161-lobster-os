#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <err.h>


int main(void)
{
	int f;
	f = open("old_file.txt", O_RDWR | O_CREAT);
	assert(f >= 0);
	close(f);

	assert(rename("old_file.txt", "new_file.txt") == 0);
}
