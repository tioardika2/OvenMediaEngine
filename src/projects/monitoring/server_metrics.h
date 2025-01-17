//
// Created by getroot on 21. 7. 13.
//

#pragma once

#include <base/ovlibrary/ovlibrary.h>
#include "base/info/host.h"
#include "base/info/managed_queue.h"
#include "host_metrics.h"
#include "queue_metrics.h"

namespace mon
{
	class ServerMetrics : public CommonMetrics
	{
	public:
		ServerMetrics(const std::shared_ptr<const cfg::Server> &server_config);
		void Release();
		void ShowInfo();

		bool OnHostCreated(const info::Host &host_info);
		bool OnHostDeleted(const info::Host &host_info);
		
		std::shared_ptr<const cfg::Server> GetConfig();
		std::chrono::system_clock::time_point GetServerStartedTime();
		std::map<uint32_t, std::shared_ptr<HostMetrics>> GetHostMetricsList();
        std::shared_ptr<HostMetrics> GetHostMetrics(const info::Host &host_info);

	protected:
		std::shared_ptr<const cfg::Server> _server_config = nullptr;
		std::chrono::system_clock::time_point _server_started_time;
		std::shared_mutex _map_guard;
		std::map<uint32_t, std::shared_ptr<HostMetrics>> _hosts;

	// Queue metrics
	public:
		void OnQueueCreated(const info::ManagedQueue& info);
		void OnQueueDeleted(const info::ManagedQueue& info);
		void OnQueueUpdated(const info::ManagedQueue& info, bool with_metadata = false);
		std::map<uint32_t, std::shared_ptr<QueueMetrics>> GetQueueMetricsList();

	protected:
		std::map<uint32_t, std::shared_ptr<QueueMetrics>> _queues;
	};
}