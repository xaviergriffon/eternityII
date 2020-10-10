//
//  eth_opencl_instance.h
//  EthServer
//
//  Created by Xavier GRIFFON on 14/10/13.
//  Copyright (c) 2013 Xavier GRIFFON. All rights reserved.
//

#ifndef eternityII_etii_opencl_instance_h
#define eternityII_etii_opencl_instance_h
#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#include "part.h"

typedef struct
{
    cl_context context;
    cl_program program;
    
    cl_command_queue queue;
    cl_mem map_buffer;
	cl_mem key_buffer;
	cl_mem faceused_buffer;
	cl_mem output_buffer;
	cl_mem qt_buffer;
    cl_mem img_map;
    cl_mem img_datas;
	cl_mem img_ids;
	cl_mem img_keys;
	cl_mem img_output;
	cl_mem img_mapId;
	cl_mem datasIds;
	cl_mem all_rotate_ids;
	cl_mem possibility;
	cl_mem output_possibility;
	cl_mem directions;
	
    int mapsizearray;
	int max_research;
	key_part *partsBuffer;
	uint8_t *resultsBuffer;
	int16_t *imgoutflat;
	void *output_possibility_buffer;
    
} etii_cl_instance;

#endif
