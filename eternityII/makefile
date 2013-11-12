CFLAGS= -Wall -std=gnu99 -lm
EthServer: lifo.o tcpclient.o tcpserver.o part.o readdata.o datamanager.o possibility.o eth_protocol.o main.o
	gcc -lpthread -lreadline -o EthServer lifo.o tcpclient.o tcpserver.o part.o datamanager.o possibility.o readdata.o eth_protocol.o main.o ${CFLAGS} ${CPPFLAGS}
