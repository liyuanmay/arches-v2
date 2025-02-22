#pragma once 
#include "../../stdafx.hpp"

#include "../unit-base.hpp"
#include "../unit-memory-base.hpp"
#include "unit-stream-scheduler.hpp"

namespace Arches { namespace Units { namespace DualStreaming {


struct RayBucketBuffer
{
	union
	{
		RayBucket ray_bucket;
		uint8_t data_u8[RAY_BUCKET_SIZE];
	};

	bool requested{false};
	uint bytes_returned{0};
	uint next_ray{0};

	RayBucketBuffer() 
	{
		ray_bucket.num_rays = 0;
	}
};

class UnitRayStagingBuffer : public UnitMemoryBase
{
public:
	UnitRayStagingBuffer(uint num_tp, uint tm_index, UnitStreamScheduler* stream_scheduler) : UnitMemoryBase(),
		_request_network(num_tp, 1), _return_network(num_tp), num_tp(num_tp), tm_index(tm_index), _stream_scheduler(stream_scheduler)
	{
		front_buffer = &ray_buffer[0];
		back_buffer = &ray_buffer[1];
		segment_executing_on_tp.resize(num_tp, ~0u);
	}

private:
	Casscade<MemoryRequest> _request_network;
	FIFOArray<MemoryReturn> _return_network;

	UnitStreamScheduler* _stream_scheduler;

	uint tm_index;
	uint num_tp;
	uint rgs_complete{0};

	RayBucketBuffer* front_buffer;
	RayBucketBuffer* back_buffer;
	RayBucketBuffer ray_buffer[2];

	struct SegmentState
	{
		uint active_rays{0};
		uint active_buckets{0};
	};

	std::map<uint, SegmentState> segment_state_map;
	std::vector<uint>            segment_executing_on_tp;

	std::queue<uint> completed_buckets;
	std::queue<uint> workitem_request_queue;

	bool request_valid{false};
	MemoryRequest request{};

	void clock_rise() override
	{
		_request_network.clock();

		if(!request_valid && _request_network.is_read_valid(0))
		{
			request = _request_network.read(0);
			request_valid = true;
		}

		if(_stream_scheduler->return_port_read_valid(tm_index))
		{
			MemoryReturn ret = _stream_scheduler->read_return(tm_index);
			if(ret.size == 0)
			{
				//termination condition
				back_buffer->bytes_returned = RAY_BUCKET_SIZE;
				back_buffer->ray_bucket.segment = ~0u;
				back_buffer->ray_bucket.num_rays = num_tp;
			}
			else
			{
				uint buffer_address = ret.paddr % RAY_BUCKET_SIZE;
				std::memcpy(&back_buffer->data_u8[buffer_address], ret.data, ret.size);
				back_buffer->bytes_returned += ret.size;
			}
		}
	}

