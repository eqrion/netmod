#include "stdafx.h"
#include "network_session.h"
#include <iostream>

namespace chat
{
	std::vector<uuid> remotes;
	std::vector<std::string> incoming_data;

	void special_on_message(bit_stream stream, const uuid& id)
	{
		printf("[%s] %s\n", id.to_string().c_str(), stream.seek());

		incoming_data.push_back(stream.seek());

		if (incoming_data.size() == 300)
		{
			std::sort(incoming_data.begin(), incoming_data.end());

			uint32_t d[300];
			for (auto iter = incoming_data.begin(); iter != incoming_data.end(); ++iter)
			{
				int r = atoi(iter->c_str());

				if (d[r] == 1 || r < 0 || r > 299)
					printf("failure");
				else
					d[r] = 1;
			}

			for (uint32_t i = 0; i < 300; ++i)
			{
				if (d[i] != 1)
					printf("failure");
			}

			printf("success");
		}
	}

	void on_message(bit_stream stream, const uuid& id)
	{
		printf("[%s] %s\n", id.to_string().c_str(), stream.seek());
	}

	void on_joined(const uuid& id)
	{
		remotes.push_back(id);

		printf("[%s] joined\n", id.to_string().c_str());
	}

	void on_disconnect(const uuid& id)
	{
		remotes.erase(std::find(remotes.begin(), remotes.end(), id));

		printf("[%s] disconnected\n", id.to_string().c_str());
	}

	void on_query_finished(const ip_address& addr, bool can_connect, bool has_password, uint32_t connections, uint32_t max_connections)
	{
	}

	void on_connect_finished(const uuid& id, bool result, uint32_t reason)
	{
		if (result)
		{
			printf("connecting to %s succeeded\n", id.to_string().c_str());
		}
		else
		{
			printf("connecting to %s failed\n", id.to_string().c_str());
		}
	}

	void loop(network_session* session)
	{
		std::string data;
		while (true)
		{
			std::cin >> data;

			for (auto iter = remotes.begin(); iter != remotes.end(); ++iter)
			{
				session->send_reliable(data.c_str(), data.size() + 1, *iter);
			}
		}
	}
	
	void special_loop(network_session* session)
	{
		std::string data;

		while (remotes.size() == 0) { std::this_thread::yield(); }

		uint32_t messages_to_send = 0;
		while (messages_to_send < 300)
		{
			char buffer[33];
			itoa(messages_to_send, buffer, 10);
			uint32_t buffer_len = strlen(buffer) + 1;

			for (auto iter = remotes.begin(); iter != remotes.end(); ++iter)
			{
				session->send_reliable(buffer, buffer_len, *iter);
			}

			++messages_to_send;
		}
		while (true)
		{
			std::cin >> data;

			for (auto iter = remotes.begin(); iter != remotes.end(); ++iter)
			{
				session->send_stream(data.c_str(), data.size() + 1, *iter);
			}
		}
	}
}

int _tmain(int argc, _TCHAR* argv[])
{
	WSAData wsa_data;

	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
	{
		printf("error initializing WSA\n");
		return 1;
	}

	network_session ses;
	
	std::string local_port;
	std::string host_service_name;
	std::string host_service_port;
	ip_address	host;

	std::cout << "enter a port to host on: ";
	std::cin >> local_port;


	if (local_port == "678")
	{
		ses.create(
			local_port.c_str(), 0, 2,
			chat::on_message,
			chat::on_joined,
			chat::on_disconnect,
			chat::on_query_finished,
			chat::on_connect_finished
			);
	}
	else
	{
		ses.create(
			local_port.c_str(), 0, 2,
			chat::special_on_message,
			chat::on_joined,
			chat::on_disconnect,
			chat::on_query_finished,
			chat::on_connect_finished
			);
	}

	std::cout << "local id = " << ses.local_id().to_string() << std::endl;

	std::string should_connect;
	std::cout << "do you want to connect to a server? (y/n) ";
	std::cin >> should_connect;

	if (should_connect == "y")
	{
		std::cout << "enter an address to connect to: ";
		std::cin >> host_service_name;
		std::cout << "enter a port to connect to: ";
		std::cin >> host_service_port;

		host.resolve(host_service_name.c_str(), host_service_port.c_str());

		ses.try_connect(host, 0);
	}

	if (local_port == "678")
	{
		chat::special_loop(&ses);
	}
	else
	{
		chat::loop(&ses);
	}

	printf("terminating..\n");
		
	WSACleanup();

	return 0;
}