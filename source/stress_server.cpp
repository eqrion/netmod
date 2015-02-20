#include "include/network_session.h"
#include <iostream>

class stress_server : public network_session_handler
{
public:
	virtual void on_message_received(bit_stream stream, const uuid& id) override
	{
		while (!stream.eof())
		{
			uint32_t val = stream.fast_read<uint32_t>();
			received[val] = true;
		}

		check_received();
	}

	virtual void on_peer_joined(const uuid& id) override
	{
		remote = id;
		std::cout << "[" << id.to_string().c_str() << "] joined" << std::endl;
	}

	virtual void on_peer_disconnected(const uuid& id) override
	{
		std::cout << "[" << id.to_string().c_str() << "] disconnected" << std::endl;
	}

	virtual void query_result_handler(const ip_address& addr, bool can_connect, bool has_password, uint32_t connections, uint32_t max_connections) override
	{
	}

	virtual void connect_result_handler(const uuid& id, bool result, uint32_t reason) override
	{
	}

	void loop(network_session& ses)
	{
		std::cout << "local id = " << ses.local_id().to_string() << std::endl;

		reset_received();

		while (true)
		{
			ses.update();
			std::this_thread::yield();
		}
	}

private:
	uuid 				remote;

	void reset_received()
	{
		received.reserve(10000);
		received.clear();
		for (uint32_t i = 0; i < 10000; ++i)
		{
			received.push_back(false);
		}
		std::cout << "waiting for the numbers [0, 99999]." << std::endl;
	}
	void check_received()
	{
		bool all_in = true;
		for (uint32_t i = 0; i < 10000; ++i)
		{
			if (received[i] == false)
			{
				all_in = false;
				break;
			}
		}

		if (all_in)
		{
			std::cout << "all the numbers are in! resetting..." << std::endl;
			reset_received();
		}
	}

	std::vector<bool> 	received;
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
	stress_server server;

	std::string local_port;
	std::string is_server;

	std::cout << "enter a port to host on: ";
	std::cin >> local_port;
	
	ses.create(local_port.c_str(), 0, 1, &server, 400 * 200, 400 * 200, true);
	server.loop(ses);

	printf("terminating..\n");

	WSACleanup();

	return 0;
}