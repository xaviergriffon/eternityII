//
//  eth_opencl.c
//  EthServer
//
//  Created by Xavier GRIFFON on 03/10/13.
//  Copyright (c) 2013 Xavier GRIFFON. All rights reserved.
//

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include <unistd.h>
#include "etii_opencl.h"

// Résultat avec au moins une facade connu
#define RESULT_BY_SEARCH 60
#define MAX_SOURCE_SIZE (0x100000)

#define CL_CHECK(_expr)                                                         \
do {                                                                         \
cl_int _err = _expr;                                                       \
if (_err == CL_SUCCESS)                                                    \
break;                                                                   \
fprintf(stderr, "OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err);   \
abort();                                                                   \
} while (0)

#define CL_CHECK_ERR(_expr,_ret)                                                     \
do {                                                                           \
cl_int _err = CL_INVALID_VALUE;                                            \
_ret = _expr;                                                \
if (_err != CL_SUCCESS) {                                                  \
fprintf(stderr, "OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err); \
abort();                                                                 \
}                                                                      \
} while (0)

void CL_CALLBACK pfn_notify(const char *errinfo, const void *private_info, size_t cb, void *user_data)
{
	fprintf(stderr, "OpenCL Error (via pfn_notify): %s\n", errinfo);
}

int check_devices(cl_device_type device_type)
{
    cl_platform_id platforms[100];
	cl_uint platforms_n = 0;
	CL_CHECK(clGetPlatformIDs(100, platforms, &platforms_n));
	
	printf("=== %d OpenCL platform(s) found: ===\n", platforms_n);
	for (int i=0; i<platforms_n; i++)
	{
		char buffer[10240];
		printf("  -- %d --\n", i);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_PROFILE, 10240, buffer, NULL));
		printf("  PROFILE = %s\n", buffer);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_VERSION, 10240, buffer, NULL));
		printf("  VERSION = %s\n", buffer);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_NAME, 10240, buffer, NULL));
		printf("  NAME = %s\n", buffer);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_VENDOR, 10240, buffer, NULL));
		printf("  VENDOR = %s\n", buffer);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_EXTENSIONS, 10240, buffer, NULL));
		printf("  EXTENSIONS = %s\n", buffer);
	}
	
	if (platforms_n == 0){
		return 1;
	}
	
	cl_device_id devices[100];
	cl_uint devices_n = 0;
	
	CL_CHECK(clGetDeviceIDs(platforms[0], device_type, 100, devices, &devices_n));
	
	printf("=== %d OpenCL device(s) found on platform:\n", platforms_n);
	for (int i=0; i<devices_n; i++)
	{
		char buffer[10240];
		cl_uint buf_uint;
		cl_ulong buf_ulong;
		printf("  -- %d --\n", i);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_NAME, sizeof(buffer), buffer, NULL));
		printf("  DEVICE_NAME = %s\n", buffer);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_VENDOR, sizeof(buffer), buffer, NULL));
		printf("  DEVICE_VENDOR = %s\n", buffer);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_VERSION, sizeof(buffer), buffer, NULL));
		printf("  DEVICE_VERSION = %s\n", buffer);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DRIVER_VERSION, sizeof(buffer), buffer, NULL));
		printf("  DRIVER_VERSION = %s\n", buffer);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(buf_uint), &buf_uint, NULL));
		printf("  DEVICE_MAX_COMPUTE_UNITS = %u\n", (unsigned int)buf_uint);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(buf_uint), &buf_uint, NULL));
		printf("  DEVICE_MAX_CLOCK_FREQUENCY = %u\n", (unsigned int)buf_uint);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(buf_ulong), &buf_ulong, NULL));
		printf("  DEVICE_GLOBAL_MEM_SIZE = %llu\n", (unsigned long long)buf_ulong);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_LOCAL_MEM_SIZE,sizeof(buf_ulong), &buf_ulong, NULL));
		printf("  DEVICE_LOCAL_MEM_SIZE = %llu\n", (unsigned long long)buf_ulong);
		
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS,sizeof(buf_ulong), &buf_ulong, NULL));
		printf("  CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS = %llu\n", (unsigned long long)buf_ulong);
	}
	free(devices);
	if (devices_n == 0)
		return 1;
    
    return 0;
}

