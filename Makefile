all: modbus-server.c
	gcc -std=gnu99 modbus-server.c -o modbus-server `pkg-config --libs --cflags libmodbus` -lpthread
