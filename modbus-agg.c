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

#include <libconfig.h>


#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#define NB_CONNECTION    INT_MAX

#include "clientthreads.h"
#include "modbus-agg.h"

modbus_t *ctx = NULL;
int server_socket = -1;
modbus_mapping_t *mb_mapping;



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
        mb_port = 1505;
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
      char port[10];
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

    strcpy(testsetup->ipaddress, "mb-quonset.evranch");
    strcpy(testsetup->port,"502");
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

// MAIN SERVER LOOP
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
