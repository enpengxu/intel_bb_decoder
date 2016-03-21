/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <err.h>
#include <assert.h>

#include "libdrm_macros.h"
#include "intel_bufmgr.h"
#include "intel_chipset.h"

#define HW_OFFSET 0x12300000

static void
usage(void)
{
	fprintf(stderr, "usage:\n");
	fprintf(stderr, "  test_decode <batch>\n");
	fprintf(stderr, "  test_decode <batch> -dump\n");
	exit(1);
}

static int
parse_line(uint32_t * line, size_t line_size,
		   uint32_t * instr_num, uint32_t * instr)
{
	char dummy[128];
	
	/*Jan 04 20:22:44    5     8   915 [drm:debug_print_error_obj]      00000000 :  7a000004*/
#define PATTERN_STR														\
	"%s %s %s    %s     %s   %s %s      %08x :  %08x\n"

#define PATTERN_PTR														\
	dummy, dummy, dummy, dummy, dummy, dummy, dummy, instr_num, instr

#define PATTERN_NUM  9		
	int matched = sscanf((char *)line, PATTERN_STR, PATTERN_PTR);
	return matched == PATTERN_NUM ? 0 : -1; 
}

static uint32_t *
read_data(FILE * file, size_t * num)
{
	const int size = 32 * 1024 * 4; /* 32 PAGES */
	unsigned int * data;
	data = malloc(size);
	if(!data){
		return NULL;
	}
	int rc;
	size_t line_size;
	uint32_t line[1024], instr_num, instr, n=0;
	fgets(line, 1024, file);
	fgets(line, 1024, file);
	while (fgets(line, 1024, file) ) {
		/* filter */
		rc = parse_line(line, line_size, &instr_num, &instr);
		assert (rc == 0 && (instr_num/4) == n && n < size);
		data[n] = instr;
		n ++;
	}
	*num = n;
	return data;
}

static void
read_file(const char *filename, void **ptr, size_t *size)
{
	int fd, ret;
	struct stat st;

	FILE * file = fopen(filename, "r");
	if (!file)
		errx(1, "couldn't open `%s'", filename);

	*ptr = read_data(file, size);
	fclose(file);
}

static void
dump_batch(struct drm_intel_decode *ctx, const char *batch_filename)
{
	void *batch_ptr;
	size_t batch_size;

	read_file(batch_filename, &batch_ptr, &batch_size);

	drm_intel_decode_set_batch_pointer(ctx, batch_ptr, HW_OFFSET,
					   batch_size);
	drm_intel_decode_set_output_file(ctx, stdout);

	drm_intel_decode(ctx);
}

static void
compare_batch(struct drm_intel_decode *ctx, const char *batch_filename)
{
	FILE *out = NULL;
	void *ptr, *ref_ptr, *batch_ptr;
#ifdef HAVE_OPEN_MEMSTREAM
	size_t size;
#endif
	size_t ref_size, batch_size;
	const char *ref_suffix = "-ref.txt";
	char *ref_filename;

	ref_filename = malloc(strlen(batch_filename) + strlen(ref_suffix) + 1);
	sprintf(ref_filename, "%s%s", batch_filename, ref_suffix);

	/* Read the batch and reference. */
	read_file(batch_filename, &batch_ptr, &batch_size);
	read_file(ref_filename, &ref_ptr, &ref_size);

	/* Set up our decode output in memory, because I don't want to
	 * figure out how to output to a file in a safe and sane way
	 * inside of an automake project's test infrastructure.
	 */
#ifdef HAVE_OPEN_MEMSTREAM
	out = open_memstream((char **)&ptr, &size);
#else
	fprintf(stderr, "platform lacks open_memstream, skipping.\n");
	exit(77);
#endif

	drm_intel_decode_set_batch_pointer(ctx, batch_ptr, HW_OFFSET,
					   batch_size / 4);
	drm_intel_decode_set_output_file(ctx, out);

	drm_intel_decode(ctx);

	if (strcmp(ref_ptr, ptr) != 0) {
		fprintf(stderr, "Decode mismatch with reference `%s'.\n",
			ref_filename);
		fprintf(stderr, "You can dump the new output using:\n");
		fprintf(stderr, "  test_decode \"%s\" -dump\n", batch_filename);
		exit(1);
	}

	fclose(out);
	free(ref_filename);
	free(ptr);
}

static uint16_t
infer_devid(const char *batch_filename)
{
	struct {
		const char *name;
		uint16_t devid;
	} chipsets[] = {
		{ "830",  0x3577},
		{ "855",  0x3582},
		{ "945",  0x2772},
		{ "gen4", 0x2a02 },
		{ "gm45", 0x2a42 },
		{ "gen5", PCI_CHIP_ILD_G },
		{ "gen6", PCI_CHIP_SANDYBRIDGE_GT2 },
		{ "gen7", PCI_CHIP_IVYBRIDGE_GT2 },
		{ "gen8", 0x1616 },
		{ "gen9", 0x0A84 },
		{ NULL, 0 },
	};
	int i;

	for (i = 0; chipsets[i].name != NULL; i++) {
		if (strstr(batch_filename, chipsets[i].name))
			return chipsets[i].devid;
	}

	fprintf(stderr, "Couldn't guess chipset id from batch filename `%s'.\n",
		batch_filename);
	fprintf(stderr, "Must be contain one of:\n");
	for (i = 0; chipsets[i].name != NULL; i++) {
		fprintf(stderr, "  %s\n", chipsets[i].name);
	}
	exit(1);
}

int
main(int argc, char **argv)
{
	uint16_t devid;
	struct drm_intel_decode *ctx;

	if (argc < 2)
		usage();


	devid = infer_devid(argv[1]);

	ctx = drm_intel_decode_context_alloc(devid);

	dump_batch(ctx, argv[2]);

	drm_intel_decode_context_free(ctx);

	return 0;
}
