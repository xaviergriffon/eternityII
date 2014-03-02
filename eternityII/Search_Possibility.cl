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
	
	int i = get_global_id(0);
	int rsize = 0;
	//if(i < nbresearch) {
		key_part key = src[i];
		const sampler_t sampler=CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;
		
		uint4 imap = read_imageui(map,sampler,positionxy_key(key,sizearray));
		
		uint p;
		int r = i * result_by_search;
		int added = 0;
		rsize = imap.b;
		int nb_real_faces = sizearray-1;
		
		for(p=0; p < (uint)result_by_search && p < imap.b;p++)
		{
			// Recalculer la position pour éviter les debordements de l'image
			int x = (imap.r + p) % 225;
			int y = imap.g + (imap.r - x + p) / 225;
			int2 xy={x,y};

	
			int4 ipart = read_imagei(datas,sampler,xy);
			int2 idxy= positionxy(ipart.r,ipart.g,ipart.b,ipart.a,nb_real_faces);
			int4 iid = read_imagei(ids,sampler, idxy);
			
			//printf("part r: %i g:%i b:%i a:%i\n",ipart.r,ipart.g,ipart.b,ipart.a);
			//		printf("iid %i \n",iid.r);
			//		printf("xy.r (id) %i | xy.g: %i | p: %i \n",xy.r,xy.g,p);
			
			if((&src_faceused[i])[iid.r -1] == 0)
			{
				dst_parts[r + added].id = iid.r;
				dst_parts[r + added].top = ipart.r;
				dst_parts[r + added].right = ipart.g;
				dst_parts[r + added].bottom = ipart.b;
				dst_parts[r + added].left = ipart.a;
				dst_parts[r + added].rotation = iid.g;
				
				added++;
				
			} else {
				rsize--;
			}
		}

		//printf("fin global i:%i \n",i);
		dst_qt[i] = rsize;
	//}
	
}
