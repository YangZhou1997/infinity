/**
 * Examples - Read/Write/Send Operations
 *
 * (c) 2018 Claude Barthels, ETH Zurich
 * Contact: claudeb@inf.ethz.ch
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <cassert>
#include <mutex>
#include <thread>
#include <vector>

#include <infinity/core/Context.h>
#include <infinity/queues/QueuePairFactory.h>
#include <infinity/queues/QueuePair.h>
#include <infinity/memory/Buffer.h>
#include <infinity/memory/RegionToken.h>
#include <infinity/requests/RequestToken.h>

#define PORT_NUMBER 8011
#define SERVER_IP "10.0.0.1"
#define NUM_CLIENTS 8

// Usage: ./progam -s for server and ./program for client component
int main(int argc, char **argv) {

	bool isServer = false;

	while (argc > 1) {
		if (argv[1][0] == '-') {
			switch (argv[1][1]) {

				case 's': {
					isServer = true;
					break;
				}

			}
		}
		++argv;
		--argc;
	}

	infinity::core::Context *context = new infinity::core::Context();
	infinity::queues::QueuePairFactory *qpFactory = new infinity::queues::QueuePairFactory(context);

	if(isServer) {
		printf("Setting up connection (blocking)\n");
		qpFactory->bindToPort(PORT_NUMBER);
        
        std::vector<std::thread> server_threads;
        for(int i = 0; i < NUM_CLIENTS; i++){
        	infinity::queues::QueuePair *qp;

            printf("Creating buffers to read from and write to\n");
    		infinity::memory::Buffer *bufferToReadWrite = new infinity::memory::Buffer(context, 128 * sizeof(char));
    		infinity::memory::RegionToken *bufferToken = bufferToReadWrite->createRegionToken();

    		printf("Creating buffers to receive a message\n");
    		infinity::memory::Buffer *bufferToReceive = new infinity::memory::Buffer(context, 128 * sizeof(char));

    		qp = qpFactory->acceptIncomingConnection(bufferToken, sizeof(infinity::memory::RegionToken));
            
            auto func = [=](){
        		printf("Posting receive buffers to receive a message\n");
                qp->postReceiveBuffer(bufferToReceive);

        		printf("Waiting for message (blocking)\n");
        		infinity::queues::receive_element_t receiveElement;
        		while(!qp->receive(&receiveElement));

        		printf("Message received\n");
        		delete bufferToReadWrite;
        		delete bufferToReceive;
	            delete qp;
            };
            std::thread t(func);
            server_threads.push_back(std::move(t));
        }
		for(auto& t: server_threads){
            t.join();
        }
	} else {
        std::vector<std::thread> client_threads;
        for(int i = 0; i < NUM_CLIENTS; i++){
    	    infinity::queues::QueuePair *qp;

    		printf("Connecting to remote node\n");
    		qp = qpFactory->connectToRemoteHost(SERVER_IP, PORT_NUMBER);
    		infinity::memory::RegionToken *remoteBufferToken = (infinity::memory::RegionToken *) qp->getUserData();

    		printf("Creating buffers\n");
    		infinity::memory::Buffer *buffer1Sided = new infinity::memory::Buffer(context, 128 * sizeof(char));
    		infinity::memory::Buffer *buffer2Sided = new infinity::memory::Buffer(context, 128 * sizeof(char));

            auto func = [=](){
        		printf("Reading content from remote buffer\n");
        		infinity::requests::RequestToken requestToken(qp);
        		qp->read(buffer1Sided, remoteBufferToken, &requestToken);
        		requestToken.waitUntilCompleted();

        		printf("Writing content to remote buffer\n");
        		qp->write(buffer1Sided, remoteBufferToken, &requestToken);
        		requestToken.waitUntilCompleted();

        		printf("Sending message to remote host\n");
        		qp->send(buffer2Sided, &requestToken);
        		requestToken.waitUntilCompleted();

        		delete buffer1Sided;
        		delete buffer2Sided;
	            delete qp;
            };

            std::thread t(func);
            client_threads.push_back(std::move(t));
        }
		for(auto& t: client_threads){
            t.join();
        }
	}

	delete qpFactory;
	delete context;

	return 0;

}
