typedef	signed char int8_t;
typedef unsigned char uint8_t;

typedef	signed short int16_t;
typedef unsigned short uint16_t;

#define ETERN_SIZE 16
#define ETERN_PARTS 256
#define CACHE_POSSIBILITIES 25

struct possibility_packet
	{
		uint8_t x;
		uint8_t y;
		int16_t grid[ETERN_SIZE][ETERN_SIZE];
		uint16_t alloc;
		uint8_t faceused[ETERN_PARTS];
	};

typedef struct struct_key_part
	{
		int8_t k1;
		int8_t k2;
		int8_t k3;
		int8_t k4;
	} key_part;

struct part
{
    int id;
    int8_t top;
    int8_t right;
    int8_t bottom;
    int8_t left;
    int8_t rotation;
};

struct array_part
{
    int size;
    __global struct part *parts;
};

inline int8_t convert_p(int8_t p, int maxFace)
{
	int8_t result = p;
	if(result ==-1)
	{
		result = maxFace + p;
	}
	return result;
}

inline int posy(int i1,int i2, int sizearray)
{
    return i1*sizearray+i2;
}
inline int posx(int i3,int i4, int sizearray)
{
    return i3*sizearray+i4;
}
inline int2 positionxy(int i1,int i2,int i3, int i4, int sizearray)
{
    return (int2)(posx(i3,i4,sizearray),posy(i1,i2,sizearray));
}

inline int2 positionxy_key(key_part p,int sizearray)
{
	int8_t k1 = convert_p(p.k1, sizearray);
	int8_t k2 = convert_p(p.k2, sizearray);
	int8_t k3 = convert_p(p.k3, sizearray);
	int8_t k4 = convert_p(p.k4, sizearray);
    return positionxy(k1,k2,k3,k4,sizearray);
}

// Calcul de x en fonction du nombre de piece utilisée
inline int8_t dirx(int16_t alloc) {
	return alloc % ETERN_SIZE;
}

// Calcul de y en fonction du nombre de piece utilisée et de x
inline int8_t diry(int16_t alloc,int8_t x) {
		return (alloc - x) / ETERN_SIZE;
}

inline void what_search_to_key(int localId,__global struct part *all_rotate_parts, struct possibility_packet possibility, int8_t x, int8_t y, key_part key) {
	switch (localId) {
		case 0:
			key.k1 = -2;
			// TOP
			if(y -1 < 0)
			{
				key.k1 = 0;
			} else
			{
				if(possibility.grid[x][y-1] < 0)
				{
					key.k1 = -1;
				} else
				{
					key.k1 = all_rotate_parts[possibility.grid[x][y-1]].bottom;
				}
			}

			break;
	case 1:
			key.k2 = -2;
			// RIGHT
			if(x + 1 >= ETERN_SIZE)
			{
				key.k2 = 0;
			} else
			{
				if(possibility.grid[x+1][y] < 0)
				{
					key.k2 = -1;
				} else
				{
					key.k2 = all_rotate_parts[possibility.grid[x+1][y]].left;
				}
			}
			
			break;
	case 2:
			key.k3 = -2;
			// BOTTOM
			if(y + 1 >= ETERN_SIZE)
			{
				key.k3 = 0;
			} else
			{
				if(possibility.grid[x][y+1] < 0)
				{
					key.k3 = -1;
				} else
				{
					key.k3 = all_rotate_parts[possibility.grid[x][y+1]].top;
				}
			}
			
		break;
	case 3:
			key.k4 = -2;
			// LEFT
			if(x -1 < 0)
			{
				key.k4 = 0;
			} else
			{
				if(possibility.grid[x-1][y] < 0)
				{
					key.k4 = -1;
				} else
				{
					key.k4 = all_rotate_parts[possibility.grid[x-1][y]].right;
				}
			}
			
			break;
	}
	barrier(CLK_LOCAL_MEM_FENCE);
	
}

