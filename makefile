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

# Active l'interface ncurses optionnelle (par défaut : interface ANSI sans
# dépendance externe). Avec NCURSES=1, on compile logger_ncurses.c à la place
# de logger.c et on lie -lncurses. Le binaire reste 100% compilable sans
# ncurses tant que NCURSES est laissé à 0.
NCURSES ?= 0
ifeq ($(NCURSES),1)
    CFLAGS += -DUSE_NCURSES
    LOGGER_OBJ := logger_ncurses.o
    NCURSES_LIB := -lncurses
else
    LOGGER_OBJ := logger.o
    NCURSES_LIB :=
endif

$(EXECUTABLE): $(LOGGER_OBJ) static_variables.o local_socket.o lifo.o tcpclient.o tcpserver.o part.o readdata.o datamanager.o possibility.o etii_protocol.o etii_client.o etii_server.o etii_search.o command_lines.o command_history.o console.o main.o
	gcc -pthread -o $(EXECUTABLE) $(LOGGER_OBJ) static_variables.o local_socket.o lifo.o tcpclient.o tcpserver.o part.o datamanager.o possibility.o readdata.o etii_protocol.o etii_client.o etii_server.o etii_search.o command_lines.o command_history.o console.o main.o ${CFLAGS} ${CPPFLAGS} $(NCURSES_LIB)
	$(CLEAN_OBJS)

clean:
	rm -f *.o $(EXECUTABLE)
