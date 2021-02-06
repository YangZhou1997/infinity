/**
 * Queues - Queue Pair
 *
 * (c) 2018 Claude Barthels, ETH Zurich
 * Contact: claudeb@inf.ethz.ch
 *
 */

#include "QueuePair.h"

#include <random>
#include <string.h>
#include <arpa/inet.h>
#include <cerrno>

#include <infinity/core/Configuration.h>
#include <infinity/utils/Debug.h>

#define MAX(a,b) ((a) > (b) ? (a) : (b))

namespace infinity {
namespace queues {

int OperationFlags::ibvFlags() {
  int flags = 0;
  if (fenced) {
    flags |= IBV_SEND_FENCE;
  }
  if (signaled) {
    flags |= IBV_SEND_SIGNALED;
  }
  if (inlined) {
    flags |= IBV_SEND_INLINE;
  }
  return flags;
}

QueuePair::QueuePair(infinity::core::Context* context) :
		context(context) {

	ibv_qp_init_attr qpInitAttributes;
	memset(&qpInitAttributes, 0, sizeof(qpInitAttributes));


	// Allocate completion queues
	this->ibvSendCompletionQueue = ibv_create_cq(context->getInfiniBandContext(), MAX(infinity::core::Configuration::SEND_COMPLETION_QUEUE_LENGTH, 1), NULL, NULL, 0);
	INFINITY_ASSERT(this->ibvSendCompletionQueue != NULL, "[INFINITY][QUEUES][QUEUEPAIR] Cannot create send completion queue.\n");
	this->ibvReceiveCompletionQueue = ibv_create_cq(context->getInfiniBandContext(), MAX(infinity::core::Configuration::RECV_COMPLETION_QUEUE_LENGTH, 1), NULL, NULL, 0);
	INFINITY_ASSERT(this->ibvReceiveCompletionQueue != NULL, "[INFINITY][QUEUES][QUEUEPAIR] Cannot create receive completion queue.\n");


	qpInitAttributes.send_cq = this->ibvSendCompletionQueue;
	qpInitAttributes.recv_cq = this->ibvReceiveCompletionQueue;
	qpInitAttributes.cap.max_send_wr = MAX(infinity::core::Configuration::SEND_COMPLETION_QUEUE_LENGTH, 1);
	qpInitAttributes.cap.max_send_sge = infinity::core::Configuration::MAX_NUMBER_OF_SGE_ELEMENTS;
	qpInitAttributes.cap.max_recv_wr = MAX(infinity::core::Configuration::RECV_COMPLETION_QUEUE_LENGTH, 1);
	qpInitAttributes.cap.max_recv_sge = infinity::core::Configuration::MAX_NUMBER_OF_SGE_ELEMENTS;
	qpInitAttributes.qp_type = IBV_QPT_RC;
	qpInitAttributes.sq_sig_all = 0;

	this->ibvQueuePair = ibv_create_qp(context->getProtectionDomain(), &(qpInitAttributes));
	INFINITY_ASSERT(this->ibvQueuePair != NULL, "[INFINITY][QUEUES][QUEUEPAIR] Cannot create queue pair.\n");

	ibv_qp_attr qpAttributes;
	memset(&qpAttributes, 0, sizeof(qpAttributes));

	qpAttributes.qp_state = IBV_QPS_INIT;
	qpAttributes.pkey_index = 0;
	qpAttributes.port_num = context->getDevicePort();
	qpAttributes.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

	int32_t returnValue = ibv_modify_qp(this->ibvQueuePair, &(qpAttributes), IBV_QP_STATE | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS | IBV_QP_PKEY_INDEX);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Cannot transition to INIT state.\n");

	std::random_device randomGenerator;
        std::uniform_int_distribution<int> range(0, 1<<24);
        this->sequenceNumber = range(randomGenerator);

	this->userData = NULL;
	this->userDataSize = 0;

    // Create a default request token
	defaultRequestToken = new infinity::requests::RequestToken(this);

}

QueuePair::~QueuePair() {
	// Destroy completion queues
	int returnValue = ibv_destroy_cq(this->ibvSendCompletionQueue);
	INFINITY_ASSERT(returnValue == 0, "[INFINITY][CORE][CONTEXT] Could not delete send completion queue\n");
	returnValue = ibv_destroy_cq(this->ibvReceiveCompletionQueue);
	INFINITY_ASSERT(returnValue == 0, "[INFINITY][CORE][CONTEXT] Could not delete receive completion queue\n");

	returnValue = ibv_destroy_qp(this->ibvQueuePair);
	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Cannot delete queue pair.\n");

	if (this->userData != NULL && this->userDataSize != 0) {
		free(this->userData);
		this->userDataSize = 0;
	}

	// Delete default token
	delete defaultRequestToken;
}

void QueuePair::activate(uint16_t remoteDeviceId, uint32_t remoteQueuePairNumber, uint32_t remoteSequenceNumber) {

	ibv_qp_attr qpAttributes;
	memset(&(qpAttributes), 0, sizeof(qpAttributes));

	qpAttributes.qp_state = IBV_QPS_RTR;
	qpAttributes.path_mtu = IBV_MTU_4096;
	qpAttributes.dest_qp_num = remoteQueuePairNumber;
	qpAttributes.rq_psn = remoteSequenceNumber;
	qpAttributes.max_dest_rd_atomic = 1;
	qpAttributes.min_rnr_timer = 12;
	qpAttributes.ah_attr.is_global = 0;
	qpAttributes.ah_attr.dlid = remoteDeviceId;
	qpAttributes.ah_attr.sl = 0;
	qpAttributes.ah_attr.src_path_bits = 0;
	qpAttributes.ah_attr.port_num = context->getDevicePort();

	int32_t returnValue = ibv_modify_qp(this->ibvQueuePair, &qpAttributes,
			IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MIN_RNR_TIMER | IBV_QP_MAX_DEST_RD_ATOMIC);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Cannot transition to RTR state.\n");

	qpAttributes.qp_state = IBV_QPS_RTS;
	qpAttributes.timeout = 14;
	qpAttributes.retry_cnt = 7;
	qpAttributes.rnr_retry = 7;
	qpAttributes.sq_psn = this->getSequenceNumber();
	qpAttributes.max_rd_atomic = 1;

	returnValue = ibv_modify_qp(this->ibvQueuePair, &qpAttributes,
			IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Cannot transition to RTS state.\n");

}

void QueuePair::setRemoteUserData(void* userData, uint32_t userDataSize) {
	if (userDataSize > 0) {
		this->userData = new char[userDataSize];
		memcpy(this->userData, userData, userDataSize);
		this->userDataSize = userDataSize;
	}
}

uint16_t QueuePair::getLocalDeviceId() {
	return this->context->getLocalDeviceId();
}

uint32_t QueuePair::getQueuePairNumber() {
	return this->ibvQueuePair->qp_num;
}

uint32_t QueuePair::getSequenceNumber() {
	return this->sequenceNumber;
}

void QueuePair::send(infinity::memory::Buffer* buffer, infinity::requests::RequestToken *requestToken) {
	send(buffer, 0, buffer->getSizeInBytes(), OperationFlags(), requestToken);
}

void QueuePair::send(infinity::memory::Buffer* buffer, uint32_t sizeInBytes, infinity::requests::RequestToken *requestToken) {
	send(buffer, 0, sizeInBytes, OperationFlags(), requestToken);
}

void QueuePair::send(infinity::memory::Buffer* buffer, uint64_t localOffset, uint32_t sizeInBytes, OperationFlags send_flags,
    infinity::requests::RequestToken *requestToken) {

	if (requestToken != NULL) {
		requestToken->reset();
		requestToken->setRegion(buffer);
	}

	struct ibv_sge sgElement;
	struct ibv_send_wr workRequest;
	struct ibv_send_wr *badWorkRequest;

	memset(&sgElement, 0, sizeof(ibv_sge));
	sgElement.addr = buffer->getAddress() + localOffset;
	sgElement.length = sizeInBytes;
	sgElement.lkey = buffer->getLocalKey();

	INFINITY_ASSERT(sizeInBytes <= buffer->getRemainingSizeInBytes(localOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while creating scatter-getter element.\n");

	memset(&workRequest, 0, sizeof(ibv_send_wr));
	workRequest.wr_id = reinterpret_cast<uint64_t>(requestToken);
	workRequest.sg_list = &sgElement;
	workRequest.num_sge = 1;
	workRequest.opcode = IBV_WR_SEND;
	workRequest.send_flags = send_flags.ibvFlags();
	if (requestToken != NULL) {
		workRequest.send_flags |= IBV_SEND_SIGNALED;
	}

	int returnValue = ibv_post_send(this->ibvQueuePair, &workRequest, &badWorkRequest);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Posting send request failed. %s.\n", strerror(errno));

	INFINITY_DEBUG("[INFINITY][QUEUES][QUEUEPAIR] Send request created (id %lu).\n", workRequest.wr_id);

}

void QueuePair::sendWithImmediate(infinity::memory::Buffer* buffer, uint64_t localOffset, uint32_t sizeInBytes, uint32_t immediateValue,
    OperationFlags send_flags, infinity::requests::RequestToken* requestToken) {

	if (requestToken != NULL) {
		requestToken->reset();
		requestToken->setRegion(buffer);
		requestToken->setImmediateValue(immediateValue);
	}

	struct ibv_sge sgElement;
	struct ibv_send_wr workRequest;
	struct ibv_send_wr *badWorkRequest;

	memset(&sgElement, 0, sizeof(ibv_sge));
	sgElement.addr = buffer->getAddress() + localOffset;
	sgElement.length = sizeInBytes;
	sgElement.lkey = buffer->getLocalKey();

	INFINITY_ASSERT(sizeInBytes <= buffer->getRemainingSizeInBytes(localOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while creating scatter-getter element.\n");

	memset(&workRequest, 0, sizeof(ibv_send_wr));
	workRequest.wr_id = reinterpret_cast<uint64_t>(requestToken);
	workRequest.sg_list = &sgElement;
	workRequest.num_sge = 1;
	workRequest.opcode = IBV_WR_SEND_WITH_IMM;
	workRequest.imm_data = htonl(immediateValue);
	workRequest.send_flags = send_flags.ibvFlags();
	if (requestToken != NULL) {
		workRequest.send_flags |= IBV_SEND_SIGNALED;
	}

	int returnValue = ibv_post_send(this->ibvQueuePair, &workRequest, &badWorkRequest);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Posting send request failed. %s.\n", strerror(errno));

	INFINITY_DEBUG("[INFINITY][QUEUES][QUEUEPAIR] Send request created (id %lu).\n", workRequest.wr_id);

}

void QueuePair::postReceiveBuffer(infinity::memory::Buffer* buffer) {

	INFINITY_ASSERT(buffer->getSizeInBytes() <= std::numeric_limits<uint32_t>::max(),
			"[INFINITY][CORE][CONTEXT] Cannot post receive buffer which is larger than max(uint32_t).\n");

	// Create scatter-getter
	ibv_sge isge;
	memset(&isge, 0, sizeof(ibv_sge));
	isge.addr = buffer->getAddress();
	isge.length = static_cast<uint32_t>(buffer->getSizeInBytes());
	isge.lkey = buffer->getLocalKey();

	// Create work request
	ibv_recv_wr wr;
	memset(&wr, 0, sizeof(ibv_recv_wr));
	wr.wr_id = reinterpret_cast<uint64_t>(buffer);
	wr.next = NULL;
	wr.sg_list = &isge;
	wr.num_sge = 1;

	// Post buffer to shared receive queue
	ibv_recv_wr *badwr;
	uint32_t returnValue = ibv_post_recv(this->ibvQueuePair, &wr, &badwr);
	INFINITY_ASSERT(returnValue == 0, "[INFINITY][CORE][CONTEXT] Cannot post buffer to receive queue.\n");

}

bool QueuePair::receive(receive_element_t* receiveElement) {

	return receive(&(receiveElement->buffer), &(receiveElement->bytesWritten), &(receiveElement->immediateValue), &(receiveElement->immediateValueValid), &(receiveElement->queuePair));

}

bool QueuePair::receive(infinity::memory::Buffer** buffer, uint32_t *bytesWritten, uint32_t *immediateValue, bool *immediateValueValid, infinity::queues::QueuePair **queuePair) {

	ibv_wc wc;
	if (ibv_poll_cq(this->ibvReceiveCompletionQueue, 1, &wc) > 0) {

		if(wc.opcode == IBV_WC_RECV) {
			*(buffer) = reinterpret_cast<infinity::memory::Buffer*>(wc.wr_id);
			*(bytesWritten) = wc.byte_len;
		} else if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
			*(buffer) = NULL;
			*(bytesWritten) = wc.byte_len;
			infinity::memory::Buffer* receiveBuffer = reinterpret_cast<infinity::memory::Buffer*>(wc.wr_id);
			this->postReceiveBuffer(receiveBuffer);
		}

		if(wc.wc_flags & IBV_WC_WITH_IMM) {
			*(immediateValue) = ntohl(wc.imm_data);
			*(immediateValueValid) = true;
		} else {
			*(immediateValue) = 0;
			*(immediateValueValid) = false;
		}

		if(queuePair != NULL) {
			*(queuePair) = this->context->queuePairMap.at(wc.qp_num);
		}

		return true;
	}

	return false;

}

bool QueuePair::pollSendCompletionQueue() {

	ibv_wc wc;
	if (ibv_poll_cq(this->ibvSendCompletionQueue, 1, &wc) > 0) {

		infinity::requests::RequestToken * request = reinterpret_cast<infinity::requests::RequestToken*>(wc.wr_id);
		if (request != NULL) {
			request->setCompleted(wc.status == IBV_WC_SUCCESS);
		}

		if (wc.status == IBV_WC_SUCCESS) {
			INFINITY_DEBUG("[INFINITY][CORE][CONTEXT] Request completed (id %lu).\n", wc.wr_id);
		} else {
			INFINITY_DEBUG("[INFINITY][CORE][CONTEXT] Request failed (id %lu).\n", wc.wr_id);
		}
		return true;
	}

	return false;

}

void QueuePair::write(infinity::memory::Buffer* buffer, infinity::memory::RegionToken* destination, infinity::requests::RequestToken *requestToken) {
	write(buffer, 0, destination, 0, buffer->getSizeInBytes(), OperationFlags(), requestToken);
	INFINITY_ASSERT(buffer->getSizeInBytes() <= ((uint64_t) UINT32_MAX), "[INFINITY][QUEUES][QUEUEPAIR] Request must be smaller or equal to UINT_32_MAX bytes. This memory region is larger. Please explicitly indicate the size of the data to transfer.\n");
}

void QueuePair::write(infinity::memory::Buffer* buffer, infinity::memory::RegionToken* destination, uint32_t sizeInBytes,
		infinity::requests::RequestToken *requestToken) {
	write(buffer, 0, destination, 0, sizeInBytes, OperationFlags(), requestToken);
}

void QueuePair::write(infinity::memory::Buffer* buffer, uint64_t localOffset, infinity::memory::RegionToken* destination, uint64_t remoteOffset,
		uint32_t sizeInBytes, OperationFlags send_flags, infinity::requests::RequestToken *requestToken) {

	if (requestToken != NULL) {
		requestToken->reset();
		requestToken->setRegion(buffer);
	}

	struct ibv_sge sgElement;
	struct ibv_send_wr workRequest;
	struct ibv_send_wr *badWorkRequest;

	memset(&sgElement, 0, sizeof(ibv_sge));
	sgElement.addr = buffer->getAddress() + localOffset;
	sgElement.length = sizeInBytes;
	sgElement.lkey = buffer->getLocalKey();

	INFINITY_ASSERT(sizeInBytes <= buffer->getRemainingSizeInBytes(localOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while creating scatter-getter element.\n");

	memset(&workRequest, 0, sizeof(ibv_send_wr));
	workRequest.wr_id = reinterpret_cast<uint64_t>(requestToken);
	workRequest.sg_list = &sgElement;
	workRequest.num_sge = 1;
	workRequest.opcode = IBV_WR_RDMA_WRITE;
	workRequest.send_flags = send_flags.ibvFlags();
	if (requestToken != NULL) {
		workRequest.send_flags |= IBV_SEND_SIGNALED;
	}
	workRequest.wr.rdma.remote_addr = destination->getAddress() + remoteOffset;
	workRequest.wr.rdma.rkey = destination->getRemoteKey();

	INFINITY_ASSERT(sizeInBytes <= destination->getRemainingSizeInBytes(remoteOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while writing to remote memory.\n");

	int returnValue = ibv_post_send(this->ibvQueuePair, &workRequest, &badWorkRequest);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Posting write request failed. %s.\n", strerror(errno));

	INFINITY_DEBUG("[INFINITY][QUEUES][QUEUEPAIR] Write request created (id %lu).\n", workRequest.wr_id);

}

void QueuePair::writeWithImmediate(infinity::memory::Buffer* buffer, uint64_t localOffset, infinity::memory::RegionToken* destination, uint64_t remoteOffset,
		uint32_t sizeInBytes, uint32_t immediateValue, OperationFlags send_flags, infinity::requests::RequestToken* requestToken) {

	if (requestToken != NULL) {
		requestToken->reset();
		requestToken->setRegion(buffer);
		requestToken->setImmediateValue(immediateValue);
	}

	struct ibv_sge sgElement;
	struct ibv_send_wr workRequest;
	struct ibv_send_wr *badWorkRequest;

	memset(&sgElement, 0, sizeof(ibv_sge));
	sgElement.addr = buffer->getAddress() + localOffset;
	sgElement.length = sizeInBytes;
	sgElement.lkey = buffer->getLocalKey();

	INFINITY_ASSERT(sizeInBytes <= buffer->getRemainingSizeInBytes(localOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while creating scatter-getter element.\n");

	memset(&workRequest, 0, sizeof(ibv_send_wr));
	workRequest.wr_id = reinterpret_cast<uint64_t>(requestToken);
	workRequest.sg_list = &sgElement;
	workRequest.num_sge = 1;
	workRequest.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	workRequest.imm_data = htonl(immediateValue);
	workRequest.send_flags = send_flags.ibvFlags();
	if (requestToken != NULL) {
		workRequest.send_flags |= IBV_SEND_SIGNALED;
	}
	workRequest.wr.rdma.remote_addr = destination->getAddress() + remoteOffset;
	workRequest.wr.rdma.rkey = destination->getRemoteKey();

	INFINITY_ASSERT(sizeInBytes <= destination->getRemainingSizeInBytes(remoteOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while writing to remote memory.\n");

	int returnValue = ibv_post_send(this->ibvQueuePair, &workRequest, &badWorkRequest);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Posting write request failed. %s.\n", strerror(errno));

	INFINITY_DEBUG("[INFINITY][QUEUES][QUEUEPAIR] Write request created (id %lu).\n", workRequest.wr_id);

}

void QueuePair::multiWrite(infinity::memory::Buffer** buffers, uint32_t* sizesInBytes, uint64_t* localOffsets, uint32_t numberOfElements,
		infinity::memory::RegionToken* destination, uint64_t remoteOffset, OperationFlags send_flags, infinity::requests::RequestToken* requestToken) {

	if (requestToken != NULL) {
		requestToken->reset();
		requestToken->setRegion(buffers[0]);
	}

	struct ibv_sge *sgElements = (ibv_sge *) calloc(numberOfElements, sizeof(ibv_sge));
	struct ibv_send_wr workRequest;
	struct ibv_send_wr *badWorkRequest;

	INFINITY_ASSERT(numberOfElements <= infinity::core::Configuration::MAX_NUMBER_OF_SGE_ELEMENTS, "[INFINITY][QUEUES][QUEUEPAIR] Request contains too many SGE.\n");

	uint32_t totalSizeInBytes = 0;
	for (uint32_t i = 0; i < numberOfElements; ++i) {
		if (localOffsets != NULL) {
			sgElements[i].addr = buffers[i]->getAddress() + localOffsets[i];
		} else {
			sgElements[i].addr = buffers[i]->getAddress();
		}
		if (sizesInBytes != NULL) {
			sgElements[i].length = sizesInBytes[i];
		} else {
			sgElements[i].length = buffers[i]->getSizeInBytes();
		}
		totalSizeInBytes += sgElements[i].length;
		sgElements[i].lkey = buffers[i]->getLocalKey();
	}

	memset(&workRequest, 0, sizeof(ibv_send_wr));
	workRequest.wr_id = reinterpret_cast<uint64_t>(requestToken);
	workRequest.sg_list = sgElements;
	workRequest.num_sge = numberOfElements;
	workRequest.opcode = IBV_WR_RDMA_WRITE;
	workRequest.send_flags = send_flags.ibvFlags();
	if (requestToken != NULL) {
		workRequest.send_flags |= IBV_SEND_SIGNALED;
	}
	workRequest.wr.rdma.remote_addr = destination->getAddress() + remoteOffset;
	workRequest.wr.rdma.rkey = destination->getRemoteKey();

	INFINITY_ASSERT(totalSizeInBytes <= destination->getRemainingSizeInBytes(remoteOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while writing to remote memory.\n");

	int returnValue = ibv_post_send(this->ibvQueuePair, &workRequest, &badWorkRequest);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Posting write request failed. %s.\n", strerror(errno));

	INFINITY_DEBUG("[INFINITY][QUEUES][QUEUEPAIR] Multi-Write request created (id %lu).\n", workRequest.wr_id);
}

void QueuePair::multiWriteWithImmediate(infinity::memory::Buffer** buffers, uint32_t* sizesInBytes, uint64_t* localOffsets, uint32_t numberOfElements,
		infinity::memory::RegionToken* destination, uint64_t remoteOffset, uint32_t immediateValue, OperationFlags send_flags, infinity::requests::RequestToken* requestToken) {

	if (requestToken != NULL) {
		requestToken->reset();
		requestToken->setRegion(buffers[0]);
		requestToken->setImmediateValue(immediateValue);
	}

	struct ibv_sge *sgElements = (ibv_sge *) calloc(numberOfElements, sizeof(ibv_sge));
	struct ibv_send_wr workRequest;
	struct ibv_send_wr *badWorkRequest;

	INFINITY_ASSERT(numberOfElements <= infinity::core::Configuration::MAX_NUMBER_OF_SGE_ELEMENTS, "[INFINITY][QUEUES][QUEUEPAIR] Request contains too many SGE.\n");

	uint32_t totalSizeInBytes = 0;
	for (uint32_t i = 0; i < numberOfElements; ++i) {
		if (localOffsets != NULL) {
			sgElements[i].addr = buffers[i]->getAddress() + localOffsets[i];
		} else {
			sgElements[i].addr = buffers[i]->getAddress();
		}
		if (sizesInBytes != NULL) {
			sgElements[i].length = sizesInBytes[i];
		} else {
			sgElements[i].length = buffers[i]->getSizeInBytes();
		}
		totalSizeInBytes += sgElements[i].length;
		sgElements[i].lkey = buffers[i]->getLocalKey();
	}

	memset(&workRequest, 0, sizeof(ibv_send_wr));
	workRequest.wr_id = reinterpret_cast<uint64_t>(requestToken);
	workRequest.sg_list = sgElements;
	workRequest.num_sge = numberOfElements;
	workRequest.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	workRequest.imm_data = htonl(immediateValue);
	workRequest.send_flags = send_flags.ibvFlags();
	if (requestToken != NULL) {
		workRequest.send_flags |= IBV_SEND_SIGNALED;
	}
	workRequest.wr.rdma.remote_addr = destination->getAddress() + remoteOffset;
	workRequest.wr.rdma.rkey = destination->getRemoteKey();

	INFINITY_ASSERT(totalSizeInBytes <= destination->getRemainingSizeInBytes(remoteOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while writing to remote memory.\n");

	int returnValue = ibv_post_send(this->ibvQueuePair, &workRequest, &badWorkRequest);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Posting write request failed. %s.\n", strerror(errno));

	INFINITY_DEBUG("[INFINITY][QUEUES][QUEUEPAIR] Multi-Write request created (id %lu).\n", workRequest.wr_id);

}

void QueuePair::read(infinity::memory::Buffer* buffer, infinity::memory::RegionToken* source, infinity::requests::RequestToken *requestToken) {
	read(buffer, 0, source, 0, buffer->getSizeInBytes(), OperationFlags(), requestToken);
	INFINITY_ASSERT(buffer->getSizeInBytes() <= ((uint64_t) UINT32_MAX), "[INFINITY][QUEUES][QUEUEPAIR] Request must be smaller or equal to UINT_32_MAX bytes. This memory region is larger. Please explicitly indicate the size of the data to transfer.\n");
}

void QueuePair::read(infinity::memory::Buffer* buffer, infinity::memory::RegionToken* source, uint32_t sizeInBytes,
		infinity::requests::RequestToken *requestToken) {
	read(buffer, 0, source, 0, sizeInBytes, OperationFlags(), requestToken);
}

void QueuePair::read(infinity::memory::Buffer* buffer, uint64_t localOffset, infinity::memory::RegionToken* source, uint64_t remoteOffset, uint32_t sizeInBytes,
		OperationFlags send_flags, infinity::requests::RequestToken *requestToken) {

	if (requestToken != NULL) {
		requestToken->reset();
		requestToken->setRegion(buffer);
	}

	struct ibv_sge sgElement;
	struct ibv_send_wr workRequest;
	struct ibv_send_wr *badWorkRequest;

	memset(&sgElement, 0, sizeof(ibv_sge));
	sgElement.addr = buffer->getAddress() + localOffset;
	sgElement.length = sizeInBytes;
	sgElement.lkey = buffer->getLocalKey();

	INFINITY_ASSERT(sizeInBytes <= buffer->getRemainingSizeInBytes(localOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while creating scatter-getter element.\n");

	memset(&workRequest, 0, sizeof(ibv_send_wr));
	workRequest.wr_id = reinterpret_cast<uint64_t>(requestToken);
	workRequest.sg_list = &sgElement;
	workRequest.num_sge = 1;
	workRequest.opcode = IBV_WR_RDMA_READ;
	workRequest.send_flags = send_flags.ibvFlags();
	if (requestToken != NULL) {
		workRequest.send_flags |= IBV_SEND_SIGNALED;
	}
	workRequest.wr.rdma.remote_addr = source->getAddress() + remoteOffset;
	workRequest.wr.rdma.rkey = source->getRemoteKey();

	INFINITY_ASSERT(sizeInBytes <= source->getRemainingSizeInBytes(remoteOffset),
			"[INFINITY][QUEUES][QUEUEPAIR] Segmentation fault while reading from remote memory.\n");

	int returnValue = ibv_post_send(this->ibvQueuePair, &workRequest, &badWorkRequest);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Posting read request failed. %s.\n", strerror(errno));

	INFINITY_DEBUG("[INFINITY][QUEUES][QUEUEPAIR] Read request created (id %lu).\n", workRequest.wr_id);

}

void QueuePair::compareAndSwap(infinity::memory::RegionToken* destination, infinity::memory::Atomic* previousValue, uint64_t compare, uint64_t swap,
		OperationFlags send_flags, infinity::requests::RequestToken *requestToken) {

	if (requestToken != NULL) {
		requestToken->reset();
		requestToken->setRegion(previousValue);
	}

	struct ibv_sge sgElement;
	struct ibv_send_wr workRequest;
	struct ibv_send_wr *badWorkRequest;

	memset(&sgElement, 0, sizeof(ibv_sge));
	sgElement.addr = previousValue->getAddress();
	sgElement.length = previousValue->getSizeInBytes();
	sgElement.lkey = previousValue->getLocalKey();

	memset(&workRequest, 0, sizeof(ibv_send_wr));
	workRequest.wr_id = reinterpret_cast<uint64_t>(requestToken);
	workRequest.sg_list = &sgElement;
	workRequest.num_sge = 1;
	workRequest.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
	workRequest.send_flags = send_flags.ibvFlags();
	if (requestToken != NULL) {
		workRequest.send_flags |= IBV_SEND_SIGNALED;
	}
	workRequest.wr.atomic.remote_addr = destination->getAddress();
	workRequest.wr.atomic.rkey = destination->getRemoteKey();
	workRequest.wr.atomic.compare_add = compare;
	workRequest.wr.atomic.swap = swap;

	int returnValue = ibv_post_send(this->ibvQueuePair, &workRequest, &badWorkRequest);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Posting cmp-and-swp request failed. %s.\n", strerror(errno));

	INFINITY_DEBUG("[INFINITY][QUEUES][QUEUEPAIR] Cmp-and-swp request created (id %lu).\n", workRequest.wr_id);
	
}

void QueuePair::compareAndSwap(infinity::memory::RegionToken* destination, uint64_t compare, uint64_t swap, infinity::requests::RequestToken *requestToken) {
	compareAndSwap(destination, context->defaultAtomic, compare, swap, OperationFlags(), requestToken);
}

void QueuePair::fetchAndAdd(infinity::memory::RegionToken* destination, uint64_t add, infinity::requests::RequestToken *requestToken) {
	fetchAndAdd(destination, context->defaultAtomic, add, OperationFlags(), requestToken);
}

void QueuePair::fetchAndAdd(infinity::memory::RegionToken* destination, infinity::memory::Atomic* previousValue, uint64_t add,
		OperationFlags send_flags, infinity::requests::RequestToken *requestToken) {

	if (requestToken != NULL) {
		requestToken->reset();
		requestToken->setRegion(previousValue);
	}

	struct ibv_sge sgElement;
	struct ibv_send_wr workRequest;
	struct ibv_send_wr *badWorkRequest;

	memset(&sgElement, 0, sizeof(ibv_sge));
	sgElement.addr = previousValue->getAddress();
	sgElement.length = previousValue->getSizeInBytes();
	sgElement.lkey = previousValue->getLocalKey();

	memset(&workRequest, 0, sizeof(ibv_send_wr));
	workRequest.wr_id = reinterpret_cast<uint64_t>(requestToken);
	workRequest.sg_list = &sgElement;
	workRequest.num_sge = 1;
	workRequest.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
	workRequest.send_flags = send_flags.ibvFlags();
	if (requestToken != NULL) {
		workRequest.send_flags |= IBV_SEND_SIGNALED;
	}
	workRequest.wr.atomic.remote_addr = destination->getAddress();
	workRequest.wr.atomic.rkey = destination->getRemoteKey();
	workRequest.wr.atomic.compare_add = add;

	int returnValue = ibv_post_send(this->ibvQueuePair, &workRequest, &badWorkRequest);

	INFINITY_ASSERT(returnValue == 0, "[INFINITY][QUEUES][QUEUEPAIR] Posting fetch-add request failed. %s.\n", strerror(errno));

	INFINITY_DEBUG("[INFINITY][QUEUES][QUEUEPAIR] Fetch-add request created (id %lu).\n", workRequest.wr_id);

}



bool QueuePair::hasUserData() {
	return (this->userData != NULL && this->userDataSize != 0);
}

uint32_t QueuePair::getUserDataSize() {
	return this->userDataSize;
}

void* QueuePair::getUserData() {
	return this->userData;
}

} /* namespace queues */
} /* namespace infinity */
