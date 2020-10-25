#include <stdio.h>
#include <stdlib.h>
#include "readdata.h"

struct array_part *read_parts(const char *file)
{
	int np = 0;
	
	FILE *f = fopen(file, "r");
	if(!f)
	{
		printf("file :%s",file);
		perror("fopen()");
		exit(EXIT_FAILURE);
	}
	
	fscanf(f, "ntiles: %d", &np);
	printf("ntiles:%i\n",np);


    struct part *parts = NULL;
    int NB_PARTS = (np+1);
	if(NULL == (parts = malloc(NB_PARTS * sizeof *parts)))
	{
		exit(EXIT_FAILURE);
	}
    struct array_part *array = malloc(sizeof(struct array_part));
    array->size = np;
    array->parts = parts;
    
	parts[0].id = 0;
	parts[0].top = 0;
	parts[0].left = 0;
	parts[0].bottom = 0;
	parts[0].right = 0;
    parts[0].rotation = 0;
	int count = 1;
	while(!feof(f))
	{
		int top;
		int left;
		int bottom;
		int right;
		fscanf(f, "%hd %d %d %d %d",
			  &parts[count].id,
			   &top,
			   &left,
			   &bottom,
			   &right);
		
		parts[count].top=(int8_t)top;
		parts[count].left=(int8_t)left;
		parts[count].bottom=(int8_t)bottom;
		parts[count].right=(int8_t)right;
        parts[count].rotation = 0;
		
		//print_part(&parts[count]);
		count++;
	}
	
	fclose(f);
    return array;
}
