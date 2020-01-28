#include <modbus.h>
#include <pthread.h>

extern modbus_mapping_t *mb_mapping;

typedef struct client_config
{
  char ipaddress[50];
  char port[10];
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

void *poll_station(void *client_struct);
