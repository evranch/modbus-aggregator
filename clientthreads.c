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

#include "clientthreads.h"

void *poll_station(void *client_struct)
{

  modbus_t *mb_poll;
  uint8_t tab_bits[50], tab_bits_slave[50]={0}, tab_bits_master[50]={0};
  uint8_t tab_input_bits[50];
  uint16_t tab_input_registers[50];
  uint16_t tab_registers[50];
  bool connection_live = false;

  client_config thisclient = *((client_config*)client_struct);

  printf("Polling IP:%s Port:%s at offset %d, slaveid %d\n",thisclient.ipaddress,thisclient.port,thisclient.offset,thisclient.slaveid);

  mb_poll = modbus_new_tcp_pi(thisclient.ipaddress, thisclient.port);

  modbus_set_error_recovery(mb_poll, MODBUS_ERROR_RECOVERY_LINK);

  modbus_set_slave(mb_poll,thisclient.slaveid);

  while(1)
  {

    // If connection is broken, close it and block until reopened
    if (!connection_live)
    {
      modbus_close(mb_poll);
      while(modbus_connect(mb_poll) != 0)
      {
          mb_mapping->tab_input_bits[thisclient.offset + thisclient.input_num] = connection_live;
          perror("Connection broken, reconnecting");
          sleep(thisclient.poll_delay);
      }
    }

    connection_live = true;

    sleep(thisclient.poll_delay);
    printf("Poll @ %d\n",thisclient.offset);

    // Read coil bits
    if (modbus_read_bits(mb_poll, thisclient.coil_start, thisclient.coil_num, tab_bits) == -1)
    {
      connection_live = false;
      continue;
    }

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
    if(modbus_read_input_bits(mb_poll, thisclient.input_start, thisclient.input_num, tab_input_bits) == -1)
    {
      connection_live = false;
      continue;
    }

    // Debug input bits
    for (size_t i = 0; i < thisclient.coil_num; i++)
    {
      printf("Input %ld:%d\n",i,tab_input_bits[i]);
    }

    // Copy read bits into main modbus table
    for (size_t i = 0; i < thisclient.input_num; i++)
    {
      mb_mapping->tab_input_bits[thisclient.offset+i]=tab_input_bits[i];
    }

    // Read input registers
    if (modbus_read_input_registers(mb_poll, thisclient.ir_start, thisclient.ir_num, tab_input_registers) == -1)
    {
      connection_live = false;
      continue;
    }

    // Copy read input registers into main modbus table
    for (size_t i = 0; i < thisclient.ir_num; i++)
    {
      mb_mapping->tab_input_registers[thisclient.offset+i]=tab_input_registers[i];
    }

    // Read holding registers
    if(modbus_read_registers(mb_poll, thisclient.hr_start, thisclient.hr_num, tab_registers) == -1)
    {
      connection_live = false;
      continue;
    }

    // Copy read holding registers into main modbus table
    for (size_t i = 0; i < thisclient.hr_num; i++)
    {
      mb_mapping->tab_registers[thisclient.offset+i]=tab_registers[i];
    }

    // Set connection good flag if we made it through all requests
    mb_mapping->tab_input_bits[thisclient.offset + thisclient.input_num] = connection_live;
  }

  modbus_close(mb_poll);
  modbus_free(mb_poll);
}
