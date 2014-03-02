//
//  eth_opencl.h
//  EthServer
//
//  Created by Xavier GRIFFON on 03/10/13.
//  Copyright (c) 2013 Xavier GRIFFON. All rights reserved.
//

#ifndef eternityII_etii_opencl_h
#define eternityII_etii_opencl_h

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif
#include "etii_opencl_instance.h"
#include "lifo.h"
#include "part.h"
#include "possibility.h"

int check_devices(cl_device_type device_type);
etii_cl_instance *create_etii_cl_instance(cl_device_type device_type, map_big_array *map, int max_research);
int free_etii_cl_instance(etii_cl_instance *instance);
File **test_opencl(etii_cl_instance *instance, key_part *keys, struct possibility_packet *possiblity, int nbresearch);

#endif
