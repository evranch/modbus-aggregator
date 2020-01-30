# modbus-aggregator
Brings multiple Modbus TCP sources together into one map for simple PLC configuration.

Modbus-aggregator was written to help standardize communications between different PLCs and slave devices, as well as overcome some of their weaknesses.

In particular, the main features of modbus-aggregator are as follows:

- places all devices into one map with configurable offset values
- simultaneous, asynchronous polling of all devices using pthreads
- slave failure detection via discrete input
- propagates changes, allowing remote operation of devices with PLCs that update coils every cycle
- option to mirror coils into the discrete input address space, for PLCs that don't implement (01) Read Coil Status
- written in C with minimal dependencies

Modbus-aggregator loads its configuration from the file **nodes.cfg** in the current directory.

Usage is straightforward as shown in nodes-test.cfg: 

Configure up to 100 devices with their own section under "nodes". Note that this application uses **addressing from zero**.

- offset is the starting point in the main address space for this device. Coils/inputs/registers will all be indexed from this point
- poll_delay is the time in seconds between poll events
- slaveid is used if the device is a gateway to multiple slave devices. Set it to 1 if you are unsure
- x_start and x_num define the starting addresses and number of addresses to read on the slave device for each data type
- if a data type is not defined in the config, it will not be polled
- mirror_coils, if true, will read the coils from the slave and place them into the discrete input address space directly after the discrete inputs
- coil_push_only and hr_push_only, if true, will disable change detection for this device and push the state of the coils and holding registers respectively from the PLC to the slave every polling cycle.

All poll threads share access to the same main modbus mapping. There are no mutexes or semaphores in use, as access should not overlap. It is up to the user to avoid collisions within the address space. In the event that address spaces overlap, the application *probably* won't crash, but it will likely return garbage data.

# Building modbus-aggregator
Dependencies:
- libpthread
- libmodbus
- libconfig

under Debian/Ubuntu, libpthread should already be part of your build package so: 

sudo apt install libmodbus-dev libconfig-dev


If you have the dependencies installed, simply type make to build. The binary, modbus-agg, can be installed in a location of your choice.