	void issue_requests()
	{
		//back buffer is full and front is empty swap buffers
		if(back_buffer->bytes_returned == RAY_BUCKET_SIZE && front_buffer->next_ray >= front_buffer->ray_bucket.num_rays)
		{
			std::swap(front_buffer, back_buffer);

			//reset back buffer to empty state
			back_buffer->bytes_returned = 0;
			back_buffer->requested = false;
		}

		if(_stream_scheduler->request_port_write_valid(tm_index))
		{
			if(!back_buffer->requested && back_buffer->next_ray == back_buffer->ray_bucket.num_rays)
			{
				//back buffer is drained request a fill from the stream scheduler
				StreamSchedulerRequest req;
				req.type = StreamSchedulerRequest::Type::LOAD_BUCKET;
				req.port = tm_index;
				_stream_scheduler->write_request(req, req.port);

				//reset ray return state as completed
				back_buffer->next_ray = 0;

				back_buffer->requested = true;
			}
			else if(request_valid && request.type == MemoryRequest::Type::STORE)
			{
				//a store is pending forward to stream scheduler
				StreamSchedulerRequest req;
				req.type = StreamSchedulerRequest::Type::STORE_WORKITEM;
				req.port = tm_index;

				WorkItem wi;
				std::memcpy(&wi, request.data, request.size);
				req.bray = wi.bray;
				req.segment = wi.segment;

				_stream_scheduler->write_request(req, req.port);
				request_valid = false;
			}
			else if(rgs_complete == num_tp && _stream_scheduler->request_port_write_valid(tm_index))
			{
				//ray generation complete
				StreamSchedulerRequest req;
				req.type = StreamSchedulerRequest::Type::BUCKET_COMPLETE;
				req.port = tm_index;
				req.segment = 0;
				_stream_scheduler->write_request(req, req.port);

				rgs_complete = ~0u;
			}
			else if(!completed_buckets.empty() && _stream_scheduler->request_port_write_valid(tm_index))
			{
				//a bucket completed
				StreamSchedulerRequest req;
				req.type = StreamSchedulerRequest::Type::BUCKET_COMPLETE;
				req.port = tm_index;
				req.segment = completed_buckets.front();
				_stream_scheduler->write_request(req, req.port);

				completed_buckets.pop();
				return;
			}
		}
	}

	void issue_returns()
	{
		//a load request is pending put it in the request queue
		if(request_valid && request.type == MemoryRequest::Type::LOAD)
		{
			workitem_request_queue.push(request.port);
			request_valid = false;

			//mark previous ray as complete
			if(segment_executing_on_tp[request.port] != ~0u)
			{
				uint segment_index = segment_executing_on_tp[request.port];
				SegmentState& segment_state = segment_state_map[segment_index];

				segment_state.active_rays--;
				if(segment_state.active_rays == 0)
				{
					for(uint i = 0; i < segment_state.active_buckets; ++i)
						completed_buckets.push(segment_index);

					segment_state_map.erase(segment_index);
				}
			}
			else
			{
				rgs_complete++;
			}
		}

		//if we have a filled bucket pop a ray from it
		if(!workitem_request_queue.empty() && front_buffer->next_ray < front_buffer->ray_bucket.num_rays && _return_network.is_write_valid(request.port))
		{
			MemoryReturn ret;

			ISA::RISCV::RegAddr reg_addr;
			reg_addr.reg = 0;
			reg_addr.reg_type = ISA::RISCV::RegType::FLOAT;
			reg_addr.sign_ext = 0;
			ret.dst = reg_addr.u8;

			ret.size = sizeof(WorkItem);
			ret.port = workitem_request_queue.front();

			WorkItem wi;
			wi.bray = front_buffer->ray_bucket.bucket_rays[front_buffer->next_ray];
			wi.segment = front_buffer->ray_bucket.segment;
			std::memcpy(ret.data, &wi, ret.size);
			_return_network.write(ret, ret.port);

			if(front_buffer->next_ray == 0)
			{
				segment_state_map[wi.segment].active_buckets++;
				segment_state_map[wi.segment].active_rays += front_buffer->ray_bucket.num_rays;
			}
			segment_executing_on_tp[ret.port] = wi.segment;

			workitem_request_queue.pop();
			front_buffer->next_ray++;
		}
	}

	void clock_fall() override
	{
		issue_requests();
		issue_returns();
		_return_network.clock();
	}

	bool request_port_write_valid(uint port_index) override
	{
		return _request_network.is_write_valid(port_index);
	}

	void write_request(const MemoryRequest& request, uint port_index) override
	{
		_request_network.write(request, port_index);
	}

	bool return_port_read_valid(uint port_index) override
	{
		return _return_network.is_read_valid(port_index);
	}

	const MemoryReturn& peek_return(uint port_index) override
	{
		return _return_network.peek(port_index);
	}

	const MemoryReturn read_return(uint port_index) override
	{
		return _return_network.read(port_index);
	}
};

}}}