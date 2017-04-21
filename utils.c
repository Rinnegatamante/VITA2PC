#include "utils.h"

#define ALIGN(x, a)	(((x) + ((a) - 1)) & ~((a) - 1))

// malloc/free re-implementation for taiHen plugins
void* malloc(size_t size){
	void* ret = NULL;
	SceUID m = sceKernelAllocMemBlock("dummy", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ALIGN(size,0x40000), NULL);
	if (m >= 0) sceKernelGetMemBlockBase(m, &ret);
	return ret;
}

void free(void* addr){
	SceUID m = sceKernelFindMemBlockByAddr(addr, 1);
	if (m >= 0) sceKernelFreeMemBlock(m);
}