#include "include/network_session.h"
#include <iostream>

#include <mutex>
#include <thread>

class stress_client : public network_session_handler
{
public:
	stress_client()
	{
		memset(&remote, 0, sizeof(remote));
	}

	virtual void on_message_received(bit_stream stream, const uuid& id) override { }

	virtual void on_peer_joined(const uuid& id) override
	{
		if (remote.is_nil())
		{
			remote = id;
			std::cout << "connected to [" << id.to_string() << "]" << std::endl;
		}
	}

	virtual void on_peer_disconnected(const uuid& id) override
	{
		if (remote == id)
		{
			memset(&remote, 0, sizeof(remote));
			std::cout << "disconnected from [" << id.to_string() << "]" << std::endl;
		}
	}

	virtual void query_result_handler(const ip_address& addr, bool can_connect, bool has_password, uint32_t connections, uint32_t max_connections) override
	{
		std::cout << "query result: " << std::endl;
		std::cout << "\t" << "can_connect: " << can_connect << std::endl;
		std::cout << "\t" << "has_password: " << has_password << std::endl;
		std::cout << "\t" << "connections: " << connections << std::endl;
		std::cout << "\t" << "max_connections: " << max_connections << std::endl;
	}

	virtual void connect_result_handler(const uuid& id, bool result, uint32_t reason) override
	{
		if (!result)
		{
			std::cout << "connecting to the server failed with reason: " << reason << std::endl;
		}
	}

	void do_stress_test(network_session& ses)
	{
		std::cout << "local id = " << ses.local_id().to_string() << std::endl;

		std::thread background([&]
		{
			while (true)
			{
				{
					std::lock_guard<std::mutex> lg(sync);
					ses.update();
				}
				std::this_thread::yield();
			}
		});

		while (remote.is_nil())
		{
			std::string host_service_name;
			std::string host_service_port;
			ip_address	host;

			std::cout << "enter an address to connect to: ";
			std::cin >> host_service_name;
			std::cout << "enter a port to connect to: ";
			std::cin >> host_service_port;

			host.resolve(host_service_name.c_str(), host_service_port.c_str());

			uint32_t attempts_left = 4;
			while (attempts_left > 0 && remote.is_nil())
			{
				std::cout << "attempting to connect..." << std::endl;
				ses.try_connect(host, 0);
				--attempts_left;

				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
		}

		std::cout << "beginning reliable stress test. sending 100 packets of 100 uint32's with a lossy connection." << std::endl;

		{
			uint32_t buffer[100];
			std::lock_guard<std::mutex> lg(sync);

			for (uint32_t i = 0; i < 100; ++i)
			{
				for (uint32_t j = i * 100, k = 0; k < 100; ++j, ++k)
				{
					buffer[k] = j;
				}

				ses.send_reliable((char*)buffer, sizeof(buffer), remote);
			}
		}

		std::cout << "enter anything to terminate: " << std::endl;
		{ std::string temp1; std::cin >> temp1; }

		background.detach();

		std::cout << "terminating..." << std::endl;
	}

private:
	std::mutex	sync;
	uuid		remote;
};

int main(int argc, char** argv)
{
	WSAData wsa_data;

	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
	{
		printf("error initializing WSA\n");
		return 1;
	}

	network_session ses;
	stress_client client;

	std::string local_port;
	std::string is_server;

	std::cout << "enter a port to host on: ";
	std::cin >> local_port;

	ses.create(local_port.c_str(), 0, 1, &client, 400 * 200, 400 * 200, true);
	client.do_stress_test(ses);

	printf("terminating..\n");

	WSACleanup();

	return 0;
}