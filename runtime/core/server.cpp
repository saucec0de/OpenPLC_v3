// Copyright 2015 Thiago Alves
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http ://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissionsand
// limitations under the License.


// This is the file for the network routines of the OpenPLC. It has procedures
// to create a socket, bind it and start network communication.
// Thiago Alves, Dec 2015
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>

#include "ladder.h"

/** \addtogroup openplc_runtime
 *  @{
 */

using namespace std;

#define NET_BUFFER_SIZE 10000


////////////////////////////////////////////////////////////////////////////////
/// @brief Verify if all errors were cleared on a socket
////////////////////////////////////////////////////////////////////////////////
int getSO_ERROR(int fd) 
{
   int err = 1;
   socklen_t len = sizeof err;
   if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len))
      perror("getSO_ERROR");
   if (err)
      errno = err;              // set errno to the socket SO_ERROR
   return err;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Properly close a socket
////////////////////////////////////////////////////////////////////////////////
void closeSocket(int fd) 
{
   if (fd >= 0) 
   {
      getSO_ERROR(fd); // first clear any errors, which can cause close to fail
      if (shutdown(fd, SHUT_RDWR) < 0) // secondly, terminate the 'reliable' delivery
         if (errno != ENOTCONN && errno != EINVAL) // SGI causes EINVAL
            perror("shutdown");
      if (close(fd) < 0) // finally call close()
         perror("close");
   }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Set or Reset the O_NONBLOCK flag from sockets
////////////////////////////////////////////////////////////////////////////////
bool SetSocketBlockingEnabled(int fd, bool blocking)
{
   if (fd < 0) return false;
   int flags = fcntl(fd, F_GETFL, 0);
   if (flags == -1) return false;
   flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
   return (fcntl(fd, F_SETFL, flags) == 0) ? true : false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Create the socket and bind it.
/// @param port
/// @return the file descriptor for the socket created
////////////////////////////////////////////////////////////////////////////////
int createSocket(uint16_t port)
{
    int socket_fd;
    struct sockaddr_in server_addr;

    //Create TCP Socket
    socket_fd = socket(AF_INET,SOCK_STREAM,0);
    if (socket_fd < 0)
    {
        spdlog::error("Server: error creating stream socket => {}", strerror(errno));
        return -1;
    }
    
    //Set SO_REUSEADDR
    int enable = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) 
    {
        spdlog::error("setsockopt(SO_REUSEADDR) failed");
    }
        
    
    SetSocketBlockingEnabled(socket_fd, false);

    //Initialize Server Struct
    bzero((char *) &server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    //Bind socket
    if (bind(socket_fd,(struct sockaddr *)&server_addr,sizeof(server_addr)) < 0)
    {
        spdlog::error("Server: error binding socket => {}", strerror(errno));
        return -1;
    }
    
    // we accept max 5 pending connections
    listen(socket_fd,5);
    spdlog::info("Server: Listening on port => {}", port);
    return socket_fd;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Blocking call. Wait here for the client to connect.
/// @param socket_fd The socket file descriptor. 
/// @param run_server A flag to terminate this client.
/// @return  file descriptor to communicate with the client
////////////////////////////////////////////////////////////////////////////////
int waitForClient(int socket_fd, volatile bool& run_server)
{
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len;

    spdlog::debug("Server: waiting for new client...");

    client_len = sizeof(client_addr);
    while (run_server)
    {
        client_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &client_len); //non-blocking call
        if (client_fd > 0)
        {
            SetSocketBlockingEnabled(client_fd, true);
            break;
        }
        
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    return client_fd;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Blocking call. Holds here until something is received from the client.
/// Once the message is received, it is stored on the buffer and the function
/// @param client_fd
/// @param buffer
/// @return  the number of bytes received.
////////////////////////////////////////////////////////////////////////////////
int listenToClient(int client_fd, unsigned char *buffer)
{
    bzero(buffer, NET_BUFFER_SIZE);
    int n = read(client_fd, buffer, NET_BUFFER_SIZE);
    return n;
}

/// Arguments passed to the server thread.
struct ServerArgs
{
    /// The client file descriptor for reading and writing.
    int client_fd;
    /// Set to false when the server should terminate.
    volatile bool* run;
    /// A function to handle received message buffers.
    process_message_fn process_message;
    /// A user provided data structure for the process message function.
    void* user_data;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Thread to handle requests for each connected client
////////////////////////////////////////////////////////////////////////////////
void *handleConnections(void *arguments)
{
    auto args = reinterpret_cast<ServerArgs*>(arguments);

    unsigned char buffer[NET_BUFFER_SIZE];
    int messageSize;

    spdlog::debug("Server: Thread created for client ID: {}", args->client_fd);

    while(*args->run)
    {
        messageSize = listenToClient(args->client_fd, buffer);
        if (messageSize <= 0 || messageSize > NET_BUFFER_SIZE)
        {
            // something has  gone wrong or the client has closed connection
            if (messageSize == 0)
            {
                spdlog::debug("Server: client ID: {} has closed the connection", args->client_fd);
            }
            else
            {
                spdlog::error("Server: Something is wrong with the  client ID: {} message Size : {}", args->client_fd, messageSize);
            }
            break;
        }

        int messageSize = args->process_message(buffer, NET_BUFFER_SIZE, args->user_data);
        write(args->client_fd, buffer, messageSize);
    }
    
    spdlog::debug("Closing client socket and calling pthread_exit");
    close(args->client_fd);
    spdlog::info("Terminating server connections thread");
    pthread_exit(NULL);
    delete args;
}

/// @brief Function to start a socket server.
///
/// It receives the port number as argument and
/// creates an infinite loop to listen and parse the messages sent by the
/// clients
/// @param port The port to listen on.
/// @param process_message A function to run to process socket messages.
/// @param user_data Passed into the process_message function as client data.
void startServer(uint16_t port, volatile bool& run_server, process_message_fn process_message, void* user_data)
{
    int socket_fd, client_fd;
    
    socket_fd = createSocket(port);
    
    while(run_server)
    {
        client_fd = waitForClient(socket_fd, run_server); //block until a client connects
        if (client_fd < 0)
        {
            spdlog::error("Server: Error accepting client!");
            continue;
        }

        pthread_t thread;
        auto args = new ServerArgs
        {
            .client_fd=client_fd,
            .run=&run_server,
            .process_message=process_message,
            .user_data=user_data
        };
        spdlog::trace("Server: Client accepted! Creating thread for the new client ID: {}...", client_fd);
        int success = pthread_create(&thread, NULL, handleConnections, args);
        if (success == 0)
        {
            pthread_detach(thread);
        }
        else
        {
            delete args;
        }
    }
    close(socket_fd);
    close(client_fd);

    spdlog::debug("Terminating server thread");
}

/** @}*/
