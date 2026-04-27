/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Bertrand Lebonnois
 * All rights reserved.
 */
#include "test.h"

DEFINE_TEST(test_option_zxc)
{
	char *p;
	int r;
	size_t s;

	/* Create a file. */
	assertMakeFile("f", 0644, "a");

	/* Archive it with zxc compression. */
	r = systemf("echo f | %s -o --zxc >archive.out 2>archive.err",
	    testprog);
	p = slurpfile(&s, "archive.err");
	p[s] = '\0';
	if (r != 0) {
		if (strstr(p, "Unsupported compression") != NULL) {
			skipping("This version of bsdcpio was compiled "
			    "without zxc support");
			goto done;
		}
		/* POSIX permits different handling of the spawnp
		 * system call used to launch the subsidiary
		 * program: */
		/* Some systems fail immediately to spawn the new process. */
		if (strstr(p, "Can't launch") != NULL && !canZxc()) {
			skipping("This version of bsdcpio uses an external zxc program "
			    "but no such program is available on this system.");
			goto done;
		}
		/* Some systems successfully spawn the new process,
		 * but fail to exec a program within that process.
		 * This results in failure at the first attempt to
		 * write. */
		if (strstr(p, "Can't write") != NULL && !canZxc()) {
			skipping("This version of bsdcpio uses an external zxc program "
			    "but no such program is available on this system.");
			goto done;
		}
		/* On some systems the error won't be detected until closing
		   time, by a 127 exit error returned by waitpid. */
		if (strstr(p, "Error closing") != NULL && !canZxc()) {
			skipping("This version of bsdcpio uses an external zxc program "
			    "but no such program is available on this system.");
			return;
		}
		failure("--zxc option is broken: %s", p);
		assertEqualInt(r, 0);
		goto done;
	}
	free(p);
	/* Check that the archive file has the zxc magic
	 * (0x9CB02EF5, little-endian). */
	p = slurpfile(&s, "archive.out");
	assert(s > 4);
	assertEqualMem(p, "\xF5\x2E\xB0\x9C", 4);

done:
	free(p);
}
