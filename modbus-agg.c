/*
 * Copyright © 2008-2014 Stéphane Raimbault <stephane.raimbault@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the BSD License.
 */


#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <stdbool.h>

#include <modbus.h>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#define NB_CONNECTION    INT_MAX

#include <pthread.h>

// Polling timer
time_t last_polled;
int data_valid = 0;

modbus_t *ctx = NULL;
int server_socket = -1;
modbus_mapping_t *mb_mapping;

typedef struct client_config
{
  char ipaddress[50];
  int port;
  int slaveid;
  int coil_start;
  int coil_num;
  int input_start;
  int input_num;
  int hr_start;
  int hr_num;
  int ir_start;
  int ir_num;
  int offset;
  int poll_delay;
} client_config;

static void close_sigint(int dummy)
{
    if (server_socket != -1) {
        close(server_socket);
    }
    modbus_free(ctx);
    modbus_mapping_free(mb_mapping);

    exit(dummy);
}

int is_valid_ip(char *ip_address) {
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ip_address, &(sa.sin_addr));
    return result != 0;
}

void *poll_station(void *client_struct)
{

/*
struct client_config
{
  char ipaddress[50];
  int coil_start;
  int coil_num;
  int input_start;
  int input_num;
  int hr_start;
  int hr_num;
  int ir_start;
  int ir_num;
  int poll_delay;
}*/

  modbus_t *mb_poll;
  uint8_t tab_bits[50], tab_bits_slave[50], tab_bits_master[50];
  uint8_t tab_input_bits[50];
  uint16_t tab_input_registers[50];
  uint16_t tab_registers[50];

  client_config thisclient = *((client_config*)client_struct);

    printf("Polling IP:%s Port:%d at offset %d, slaveid %d\n",thisclient.ipaddress,thisclient.port,thisclient.offset,thisclient.slaveid);

  mb_poll = modbus_new_tcp(thisclient.ipaddress, thisclient.port);

  modbus_set_slave(mb_poll,thisclient.slaveid);
  modbus_connect(mb_poll);

  while(1)
  {
    sleep(thisclient.poll_delay);
    printf("Poll @ %d\n",thisclient.offset);

    // Read coil bits
    modbus_read_bits(mb_poll, thisclient.coil_start, thisclient.coil_num, tab_bits);

    // Debug tables
    for (size_t i = 0; i < thisclient.coil_num; i++)
    {
      printf("Master:%d Last:%d Slave:%d Last:%d\n",mb_mapping->tab_bits[thisclient.offset+i],tab_bits_master[i],tab_bits[i],tab_bits_slave[i]);
    }

    // Check for change on slave side and update last state
    bool slave_changed = false;
    for (size_t i = 0; i < thisclient.coil_num; i++)
    {
      if (tab_bits[i] != tab_bits_slave[i])
        slave_changed = true;

      tab_bits_slave[i] = tab_bits[i];
    }

    // Check for change on master side and update last state
    bool master_changed = false;
    for (size_t i = 0; i < thisclient.coil_num; i++)
    {
      if (mb_mapping->tab_bits[thisclient.offset+i] != tab_bits_master[i])
        master_changed = true;

      tab_bits_master[i] = mb_mapping->tab_bits[thisclient.offset+i];
    }

    // Prioritize change pushed from master
    if (master_changed)
    {
      // Write coils from main mobdus table
      modbus_write_bits(mb_poll, thisclient.coil_start, thisclient.coil_num,&mb_mapping->tab_bits[thisclient.offset]);
    } else if (slave_changed)
    {
      // Copy read coils into main modbus table
      for (size_t i = 0; i < thisclient.coil_num; i++)
      {
        mb_mapping->tab_bits[thisclient.offset+i]=tab_bits[i];
      }
    }

    // Read input bits
    modbus_read_bits(mb_poll, thisclient.input_start, thisclient.input_num, tab_input_bits);

    // Copy read bits into main modbus table
    for (size_t i = 0; i < thisclient.input_num; i++)
    {
      mb_mapping->tab_input_bits[thisclient.offset+i]=tab_input_bits[i];
    }

    // Read input registers
    modbus_read_input_registers(mb_poll, thisclient.ir_start, thisclient.ir_num, tab_input_registers);

    // Copy read input registers into main modbus table
    for (size_t i = 0; i < thisclient.ir_num; i++)
    {
      mb_mapping->tab_input_registers[thisclient.offset+i]=tab_input_registers[i];
    }

    // Read holding registers
    modbus_read_registers(mb_poll, thisclient.hr_start, thisclient.hr_num, tab_registers);

    // Copy read holding registers into main modbus table
    for (size_t i = 0; i < thisclient.hr_num; i++)
    {
      mb_mapping->tab_registers[thisclient.offset+i]=tab_registers[i];
    }
  }

  modbus_close(mb_poll);
  modbus_free(mb_poll);
}