etii_cl_instance *create_etii_cl_instance(cl_device_type device_type, map_big_array *map, int max_research)
{
	check_devices(device_type);
	cl_platform_id platforms[100];
	cl_uint platforms_n = 0;
	CL_CHECK(clGetPlatformIDs(100, platforms, &platforms_n));
	
	if (platforms_n == 0){
		return NULL;
	}
	
	cl_device_id devices[100];
	cl_uint devices_n = 0;
    
	CL_CHECK(clGetDeviceIDs(platforms[0], device_type, 100, devices, &devices_n));
	
	if (devices_n == 0){
		return NULL;
	}
	cl_context context;
	
	CL_CHECK_ERR(clCreateContext(NULL, 1, devices, &pfn_notify, NULL, &_err), context);
    
    FILE *fp;
	char fileName[] = "./Search_Possibility.cl";
	char *source_str;
	size_t source_size;
	//Lecture du fichier
	fp = fopen(fileName, "r");
	source_str = (char*)calloc(MAX_SOURCE_SIZE, sizeof(char));
	source_size = fread( source_str, 1, MAX_SOURCE_SIZE, fp);
	fclose( fp );
	
	cl_program program;
	CL_CHECK_ERR(clCreateProgramWithSource(context, 1, (const char **)&source_str, (const size_t *)&source_size, &_err),program);
	if (clBuildProgram(program, 1, devices, "", NULL, NULL) != CL_SUCCESS) {
		char buffer[10240];
		clGetProgramBuildInfo(program, devices[0], CL_PROGRAM_BUILD_LOG, sizeof(buffer), buffer, NULL);
		fprintf(stderr, "CL Compilation failed:\n%s", buffer);
		abort();
	}
	
	free(source_str);
	
	struct map_in_one *mapinone = regroup_map(map);
	
	
	cl_mem map_buffer;
	CL_CHECK_ERR(clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(struct array_part) * mapinone->nbarrays, NULL, &_err), map_buffer);
	cl_mem key_buffer;
	CL_CHECK_ERR(clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(key_part)*max_research, NULL, &_err), key_buffer);
	cl_mem faceused_buffer;
	CL_CHECK_ERR(clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(uint8_t)* 256 * max_research, NULL, &_err), faceused_buffer);
    
    cl_mem output_buffer;
	CL_CHECK_ERR(clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(int16_t)*max_research*RESULT_BY_SEARCH, NULL, &_err), output_buffer);
	cl_mem qt_buffer;
	CL_CHECK_ERR(clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(uint8_t)*max_research, NULL, &_err), qt_buffer);
	
	size_t w_src, h_src;
	w_src = 1;
	h_src = max_research;
	cl_image_format iformat_src;
	iformat_src.image_channel_data_type = CL_SIGNED_INT8;
	iformat_src.image_channel_order = CL_RGBA;
	cl_image_desc idesc_src;
	idesc_src.image_width = w_src;
	idesc_src.image_height = h_src;
	idesc_src.image_type = CL_MEM_OBJECT_IMAGE2D;
	idesc_src.image_slice_pitch = 0;
	idesc_src.image_row_pitch = 0;
	idesc_src.image_array_size = 1;
	idesc_src.num_mip_levels = 0;
	idesc_src.num_samples = 0;
	idesc_src.buffer = NULL;
	cl_mem img_src;
	CL_CHECK_ERR(clCreateImage(context, CL_MEM_READ_ONLY, &iformat_src, &idesc_src, NULL, &_err), img_src);
	
	size_t w_dest, h_dest, d_dest;
	w_dest = RESULT_BY_SEARCH;
	h_dest = max_research;
	d_dest = RESULT_BY_SEARCH + 1;
	cl_image_format iformat_dest;
	iformat_dest.image_channel_data_type = CL_SIGNED_INT8;
	iformat_dest.image_channel_order = CL_RGBA;
	cl_image_desc idesc_dest;
	idesc_dest.image_width = w_dest;
	idesc_dest.image_height = h_dest;
	idesc_dest.image_depth = d_dest;
	idesc_dest.image_type = CL_MEM_OBJECT_IMAGE3D;
	idesc_dest.image_slice_pitch = 0;
	idesc_dest.image_row_pitch = 0;
	idesc_dest.image_array_size = 1;
	idesc_dest.num_mip_levels = 0;
	idesc_dest.num_samples = 0;
	idesc_dest.buffer = NULL;
	cl_mem img_dest;
	CL_CHECK_ERR(clCreateImage(context, CL_MEM_WRITE_ONLY, &iformat_dest, &idesc_dest, NULL, &_err), img_dest);
    
    size_t w_map, h_map;
	w_map = h_map = 576;
	cl_image_format iformat_map;
	iformat_map.image_channel_data_type = CL_UNSIGNED_INT16;
	iformat_map.image_channel_order = CL_RGBA;
	cl_image_desc idesc;
	idesc.image_width = w_map;
	idesc.image_height = h_map;
	idesc.image_type = CL_MEM_OBJECT_IMAGE2D;
	idesc.image_slice_pitch = 0;
	idesc.image_row_pitch = 0;
	idesc.image_array_size = 1;
	idesc.num_mip_levels = 0;
	idesc.num_samples = 0;
	idesc.buffer = NULL;
	cl_mem img_map;
	CL_CHECK_ERR(clCreateImage(context, CL_MEM_READ_ONLY, &iformat_map, &idesc, NULL, &_err), img_map);
    
    size_t w_datas, h_datas;
	w_datas = 225;
    h_datas = 64;
	cl_image_format iformat_datas;
	iformat_datas.image_channel_data_type = CL_SIGNED_INT8;
	iformat_datas.image_channel_order = CL_RGBA;
	cl_image_desc idesc_datas;
	idesc_datas.image_width = w_datas;
	idesc_datas.image_height = h_datas;
	idesc_datas.image_type = CL_MEM_OBJECT_IMAGE2D;
	idesc_datas.image_slice_pitch = 0;
	idesc_datas.image_row_pitch = 0;
	idesc_datas.image_array_size = 1;
	idesc_datas.num_mip_levels = 0;
	idesc_datas.num_samples = 0;
	idesc_datas.buffer = NULL;
	cl_mem img_datas;
	CL_CHECK_ERR(clCreateImage(context, CL_MEM_READ_ONLY, &iformat_datas, &idesc_datas, NULL, &_err), img_datas);
	
	long map_size = w_map * h_map * 4;
    long data_size = mapinone->nbparts * 4;
	uint16_t *maps = malloc(sizeof(uint16_t)*map_size);;
    int8_t *datas = malloc(sizeof(int8_t)*data_size);
    int d_positions = 0;
	int nd_positions = 0;
    int m_positions = 0;
    int m;
    for(m = 0; m < mapinone->nbarrays;m++)
    {
        uint16_t x = (uint16_t)(nd_positions%w_datas);
        uint16_t y = (uint16_t)((nd_positions - x) / w_datas);
        maps[m_positions] = 0;
        maps[m_positions] = x;
        maps[m_positions+1] = y;
		if(x > 225)
		{
			printf("x > 225\n");
		}
		if(y > 64)
		{
			printf("y > 64\n");
		}
        maps[m_positions+2] = (uint16_t)(mapinone->quantity[m]);
        maps[m_positions+3] = 0;
        m_positions = m_positions+4;
        
        int d;
        for(d=0; d < mapinone->quantity[m]; d++)
        {
            struct part part = mapinone->parts[mapinone->position[m]+d];
            datas[d_positions] = part.top;
            datas[d_positions+1] = part.right;
            datas[d_positions+2] = part.bottom;
            datas[d_positions+3] = part.left;
			//			printf("m:%i d:%i |",m,d_positions/4);
			//			print_part(&part);
            d_positions = d_positions +4;
			nd_positions++;
        }
        
    }
    
	size_t w_ids, h_ids;
	w_ids = h_ids = 529;//23*23
	cl_image_format iformat_ids;
	iformat_ids.image_channel_data_type = CL_SIGNED_INT16;
	iformat_ids.image_channel_order = CL_RGBA;
	cl_image_desc idesc_ids;
	idesc_ids.image_width = w_ids;
	idesc_ids.image_height = h_ids;
	idesc_ids.image_type = CL_MEM_OBJECT_IMAGE2D;
	idesc_ids.image_slice_pitch = 0;
	idesc_ids.image_row_pitch = 0;
	idesc_ids.image_array_size = 1;
	idesc_ids.num_mip_levels = 0;
	idesc_ids.num_samples = 0;
	idesc_ids.buffer = NULL;
	cl_mem img_ids;
	CL_CHECK_ERR(clCreateImage(context, CL_MEM_READ_ONLY, &iformat_ids, &idesc_ids, NULL, &_err), img_ids);
	long ids_size = w_ids * h_ids * 4;
    int16_t *ids = malloc(sizeof(int16_t)*ids_size);
    int i_positions = 0;
	int nb_real_faces = map->sizearray-1;
	int k1;
    for(k1 = 0; k1 < nb_real_faces;k1++)
    {
		int k2;
		for(k2 = 0; k2 < nb_real_faces;k2++)
		{
			int k3;
			for(k3 = 0; k3< nb_real_faces;k3++)
			{
				int k4;
				for(k4 = 0; k4 < nb_real_faces;k4++)
				{
					key_part key;
					key.k1 = k1;
					key.k2 = k2;
					key.k3 = k3;
					key.k4 = k4;
					struct part*p= get_one_part(map, key);
					if(p != NULL)
					{
						ids[i_positions] = p->id;
						ids[i_positions+1] = p->rotation;
					}else
					{
						ids[i_positions] = -1;
						ids[i_positions+1] = -1;
					}
					i_positions = i_positions +4;
				}
			}
		}
	}
	
	
	
	
    
    cl_command_queue queue;
	CL_CHECK_ERR(clCreateCommandQueue(context, devices[0], CL_QUEUE_PROFILING_ENABLE, &_err), queue);
    
    size_t origin[] = {0,0,0};
	size_t region[] = {w_map,h_map, 1};
    CL_CHECK(clEnqueueWriteImage(queue, img_map, CL_TRUE, origin, region, 0, 0, maps, 0, NULL, NULL));
	
    size_t region_data[] = {w_datas,h_datas, 1};
    CL_CHECK(clEnqueueWriteImage(queue, img_datas, CL_TRUE, origin, region_data, 0, 0, datas, 0, NULL, NULL));
	
	size_t region_id[] = {w_ids,h_ids, 1};
    CL_CHECK(clEnqueueWriteImage(queue, img_ids, CL_TRUE, origin, region_id, 0, 0, ids, 0, NULL, NULL));
	free(maps);
	free(datas);
	free(ids);
    
    etii_cl_instance *instance = malloc(sizeof(etii_cl_instance));
    instance->context = context;
    instance->program = program;
    instance->map_buffer = map_buffer;
    //instance->parts_buffer = parts_buffer;
	instance->key_buffer = key_buffer;
	instance->img_keys = img_src;
	instance->faceused_buffer = faceused_buffer;
	instance->output_buffer = output_buffer;
	instance->img_output = img_dest;
	instance->qt_buffer = qt_buffer;
    instance->queue = queue;
    instance->mapsizearray = map->sizearray;
	instance->max_research = max_research;
    instance->img_datas = img_datas;
    instance->img_map = img_map;
    instance->img_ids = img_ids;
	instance->partsBuffer = malloc(sizeof(key_part) * MAX_SOURCE_SIZE * max_research);
	instance->resultsBuffer = malloc(sizeof(uint8_t) * max_research);
	//	long results_size = RESULT_BY_SEARCH * instance->max_research * (RESULT_BY_SEARCH +1)*4;
    instance->imgoutflat = malloc(sizeof(int16_t)*max_research*RESULT_BY_SEARCH);
	
    return instance;
}

