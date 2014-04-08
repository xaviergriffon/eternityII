CFLAGS= -Wall -std=gnu99 -lm -Ofast
eternityII: lifo.o tcpclient.o tcpserver.o part.o readdata.o datamanager.o possibility.o etii_protocol.o etii_opencl.o main.o
	gcc -lpthread -lOpenCL -o eternityII lifo.o tcpclient.o tcpserver.o part.o datamanager.o possibility.o readdata.o etii_protocol.o etii_opencl.o main.o ${CFLAGS} ${CPPFLAGS}
