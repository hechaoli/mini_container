# Introduction

This repo contains the code that uses minimal code to implementd a container like docker.
The code is used in my Mini Container blog series including the following posts:
* [Part 0 - Not a Real Container](https://hechao.li/2020/06/09/Mini-Container-Series-Part-0-Not-a-Real-Container/)
* [Part 1 - Filesystem isolation](https://hechao.li/2020/06/09/Mini-Container-Series-Part-1-Filesystem-Isolation/)
* [Part 2 - Process isolation](https://hechao.li/2020/06/10/Mini-Container-Series-Part-2-Process-Isolation/)
* [Part 3 - Hostname and Domain Name Isolation](https://hechao.li/2020/06/18/Mini-Container-Series-Part-3-Host-and-Domain-Name-Isolation/)
* [Part 4 - IPC Isolation](https://hechao.li/2020/06/25/Mini-Container-Series-Part-4-IPC-Isolation/)
* [Part 5 - Network Isolation](https://hechao.li/2020/07/01/Mini-Container-Series-Part-5-Network-Isolation/)
* [Part 6 - Limit memory usage](https://hechao.li/2020/07/09/Mini-Container-Series-Part-6-Limit-Memory-Usage/)

# Build
```
$ mkdir build
$ cd build
$ cmake ..
$ make
```

# Run
```
./mini_container
Usage: ./mini_container [options] COMMAND

Options:
  -h [ --help ]         Print help message
  -v [ --verbose ]      Enable verose logging
  -r [ --rootfs ] arg   Root filesystem path of the container
  -p [ --pid ]          Enable PID isolation
  -h [ --hostname ] arg Hostname of the container
  -d [ --domain ] arg   NIS domain name of the container
  -i [ --ipc ]          Enable IPC isolation
  --ip arg              IP of the container
  -R [ --max-ram ] arg  The max amount of ram (in bytes) that the container can
                        use
```