File **test_opencl(etii_cl_instance *instance, key_part *keys, struct possibility_packet *possiblity, int nbresearch)
{
	//	printf("nb research:%i\n",nbresearch);
	//    struct timeval tmv1, tmv2;
	//	long ms;
	//	gettimeofday(&tmv1, NULL);
    File **results = malloc(sizeof(File*)*nbresearch);
	int nbr =0;
	int r;
	for(r=0; r< nbresearch; r++)
	{
		results[r] = malloc(sizeof(File));
		init_file_with_cache(results[r], RESULT_BY_SEARCH, sizeof(int16_t));
	}
	
	long ids_size = 1 * instance->max_research * 4;
    int8_t *keysflat = malloc(sizeof(int8_t)*ids_size);
    int i_positions = 0;
	
	int k;
    for(k = 0; k < nbresearch;k++)
    {
		key_part key = keys[k];
		keysflat[i_positions] = key.k1;
		keysflat[i_positions +1] = key.k2;
		keysflat[i_positions +2] = key.k3;
		keysflat[i_positions +3] = key.k4;
		
		i_positions = i_positions +4;
	}
	
	size_t origin[] = {0,0,0};
	size_t region[] = {1,instance->max_research, 1};
    CL_CHECK(clEnqueueWriteImage(instance->queue, instance->img_keys, CL_TRUE, origin, region, 0, 0, keysflat, 0, NULL, NULL));
	free(keysflat);
    
	CL_CHECK(clEnqueueWriteBuffer(instance->queue, instance->key_buffer, CL_TRUE, 0, sizeof(key_part)*nbresearch, keys, 0, NULL, NULL));
	uint8_t *faceused = malloc(sizeof(uint8_t)*256*nbresearch);
	for (int i=0; i<nbresearch; i++) {
		struct possibility_packet poss =possiblity[i];
		for(int p=0;p<256;p++) {
			faceused[i*256+p]=poss.faceused[p];
		}
	}
	CL_CHECK(clEnqueueWriteBuffer(instance->queue, instance->faceused_buffer, CL_TRUE, 0, sizeof(uint8_t)*256*nbresearch, faceused, 0, NULL, NULL));
	free(faceused);
	
	cl_kernel kernel;
	CL_CHECK_ERR(clCreateKernel(instance->program, "search_part_img", &_err), kernel);
	CL_CHECK(clSetKernelArg(kernel, 0, sizeof(instance->img_keys), &instance->img_keys));
	CL_CHECK(clSetKernelArg(kernel, 1, sizeof(instance->output_buffer), &instance->output_buffer));
	CL_CHECK(clSetKernelArg(kernel, 2, sizeof(instance->qt_buffer), &instance->qt_buffer));
    int result_by_search = RESULT_BY_SEARCH;
    CL_CHECK(clSetKernelArg(kernel, 3, sizeof(int), &result_by_search));
	CL_CHECK(clSetKernelArg(kernel, 4, sizeof(instance->faceused_buffer), &instance->faceused_buffer));
	CL_CHECK(clSetKernelArg(kernel, 5, sizeof(instance->img_map), &instance->img_map));
	CL_CHECK(clSetKernelArg(kernel, 6, sizeof(instance->img_datas), &instance->img_datas));
	CL_CHECK(clSetKernelArg(kernel, 7, sizeof(instance->img_ids), &instance->img_ids));
	CL_CHECK(clSetKernelArg(kernel, 8, sizeof(int), &instance->mapsizearray));
	CL_CHECK(clSetKernelArg(kernel, 9, sizeof(int), &nbresearch));
	
	
	cl_event kernel_completion;
	size_t global_work_size[1] = { nbresearch*64};
	size_t local_work_size[1] = { 64 };
	
	CL_CHECK(clEnqueueNDRangeKernel(instance->queue, kernel, 1, NULL, global_work_size, local_work_size, 0, NULL, &kernel_completion));
	CL_CHECK(clWaitForEvents(1, &kernel_completion));
	CL_CHECK(clReleaseEvent(kernel_completion));
	
	//	gettimeofday(&tmv2, NULL);
	//	ms = (tmv2.tv_sec-tmv1.tv_sec)*1000
	//	+ (tmv2.tv_usec-tmv1.tv_usec)/1000;
	//	printf("-duree = %ld ms | r:%i\n", ms, nbr);
	//	printf("Result:");
	
	for(int i=0;i< nbresearch;i++) {
		instance->resultsBuffer[i]=0;
	}
	
	CL_CHECK(clEnqueueReadBuffer(instance->queue, instance->qt_buffer, CL_TRUE, 0, sizeof(uint8_t) * instance->max_research, instance->resultsBuffer, 0, NULL, NULL));
	
	CL_CHECK(clEnqueueReadBuffer(instance->queue, instance->output_buffer, CL_TRUE, 0, sizeof(int16_t) * result_by_search * nbresearch, instance->imgoutflat, 0, NULL, NULL));
	
	for (int y= 0; y< nbresearch; y++) {
		int posy = y * RESULT_BY_SEARCH;
		if(instance->resultsBuffer[y] >0) {
			for(int x=0; x <instance->resultsBuffer[y];x++) {
				int16_t kp;
				
				kp = instance->imgoutflat[posy + x];
				put(results[y], &kp);
				nbr++;
			}
		}
	}
	
	CL_CHECK(clReleaseKernel(kernel));
	//	gettimeofday(&tmv2, NULL);
	//	ms = (tmv2.tv_sec-tmv1.tv_sec)*1000
	//	+ (tmv2.tv_usec-tmv1.tv_usec)/1000;
	//printf("duree = %ld ms | r:%i\n", ms, nbr);
	
	return results;
}

int free_etii_cl_instance(etii_cl_instance *instance)
{
    CL_CHECK(clReleaseMemObject(instance->map_buffer));
	CL_CHECK(clReleaseMemObject(instance->key_buffer));
	CL_CHECK(clReleaseMemObject(instance->faceused_buffer));
	CL_CHECK(clReleaseMemObject(instance->output_buffer));
	CL_CHECK(clReleaseMemObject(instance->qt_buffer));
    CL_CHECK(clReleaseMemObject(instance->img_datas));
    CL_CHECK(clReleaseMemObject(instance->img_map));
	CL_CHECK(clReleaseMemObject(instance->img_ids));
	CL_CHECK(clReleaseMemObject(instance->img_keys));
	CL_CHECK(clReleaseMemObject(instance->img_output));
	
    
    CL_CHECK(clReleaseCommandQueue(instance->queue));
    
    CL_CHECK(clReleaseProgram(instance->program));
	CL_CHECK(clReleaseContext(instance->context));
	free(instance->partsBuffer);
	free(instance->resultsBuffer);
	free(instance->imgoutflat);
    return 0;
}