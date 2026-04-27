/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Bertrand Lebonnois
 * All rights reserved.
 */
#include "test.h"

DEFINE_TEST(test_empty_zxc)
{
	const char *reffile = "test_empty.zxc";
	int f;

	extract_reference_file(reffile);
	f = systemf("%s %s >test.out 2>test.err", testprog, reffile);
	if (f == 0 || canZxc()) {
		assertEqualInt(0, f);
		assertEmptyFile("test.out");
		assertEmptyFile("test.err");
	} else {
		skipping("It seems zxc is not supported on this platform");
	}
}
