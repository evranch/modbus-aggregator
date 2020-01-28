CC=gcc

all: modbus-agg.c clientthreads.c
	$(CC) -std=gnu99 modbus-agg.c clientthreads.c -o modbus-agg `pkg-config --libs --cflags libmodbus` -lpthread -lconfig
