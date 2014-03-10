typedef	signed char int8_t;
typedef unsigned char uint8_t;

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

__kernel void search_part_img(__global key_part *src, __global struct part *dst_parts, __global int *dst_qt, int result_by_search, __global uint8_t *src_faceused, __read_only image2d_t map,__read_only image2d_t datas, __read_only image2d_t ids, int sizearray, int nbresearch)
{
	
	//	int i = get_global_id(0);
	int g = get_group_id(0);
	int l = get_local_id(0);
	
	__local uint4 imap;
	__local int used[64];
	__local int positions[64];
	__local struct part resultParts[64];
	if(g < nbresearch) {
		const sampler_t sampler=CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;
		
		if(l == 0) {
			
			key_part key = src[g];
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
				resultParts[l].id = iid.r;
				resultParts[l].top = ipart.r;
				resultParts[l].right = ipart.g;
				resultParts[l].bottom = ipart.b;
				resultParts[l].left = ipart.a;
				resultParts[l].rotation = iid.g;
				used[l] = 1;
			}
		}
		barrier(CLK_LOCAL_MEM_FENCE);
		if(l == 0) {
			int rsize = 0;
			int u;
			int gap=0;
			for(u=0;u<64;u++) {
				if(used[u] == 1){
					rsize++;
					positions[u] = u +gap;
				} else {
					gap--;
				}
				
			}
			dst_qt[g] = rsize;
		}
		
		barrier(CLK_LOCAL_MEM_FENCE);
		if(used[l] == 1) {
			int position = r+ positions[l];
			dst_parts[position].id = resultParts[l].id;
			dst_parts[position].top = resultParts[l].top;
			dst_parts[position].right = resultParts[l].right;
			dst_parts[position].bottom = resultParts[l].bottom;
			dst_parts[position].left = resultParts[l].left;
			dst_parts[position].rotation = resultParts[l].rotation;
		}
	}
	
}
