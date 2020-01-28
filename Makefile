all: modbus-server.c
	gcc -std=gnu99 modbus-agg.c -o modbus-agg `pkg-config --libs --cflags libmodbus` -lpthread -lconfig