int main(int argc, char **argv) {

    pthread_t pollthread;

    // Modbus vars
    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
    int master_socket;
    int rc;
    fd_set refset;
    fd_set rdset;
    /* Maximum file descriptor number */
    int fdmax;

    /* Getting the options through getopt */
    int c;
    char *ip_addr = NULL;
    char *port_s = NULL;
    int mb_port;

    opterr = 0;

    while ((c = getopt (argc, argv, "a:p:")) != -1)
        switch (c)
        {
            case 'a':
                ip_addr = optarg;
                break;
            case 'p':
                port_s = optarg;
                break;
            case '?':
                if ((optopt == 'a')&&(optopt == 'p'))
                    fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                else if (isprint (optopt))
                    fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                else
                    fprintf (stderr,
                            "Unknown option character `\\x%x'.\n",
                            optopt);
                return 1;
            default:
                abort ();
        }

    for (int index = optind; index < argc; index++)
        printf ("Non-option argument %s\n", argv[index]);

    if (ip_addr == NULL) {
        ip_addr = "0.0.0.0";
    } else if(!is_valid_ip(ip_addr)) {
        printf("%s is not a valid ip address, please try with a proper ip address \n", ip_addr);
        return -1;
    }

    if (port_s == NULL) {
        mb_port = 1503;
    } else if (atoi(port_s) > 0) {
        mb_port = atoi(port_s);
    } else {
        printf("%s is not a valid port, please try with a proper port \n", port_s);
        return -1;
    }

    printf("ip_addr : %s, port : %d \n", ip_addr, mb_port);

    ctx = modbus_new_tcp(ip_addr, mb_port);

    /* For reading registers and bits, the addesses go from 0 to 0xFFFF */
    mb_mapping = modbus_mapping_new(0xFF, 0xFF,
                                    0xFF, 0xFF);
    if (mb_mapping == NULL) {
        fprintf(stderr, "Failed to allocate the mapping: %s\n",
                modbus_strerror(errno));
        modbus_free(ctx);
        return -1;
    }

    server_socket = modbus_tcp_listen(ctx, NB_CONNECTION);

    signal(SIGINT, close_sigint);

    /* Clear the reference set of socket */
    FD_ZERO(&refset);
    /* Add the server socket */
    FD_SET(server_socket, &refset);

    /* Keep track of the max file descriptor */
    fdmax = server_socket;

    /*
    struct client_config
    {
      char ipaddress[50];
      int coil_start;
      int coil_num;
      int input_start;
      int input_num;
      int hr_start;
      int hr_num;
      int ir_start;
      int ir_num;
      int poll_delay;
    }*/

    //pthread_create(pthread_t *restrict __newthread,
    //const pthread_attr_t *restrict __attr,
    //void *(*__start_routine)(void *),
    //void *restrict __arg)
    client_config *testsetup;
    testsetup = malloc(sizeof(client_config));

    strcpy(testsetup->ipaddress, "192.168.1.11");
    testsetup->port=502;
    testsetup->offset=0;
    testsetup->slaveid=1;
    testsetup->poll_delay=1;
    testsetup->coil_start = 0;
    testsetup->coil_num = 8;
    testsetup->input_start = 0;
    testsetup->input_num = 8;

    fprintf(stderr, "Creating poll thread\n");
    if(pthread_create(&pollthread, NULL, poll_station, testsetup)){
      fprintf(stderr, "Poll thread creation failed\n");
    }

// MAIN POLLING LOOP
    for (;;) {

        rdset = refset;
        if (select(fdmax+1, &rdset, NULL, NULL, NULL) == -1) {
            perror("Server select() failure.");
            close_sigint(1);
        }
        /* Run through the existing connections looking for data to be
         * read */
        for (master_socket = 0; master_socket <= fdmax; master_socket++) {

            if (!FD_ISSET(master_socket, &rdset)) {
                continue;
            }

            if (master_socket == server_socket) {

                /* A client is asking a new connection */
                socklen_t addrlen;
                struct sockaddr_in clientaddr;
                int newfd;

                /* Handle new connections */
                addrlen = sizeof(clientaddr);
                memset(&clientaddr, 0, sizeof(clientaddr));
                newfd = accept(server_socket, (struct sockaddr *)&clientaddr, &addrlen);
                if (newfd == -1) {
                    perror("Server accept() error");
                } else {
                    FD_SET(newfd, &refset);

                    if (newfd > fdmax) {
                        /* Keep track of the maximum */
                        fdmax = newfd;
                    }
                    printf("New connection from %s:%d on socket %d\n",
                           inet_ntoa(clientaddr.sin_addr), clientaddr.sin_port, newfd);
                }
            } else {
                modbus_set_socket(ctx, master_socket);
                rc = modbus_receive(ctx, query);
                if (rc > 0) {
                    modbus_reply(ctx, query, rc, mb_mapping);
                } else if (rc == -1) {
                    /* This example server in ended on connection closing or
                     * any errors. */
                    printf("Connection closed on socket %d\n", master_socket);
                    printf("Error %s\n",modbus_strerror(errno));
                    close(master_socket);

                    /* Remove from reference set */
                    FD_CLR(master_socket, &refset);

                    if (master_socket == fdmax) {
                        fdmax--;
                    }
                }
            }
        }
    }

    return 0;
}
