# Fast (Epoll) Server
A scalable server library written for the C10k problem
--------------------------------------------------------------------------------

This is a light-weight, header-only, epoll-based server library designed to achieve a high number of persistent TCP connections. Written in modern C++, the library uses a lock-free concurrency model for high performance thread synchronization. Worker threads are client-agnostic and have access to every client, ensuring that CPU resources aren't wasted on idling.

Installation
--------------------------------------------------------------------------------
As this is a header-only library, the easiest way to use it is to copy all header files from the `fserv` subdirectory directly to your project. `git clone https://github.com/sam-ysf/Fast-Server`, then `cp -r Fast-Server/fserv <your-project-directory>`.

To instead include as a submodule, run `git submodule add https://github.com/sam-ysf/Fast-Server` in your project's root directory followed by `git submodule update --init --recursive`.

Usage
--------------------------------------------------------------------------------
Below is a sample of a bare-bones echo server (see also `sample/echo_server.cpp` for a more robust implementation).

In the following example, a server is created then bound to callbacks that handle newly-arrived messages, out-of-band data, new client connections, and client termination notifications.

```C++
#include "fserv/basic_client.hpp"
#include "fserv/basic_server.hpp"

fserv::BasicServer<fserv::BasicClient> server;

// Handle new client data
// This is triggered when there is a read ready on the socket
server.bind_client_data_received_callback([](fserv::ClientSession<fserv::BasicClient>& client,
                                             const char* message,
                                             const int message_size) {
    // Write back the message
    client.write(message, message_size);

    // Client must be rearmed or it won't be re-triggered on future
    // read events
    client.rearm();

    // To permanently terminate and remove the client, use
    // client.terminate()
});

// Handle new client
// This is triggered on successful client connections
server.bind_new_client_callback([](fserv::ClientSession<fserv::BasicClient>& client) {
    // ...
});

// Handle client error
// This is triggered on socket errors
server.bind_client_error_callback([](fserv::ClientSession<fserv::BasicClient>& client) {
    // ...
});

// Handle connection closed
// This is triggered when the remote endpoint has closed the connection or
// if the connection is killed by timeout
server.bind_client_closed_callback([](fserv::ClientSession<fserv::BasicClient>& client) {
    // ...
});

// Handle new out-of-band data
// This is triggered whenever out-of-band data arrives on the socket
server.bind_oob_received_callback([](fserv::ClientSession<fserv::BasicClient>& client,
                                     char oob_data) {
    // ...
});
```

The server must be bound to a port before it can be run using `BasicServer::run`, which is a blocking call.

```C++
// Bind server to port 60008
int port = 60008;
if (!server.bind(port)) {
    // Handle error
    // Bind can fail if the port is already bound to another socket
}

// Using six worker threads for this example
int worker_count = 6;

// Will allocate memory for a maximum of 50,000 concurrent clients
// Note ~~> Don't forget to increment ulimit if you need to handle more than the default
//          file limit
// See: https://tldp.org/LDP/solrhe/Securing-Optimizing-Linux-RH-Edition-v1.3/x4733.html
int max_concurrent_connections = 5e4;

// Run the server
// Note ~~> This is a blocking call
server.run(worker_count, max_concurrent_connections);
```

Only necessary callbacks need to be implemented. If the application doesn't need to be notified of out-of-band data, it doesn't need to bind an out-of-band read handler.

The library uses _edge triggered_ socket notification, meaning that it is the user's responsibility to handle all read events immediately, as there will be no second notification, and any uncopied buffer data will be discarded.

Because the underlying implementation uses one-shot notification, care must be taken to rearm the client after every event in order to receive subsequent notifications. Although client handling is multithreaded, one-shot notification ensures that read notifications are guaranteed to be triggered in event-arrival order.

`BasicServer::run` utilizes an optional timeout that terminates clients that have been inactive for a period. To enable, a timeout interval must be passed to `BasicServer::run`.

```C++
// Run server with a 10,000 millisecond timeout interval
server.run(worker_count, max_concurrent_connections, 10000);
```

Building the Sample
--------------------------------------------------------------------------------
Before compiling, ensure that you have the necessary ncurses dependencies installed (`apt install libncurses-dev` in Debian).

Create and navigate to `sample/build` of the cloned repository (`git clone https://github.com/sam-ysf/Fast-Server --recursive`) then call `cmake .. && cmake --build .`.

This generates `fserv` (a simple echo server) that can be run from the command line. The sample server writes a default configuration file to `~/.config/fserv/server/config.json` that can later be edited with custom values.

Sources
--------------------------------------------------------------------------------
C10k problem\
<https://wikipedia.org/wiki/C10k_problem>

Edge triggering\
<https://www.geeksforgeeks.org/edge-triggering-and-level-triggering>

__nlohmann/json__ is used by the sample for file parsing\
<https://github.com/nlohmann/json>


--------------------------------------------------------------------------------
This software is entirely in the public domain and is provided as is, without restricitions. See the LICENSE for more information.
