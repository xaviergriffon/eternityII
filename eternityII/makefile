CFLAGS= -Wall -std=gnu99 -lm
eternityII: lifo.o tcpclient.o tcpserver.o part.o readdata.o datamanager.o possibility.o etii_protocol.o main.o
	gcc -lpthread -lreadline -o eternityII lifo.o tcpclient.o tcpserver.o part.o datamanager.o possibility.o readdata.o etii_protocol.o main.o ${CFLAGS} ${CPPFLAGS}
