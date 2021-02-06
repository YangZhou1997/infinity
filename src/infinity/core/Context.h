/**
 * Core - Context
 *
 * (c) 2018 Claude Barthels, ETH Zurich
 * Contact: claudeb@inf.ethz.ch
 *
 */

#ifndef CORE_CONTEXT_H_
#define CORE_CONTEXT_H_

#include <stdlib.h>
#include <stdint.h>
#include <unordered_map>
#include <infiniband/verbs.h>

namespace infinity {
namespace memory {
class Region;
class Buffer;
class Atomic;
class RegisteredMemory;
}
}

namespace infinity {
namespace queues {
class QueuePair;
class QueuePairFactory;
}
}

namespace infinity {
namespace requests {
class RequestToken;
}
}

namespace infinity {
namespace core {

class Context {

	friend class infinity::memory::Region;
	friend class infinity::memory::Buffer;
	friend class infinity::memory::Atomic;
	friend class infinity::memory::RegisteredMemory;
	friend class infinity::queues::QueuePair;
	friend class infinity::queues::QueuePairFactory;

public:

	/**
	 * Constructors
	 */
	Context(uint16_t device = 0, uint16_t devicePort = 1);

	/**
	 * Destructor
	 */
	~Context();

public:

	infinity::memory::Atomic * defaultAtomic;

protected:

	/**
	 * Returns ibVerbs context
	 */
	ibv_context * getInfiniBandContext();

	/**
	 * Returns local device id
	 */
	uint16_t getLocalDeviceId();

	/**
	 * Returns device port
	 */
	uint16_t getDevicePort();

	/**
	 * Returns ibVerbs protection domain
	 */
	ibv_pd * getProtectionDomain();

protected:

	/**
	 * IB context and protection domain
	 */
	ibv_context *ibvContext;
	ibv_pd *ibvProtectionDomain;

	/**
	 * Local device id and port
	 */
	ibv_device *ibvDevice;
	uint16_t ibvLocalDeviceId;
	uint16_t ibvDevicePort;

protected:

	void registerQueuePair(infinity::queues::QueuePair *queuePair);
	std::unordered_map<uint32_t, infinity::queues::QueuePair *> queuePairMap;

};

} /* namespace core */
} /* namespace infinity */

#endif /* CORE_CONTEXT_H_ */
