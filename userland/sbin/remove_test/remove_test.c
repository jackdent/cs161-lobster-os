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
	f = open("test.txt", O_RDWR | O_CREAT);
	assert(f >= 0);
	close(f);

	assert(remove("test.txt") >= 0);
}
