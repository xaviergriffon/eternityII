#ifndef eternityII_readdata_h
#define eternityII_readdata_h
#include "part.h"
#include "possibility.h"

struct array_part *read_parts(const char *files);

struct possibility_packet * read_from_json(const char *json_possiblity);

#endif
