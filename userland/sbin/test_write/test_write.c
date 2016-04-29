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


	char bigbuf[1024];

	for (int i = 0; i < 1024; i++) {
		bigbuf[i] = (char)i;
	}

	char littlebuf[] = "abcdefgh";

	// Little write
	assert(write(f, littlebuf, strlen(littlebuf)) == (int)strlen(littlebuf));

	// Big write
	assert(write(f, bigbuf, 1024) == 1024);

	close(f);
}
