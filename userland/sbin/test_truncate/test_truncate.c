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
	f = open("test.txt", O_RDWR);
	assert(f >= 0);

	assert(ftruncate(f, 0) == 0);
	assert(ftruncate(f, 4096) == 0);
	assert(ftruncate(f, 5000) == 0);
	assert(ftruncate(f, 1000) == 0);
	assert(ftruncate(f, 300) == 0);

	close(f);

	assert(remove("test.txt") == 0);
}
