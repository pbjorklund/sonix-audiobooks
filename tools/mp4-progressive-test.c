#include "src/system/decode/mp4.h"

#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv) {
	assert(argc == 2);
	// ftyp (8), moov (16), mdat (16). Only the published prefix counts.
	const unsigned char front[] = {
		0,0,0,8,'f','t','y','p', 0,0,0,16,'m','o','o','v',
		0,0,0,8,'f','r','e','e', 0,0,0,16,'m','d','a','t', 1,2,3,4,5,6,7,8,
	};
	FILE *f = fopen(argv[1], "wb");
	assert(f && fwrite(front, 1, sizeof(front), f) == sizeof(front) && fclose(f) == 0);
	assert(!mp4_front_metadata_ready(argv[1], 15));
	assert(!mp4_front_metadata_ready(argv[1], 23));
	assert(mp4_front_metadata_ready(argv[1], 24));
	const unsigned char tail[] = {
		0,0,0,8,'f','t','y','p', 0,0,0,16,'m','d','a','t', 1,2,3,4,5,6,7,8,
		0,0,0,16,'m','o','o','v', 0,0,0,8,'f','r','e','e',
	};
	f = fopen(argv[1], "wb");
	assert(f && fwrite(tail, 1, sizeof(tail), f) == sizeof(tail) && fclose(f) == 0);
	assert(!mp4_front_metadata_ready(argv[1], sizeof(tail)));
	const unsigned char overflow[] = {
		0,0,0,8,'f','t','y','p', 0,0,0,1,'m','o','o','v',
		255,255,255,255,255,255,255,255,
	};
	f = fopen(argv[1], "wb");
	assert(f && fwrite(overflow, 1, sizeof(overflow), f) == sizeof(overflow) && fclose(f) == 0);
	assert(!mp4_front_metadata_ready(argv[1], sizeof(overflow)));
	puts("MP4 progressive metadata: bounded prefix and tail layout passed.");
}
