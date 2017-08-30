#include <vitasdk.h>

// CPU downscaling function (0.5 scale)
void rescaleBuffer(uint32_t* src, uint32_t* dst, uint32_t pitch, uint32_t width, uint32_t height){
	int i,j,z,k,ptr;
	z=0;
	k=0;
	for (i=0;i < height; i+=2){
		ptr = pitch * i;
		z = height * (k++);
		for (j=0;j < width; j+=2){
			dst[z++]=src[ptr+j];
		}
	}
}