// Recherche des possibilités par élimination des sans suite
__kernel void search_possibility(__global struct possibility_packet *possibility, __read_only image2d_t mapParts,int sizearray,__global int16_t *datas,__global struct part *all_rotate_parts, __global struct possibility_packet *resultPossibility, __global unsigned short *nbResult, int maxResulByGroup,__global uint8_t *directions)
{
	const sampler_t sampler=CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;
	
	int groupId = get_group_id(0);
	int localId = get_local_id(0);
	
	
	__local struct possibility_packet possibilities[CACHE_POSSIBILITIES];
	// on déporte la recherche en local
	possibilities[0] = possibility[groupId];
	
	// calcul de la position actuel
	__private uint8_t place = directions[possibility[0].alloc];
	__private int8_t x = dirx(place);
	__private int8_t y = diry(place, x);
	
	__local uint4 resultForKey;
	__local key_part key;
	uint8_t rsize = 0;
	
	// On vérifie qu'aucune piece est déjà proposé
	// suite à un calcul de suite possible
	if(possibility[0].grid[x][y] == -2) {
		// recherche de la clé des possibilités
		what_search_to_key(localId,all_rotate_parts,possibility[0],x,y,key);
		
		if(localId==0) {
			resultForKey = read_imageui(mapParts,sampler,positionxy_key(key,sizearray));
		}
		barrier(CLK_LOCAL_MEM_FENCE);
		
		if(localId < resultForKey.g && localId<12) {
			__local int16_t resultParts[12];
			__local uint8_t positions[12];
			resultParts[localId] = -1;
			
			int16_t partId = datas[resultForKey.r + localId];
			uint16_t id = partId % ETERN_PARTS;
			if(possibility[0].faceused[id]) {
				resultParts[localId] = partId;
			}
			barrier(CLK_LOCAL_MEM_FENCE);
			
			
			if(localId==0) {
				uint8_t u;
				int8_t gap=0;
				for(u=0;u<12;u++) {
					if(resultParts[u] >=0){
						rsize++;
						positions[u] = u +gap;
					} else {
						gap--;
					}
					
				}
			}
			barrier(CLK_LOCAL_MEM_FENCE);
			if(rsize > 0) {
				struct possibility_packet currPossibility = possibilities[0];
				uint16_t nextAlloc = currPossibility.alloc+1;
				if(resultParts[localId] >=0) {
					uint8_t pos = positions[localId];
					uint16_t id = resultParts[localId] % ETERN_PARTS;
					possibilities[pos] = currPossibility;
					possibilities[pos].faceused[id-1] = 1;
					possibilities[pos].grid[x][y] = resultParts[localId];
					possibilities[pos].alloc = nextAlloc;
					possibilities[pos].x = x;
					possibilities[pos].y = y;
					
				}
				barrier(CLK_LOCAL_MEM_FENCE);
			}
			
		}
		barrier(CLK_LOCAL_MEM_FENCE);
		
	} else {
		rsize++;
		// la possibilité est toujours valide donc on la repropose pour la prochaine analyse
		possibility[0].alloc++;
	}
	
	if(localId < rsize) {
		resultPossibility[groupId*maxResulByGroup + localId] = possibilities[localId];
	}
	
	if(localId == 0) {
		nbResult[groupId] = rsize;
	}
	
	barrier(CLK_LOCAL_MEM_FENCE);
	
	//
}

// Recherche des posssibilités en brut
__kernel void search_part_img(__read_only image2d_t img_src, __global short *data_dst, __global uint8_t *dst_qt, int result_by_search, __global uint8_t *src_faceused, __read_only image2d_t map,__read_only image2d_t datas, __read_only image2d_t ids, int sizearray, int nbresearch)
{
	
	//	int i = get_global_id(0);
	int g = get_group_id(0);
	int l = get_local_id(0);
	
	__local uint4 imap;
	__local int4 ikey;
	__local uint8_t used[64];
	__local uint8_t positions[64];
	__local short resultParts[64];
	if(g < nbresearch) {
		const sampler_t sampler=CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;
		
		if(l == 0) {
			int2 xy={0,g};
			ikey = read_imagei(img_src,sampler,xy);
			key_part key;
			key.k1 = ikey.r;
			key.k2 = ikey.g;
			key.k3 = ikey.b;
			key.k4 = ikey.a;
			imap = read_imageui(map,sampler,positionxy_key(key,sizearray));
		}
		used[l] = 0;
		
		barrier(CLK_LOCAL_MEM_FENCE);
		uint p;
		int r = g * result_by_search;
		int nb_real_faces = sizearray-1;
		if(l < imap.b && l < (uint)result_by_search)
		{
			// Recalculer la position pour éviter les debordements de l'image
			int x = (imap.r + l) % 225;
			int y = imap.g + (imap.r - x + l) / 225;
			int2 xy={x,y};
			
			
			int4 ipart = read_imagei(datas,sampler,xy);
			int2 idxy= positionxy(ipart.r,ipart.g,ipart.b,ipart.a,nb_real_faces);
			int4 iid = read_imagei(ids,sampler, idxy);
			
			if(src_faceused[g*256+iid.r -1] == 0)
			{
				resultParts[l] = iid.r + 256 * iid.g;
				//resultParts[l].k4 = ipart.a;
				used[l] = 1;
			}
		}
		barrier(CLK_LOCAL_MEM_FENCE);
		if(l == 0) {
			uint8_t rsize = 0;
			uint8_t u;
			int8_t gap=0;
			for(u=0;u<64;u++) {
				if(used[u] == 1){
					rsize++;
					positions[u] = u +gap;
				} else {
					gap--;
				}
				
			}
			dst_qt[g] = rsize;
			//			printf("g:%i l:%i result:%i\n",g,l,rsize);
		}
		
		barrier(CLK_LOCAL_MEM_FENCE);
		if(used[l] == 1) {
			//			printf("g:%i l:%i\n",g,l);
			data_dst[g*result_by_search + positions[l]] = resultParts[l];
//			int4 values ={resultParts[l].k1,resultParts[l].k2,resultParts[l].k3,resultParts[l].k4};
			//			printf("values:%i,%i,%i,%i\n",values.r,values.g,values.b,values.a);
//			write_imagei (	img_dst, position, values);
		}
	}
	
}
