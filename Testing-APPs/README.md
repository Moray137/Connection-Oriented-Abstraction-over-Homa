This directory contains all testing source files used for testing our implementation of connection-oriented abstraction over Homa.

- To use these files for testing, you need to install our implementation following the guide in README.md of (https://github.com/Moray137/Connection-Oriented-Abstraction-over-Homa) when testing files without any protocol names, for example, `client_1KB.c`. To test with vanilla HomaModule, install the module of vanilla HomaModule: (https://github.com/PlatformLab/HomaModule). When using vanilla HomaModule, make sure that the local repo has the exact commit `6f58bef`.
- Type `gcc-14 file_name.c homa_api.c output_name` to compile the file of interest. Note that the files are in pair with their names.  `xxx_homa_size.c` are vanilla Homa apps; `xxx_size.c` are apps with our implementation and `xxx_tcp_size.c` are TCP apps, which do not need extra module installed. Note that you need the corresponding `homa_api.c` source file of the runtime library and `homa.h` header file to run the experiments. The vanilla HomaModule and our implementation DO NOT share these files. 
- Client applications takes one parameter, which is the number of the client sockets. The server does not take any parameters.
- Run the server application first, then the client.
- The client apps will print the throughput as OPs/sec on the console. To stop the server process, press ^C in terminal.