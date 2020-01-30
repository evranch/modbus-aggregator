/*
 * Modbus Aggregator
 * Copyright © 2020 Alex Evans
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
#define THREADPOOL  100

#include "clientthreads.h"
#include "modbus-agg.h"


// Modbus globals
modbus_t *ctx = NULL;
int server_socket = -1;
modbus_mapping_t *mb_mapping;

int main(int argc, char **argv) {

    // Thread pool
    pthread_t pollthread[THREADPOOL];

    // Modbus server vars
    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
    int master_socket;
    int rc;
    fd_set refset;
    fd_set rdset;
    int fdmax;

    // Getopt vars
    int c;
    const char *ip_addr = NULL;
    char *port_s = NULL;
    int mb_port;

    // Libconfig vars
    config_t cfg;
    config_setting_t *setting;
    const char *c_ip_addr = NULL;
    int c_port = 0;
    int node_count = 0;
    client_config *nodesetup[THREADPOOL];
    int debug_level = 1;
    int largest_coil = 0, largest_input = 0, largest_hr = 0, largest_ir = 0;

    // Libconfig section
    config_init(&cfg);

    // Read config file in local directory
    if(! config_read_file(&cfg, "nodes.cfg"))
    {
      fprintf(stderr, "%s:%d - %s\n", config_error_file(&cfg),
            config_error_line(&cfg), config_error_text(&cfg));
            config_destroy(&cfg);
      return(EXIT_FAILURE);
    }

    // Look for global config variables
    config_lookup_int(&cfg, "debug", &debug_level);

    if (config_lookup_string(&cfg,"ip_addr",&c_ip_addr))
    {
      printf("Config file: listening address %s\n",c_ip_addr);
    }

    if (config_lookup_int(&cfg,"port",&c_port))
    {
      printf("Config file: listening port %d\n",c_port);
    }

    // Parse node list
    setting = config_lookup(&cfg, "nodes");

    if(setting != NULL)
    {
      int count = config_setting_length(setting);
      int i;

      printf("\n%d nodes defined\n\n",count);

      if (count > THREADPOOL)
      {
        printf("Too many nodes in file! Max %d", THREADPOOL);
        return -1;
      }

      // Create array of node configurations
      // I'm sure this can be reworked to get rid of local variables
      // and strncpys but this works well enough for now.
      // If variables aren't initialized to zero then they can contain random
      // data on the Raspberry Pi!
      for(i = 0; i < count; ++i)
      {
        const char *c_name, *c_ipaddress, *c_port;
        int c_offset = 0, c_slaveid = 0, c_poll_delay = 0;
        int c_coil_start = 0, c_coil_num = 0;
        int c_input_start = 0, c_input_num = 0;
        int c_ir_start = 0, c_ir_num = 0;
        int c_hr_start = 0, c_hr_num = 0;
        int c_coil_push_only = 0, c_hr_push_only = 0;
        int c_coil_dir_mask = 0, c_hr_dir_mask = 0;
        int c_debug = 0, c_mirror_coils = 0;

        config_setting_t *node = config_setting_get_elem(setting, i);
        config_setting_lookup_string(node, "name", &c_name);
        config_setting_lookup_string(node, "ipaddress", &c_ipaddress);
        config_setting_lookup_string(node, "port", &c_port);

        config_setting_lookup_int(node, "slaveid", &c_slaveid);
        config_setting_lookup_int(node, "offset", &c_offset);
        config_setting_lookup_int(node, "poll_delay", &c_poll_delay);
        config_setting_lookup_int(node, "debug", &c_debug);

        config_setting_lookup_int(node, "coil_start", &c_coil_start);
        config_setting_lookup_int(node, "coil_num", &c_coil_num);
        config_setting_lookup_int(node, "input_start", &c_input_start);
        config_setting_lookup_int(node, "input_num", &c_input_num);
        config_setting_lookup_int(node, "hr_start", &c_hr_start);
        config_setting_lookup_int(node, "hr_num", &c_hr_num);
        config_setting_lookup_int(node, "ir_start", &c_ir_start);
        config_setting_lookup_int(node, "ir_num", &c_ir_num);

        config_setting_lookup_bool(node, "coil_push_only", &c_coil_push_only);
        config_setting_lookup_bool(node, "hr_push_only", &c_hr_push_only);

        config_setting_lookup_bool(node, "coil_dir_mask", &c_coil_dir_mask);
        config_setting_lookup_bool(node, "hr_dir_mask", &c_hr_dir_mask);

        config_setting_lookup_bool(node, "mirror_coils", &c_mirror_coils);

        if (debug_level)
        {
          printf("Node %d: %s\n",i,c_name);
          printf("-------\n");
          printf("%s:%s slave #%d offset: %d Polling every %ds\n",c_ipaddress,c_port,c_slaveid, c_offset, c_poll_delay);
          if (c_coil_num)
            printf("%d Coils: %d - %d mapped to %d - %d\n",c_coil_num,c_coil_start,c_coil_start+c_coil_num-1,c_coil_start+c_offset,c_coil_start+c_coil_num+c_offset-1);
          if (c_input_num)
            printf("%d Inputs: %d - %d mapped to %d - %d\n",c_input_num,c_input_start,c_input_start+c_input_num-1,c_input_start+c_offset,c_input_start+c_input_num+c_offset-1);
          if (c_mirror_coils)
            printf("%d Mirrored Coils: %d - %d mapped to inputs %d - %d\n",c_coil_num,c_coil_start,c_coil_start+c_coil_num-1,c_coil_start+c_offset+c_input_num,c_coil_start+c_coil_num+c_offset+c_input_num-1);
          if (c_hr_num)
            printf("%d Holding regs: %d - %d mapped to %d - %d\n",c_hr_num,c_hr_start,c_hr_start+c_hr_num-1,c_hr_start+c_offset,c_hr_start+c_hr_num+c_offset-1);
          if (c_ir_num)
          printf("%d Input regs: %d - %d mapped to %d - %d\n",c_ir_num,c_ir_start,c_ir_start+c_ir_num-1,c_ir_start+c_offset,c_ir_start+c_ir_num+c_offset-1);
          printf("Connection live bit at input %d\n\n",c_input_start+c_input_num+c_offset+(c_coil_num*c_mirror_coils));
        }

        // Track largest address for main mapping context allocation
        largest_coil = max(largest_coil, c_coil_start+c_coil_num+c_offset);
        largest_input = max(largest_input, c_input_start+c_input_num+c_offset+(c_coil_num*c_mirror_coils)+1);
        largest_hr = max(largest_hr,c_hr_start+c_hr_num+c_offset);
        largest_ir = max(largest_ir,c_ir_start+c_ir_num+c_offset);

        nodesetup[i] = malloc(sizeof(client_config));

        strncpy(nodesetup[i]->name, c_name, sizeof(((client_config){0}).name));
        strncpy(nodesetup[i]->ipaddress, c_ipaddress, sizeof(((client_config){0}).ipaddress));
        strncpy(nodesetup[i]->port, c_port, sizeof(((client_config){0}).port));
        nodesetup[i]->offset=c_offset;
        nodesetup[i]->slaveid=c_slaveid;
        nodesetup[i]->poll_delay=c_poll_delay;

        nodesetup[i]->coil_start = c_coil_start;
        nodesetup[i]->coil_num = c_coil_num;
        nodesetup[i]->input_start = c_input_start;
        nodesetup[i]->input_num = c_input_num;
        nodesetup[i]->hr_start = c_hr_start;
        nodesetup[i]->hr_num = c_hr_num;
        nodesetup[i]->ir_start = c_ir_start;
        nodesetup[i]->ir_num = c_ir_num;


        nodesetup[i]->coil_push_only = c_coil_push_only;
        nodesetup[i]->hr_push_only = c_hr_push_only;

        nodesetup[i]->coil_dir_mask = c_coil_dir_mask;
        nodesetup[i]->hr_dir_mask = c_hr_dir_mask;

        nodesetup[i]->debug = c_debug;
        nodesetup[i]->mirror_coils = c_mirror_coils;

      }

      node_count = count;
    }
    else
    {
      printf("No nodes defined in configuration file. Exiting...\n");
      return -1;
    }

    // Getopt section - override listening address and port
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

    // Default to listening on all interfaces
    if (ip_addr == NULL) {
      if (c_ip_addr == NULL)
      {
        ip_addr = "0.0.0.0";
      } else {
        ip_addr = c_ip_addr;
      }
    } else if(!is_valid_ip(ip_addr)) {
        printf("%s is not a valid ip address, please try with a proper ip address \n", ip_addr);
        return -1;
    }

    // Default to port 1502 (assuming non-superuser)
    if (port_s == NULL) {
      if (c_port == 0)
      {
        mb_port = 1502;
      } else {
        mb_port = c_port;
      }
    } else if (atoi(port_s) > 0) {
        mb_port = atoi(port_s);
    } else {
        printf("%s is not a valid port, please try with a proper port \n", port_s);
        return -1;
    }

    printf("Listening on %s:%d \n", ip_addr, mb_port);

    ctx = modbus_new_tcp(ip_addr, mb_port);

    // Allocate main modbus map
    if (debug_level > 1)
      printf("Allocating main modbus map. %d coils, %d inputs, "\
       "%d registers, %d input registers\n", largest_coil, largest_input,\
        largest_hr, largest_ir);

    mb_mapping = modbus_mapping_new(largest_coil, largest_input, largest_hr, largest_ir);

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

    // Start up polling threads and pass node setups
    for (int i = 0; i < node_count; i++) {

      if (debug_level > 1)
        fprintf(stderr, "Creating poll thread %d\n", i);

      if(pthread_create(&pollthread[i], NULL, poll_station, nodesetup[i])){
        fprintf(stderr, "Poll thread %d creation failed\n", i);
      }
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

                    if (debug_level > 2)
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
                     if (debug_level > 2)
                     {
                       printf("Connection closed on socket %d\n", master_socket);
                       printf("Error %s\n",modbus_strerror(errno));
                     }
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

int is_valid_ip(const char *ip_address) {
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ip_address, &(sa.sin_addr));
    return result != 0;
}

int max(int x, int y)
{
  return (((x) > (y)) ? (x) : (y));
}
