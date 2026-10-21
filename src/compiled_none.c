/* Stand-in when no recompiled code is linked: everything is interpreted. */
#include "machine.h"

const char *compiled_info = "no compiled code: interpreter only";
const uint32_t compiled_image_sum = 0;

void compiled_init(void)
{
}

void compiled_disable(void)
{
}

CompiledFn compiled_lookup(uint16_t cs, uint16_t ip)
{
	(void)cs;
	(void)ip;
	return NULL;
}
