/**
 * Core - Context
 *
 * (c) 2018 Claude Barthels, ETH Zurich
 * Contact: claudeb@inf.ethz.ch
 *
 */

#include "Context.h"

#include <string.h>
#include <limits>
#include <arpa/inet.h>

#include <infinity/core/Configuration.h>
#include <infinity/queues/QueuePair.h>
#include <infinity/memory/Atomic.h>
#include <infinity/memory/Buffer.h>
#include <infinity/requests/RequestToken.h>
#include <infinity/utils/Debug.h>

#define MAX(a,b) ((a) > (b) ? (a) : (b))

namespace infinity {
namespace core {

/*******************************
 * Context
 ******************************/

Context::Context(uint16_t device, uint16_t devicePort) {

	// Get IB device list
	int32_t numberOfInstalledDevices = 0;
	ibv_device **ibvDeviceList = ibv_get_device_list(&numberOfInstalledDevices);
	INFINITY_ASSERT(numberOfInstalledDevices > 0, "[INFINITY][CORE][CONTEXT] No InfiniBand devices found.\n");
	INFINITY_ASSERT(device < numberOfInstalledDevices, "[INFINITY][CORE][CONTEXT] Requested device %d not found. There are %d devices available.\n",
			device, numberOfInstalledDevices);
	INFINITY_ASSERT(ibvDeviceList != NULL, "[INFINITY][CORE][CONTEXT] Device list was NULL.\n");

	// Get IB device
	this->ibvDevice = ibvDeviceList[device];
	INFINITY_ASSERT(this->ibvDevice != NULL, "[INFINITY][CORE][CONTEXT] Requested device %d was NULL.\n", device);

	// Open IB device and allocate protection domain
	this->ibvContext = ibv_open_device(this->ibvDevice);
	INFINITY_ASSERT(this->ibvContext != NULL, "[INFINITY][CORE][CONTEXT] Could not open device %d.\n", device);
	this->ibvProtectionDomain = ibv_alloc_pd(this->ibvContext);
	INFINITY_ASSERT(this->ibvProtectionDomain != NULL, "[INFINITY][CORE][CONTEXT] Could not allocate protection domain.\n");

	// Get the LID
	ibv_port_attr portAttributes;
	ibv_query_port(this->ibvContext, devicePort, &portAttributes);
	this->ibvLocalDeviceId = portAttributes.lid;
	this->ibvDevicePort = devicePort;

	defaultAtomic = new infinity::memory::Atomic(this);

}

Context::~Context() {

	delete defaultAtomic;

	// Destroy protection domain
	int returnValue = ibv_dealloc_pd(this->ibvProtectionDomain);
	INFINITY_ASSERT(returnValue == 0, "[INFINITY][CORE][CONTEXT] Could not delete protection domain\n");

	// Close device
	returnValue = ibv_close_device(this->ibvContext);
	INFINITY_ASSERT(returnValue == 0, "[INFINITY][CORE][CONTEXT] Could not close device\n");

}

void Context::registerQueuePair(infinity::queues::QueuePair* queuePair) {
	this->queuePairMap.insert({queuePair->getQueuePairNumber(), queuePair});
}

ibv_context* Context::getInfiniBandContext() {
	return this->ibvContext;
}

uint16_t Context::getLocalDeviceId() {
	return this->ibvLocalDeviceId;
}

uint16_t Context::getDevicePort() {
	return this->ibvDevicePort;
}

ibv_pd* Context::getProtectionDomain() {
	return this->ibvProtectionDomain;
}

} /* namespace core */
} /* namespace infinity */
