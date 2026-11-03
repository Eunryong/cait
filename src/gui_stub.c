#include <stdio.h>

#include "cait.h"

/* Built on systems without Cocoa. */
int cmd_gui(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "cait: gui is only available on macOS; use `cait panel`\n");
	return 1;
}
