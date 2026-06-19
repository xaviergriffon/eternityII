#ifndef eternityII_packed_h
#define eternityII_packed_h
#ifdef WIN32
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop) )
#else
#define PACK( __Declaration__ ) __Declaration__ __attribute__((__packed__))
//#define PACK( __Declaration__ ) __Declaration__
#endif

#endif
