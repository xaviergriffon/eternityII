ifeq ($(OS),Windows_NT) 
    detected_OS := Windows
else
    detected_OS := $(shell sh -c 'uname 2>/dev/null || echo Unknown')
endif


OPENCLLIB := -lOpenCL
ifeq ($(detected_OS),Darwin)
	OPENCLLIB := -framework OpenCL
endif

# etii_opencl.o ${OPENCLLIB}

# Ajout d'une variable DEBUG pour activer ou désactiver les informations de débogage
DEBUG ?= 0
ifeq ($(DEBUG),1)
    CFLAGS= -Wall -std=gnu99 -Ofast -g
	CLEAN_OBJS =
else
    CFLAGS= -Wall -std=gnu99 -Ofast
	CLEAN_OBJS = rm *.o
endif

EXECUTABLE ?= eternityII

$(EXECUTABLE): logger.o static_variables.o local_socket.o lifo.o tcpclient.o tcpserver.o part.o readdata.o datamanager.o possibility.o etii_protocol.o etii_client.o etii_server.o etii_search.o command_lines.o console.o main.o
	gcc -pthread -o $(EXECUTABLE) logger.o static_variables.o local_socket.o lifo.o tcpclient.o tcpserver.o part.o datamanager.o possibility.o readdata.o etii_protocol.o etii_client.o etii_server.o etii_search.o command_lines.o console.o main.o ${CFLAGS} ${CPPFLAGS}
	$(CLEAN_OBJS)

clean: 
	rm -f *.o $(EXECUTABLE)
