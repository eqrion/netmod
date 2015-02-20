#include "include/network_session.h"
#include <iostream>
#include <thread>

class chat_client : public network_session_handler
{
public:
	chat_client()
	{
		memset(&remote, 0, sizeof(remote));
	}

	virtual void on_message_received(bit_stream stream, const uuid& id) override
	{
		std::cout << stream.seek();
	}

	virtual void on_peer_joined(const uuid& id) override
	{
		remote = id;
		std::cout << "connected to [" << id.to_string() << "]" << std::endl;
	}

	virtual void on_peer_disconnected(const uuid& id) override
	{
		memset(&remote, 0, sizeof(remote));
		std::cout << "disconnected from [" << id.to_string() << "]" << std::endl;
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

	void loop(network_session& ses)
	{
		std::cout << "local id = " << ses.local_id().to_string() << std::endl;

		std::thread background([&]
		{
			while (true)
			{
				ses.update();
				std::this_thread::yield();
			}
		});

		while (true)
		{
			std::string input;

			std::getline(std::cin, input);
			
			if (input == "?" || input == "help")
			{
				std::cout << "commands: " << std::endl;
				std::cout << "\thelp - prints this menu" << std::endl;
				std::cout << "\tconnect - connects to a chat server" << std::endl;
				std::cout << "\tdisconnect - disconnects to a chat server" << std::endl;
				std::cout << "\texit|quit - exits the client" << std::endl;
				std::cout << "\tquery - queries a remote ip for server status" << std::endl;
				std::cout << "\t'' - sends a message to the current server" << std::endl;
			}
			else if (input == "connect")
			{
				do_connect(ses);
			}
			else if (input == "disconnect")
			{
				do_disconnect(ses);
			}
			else if (input == "exit" || input == "quit")
			{
				break;
			}
			else if (input == "query")
			{
				do_query(ses);
			}
			else
			{
				if (!remote.is_nil() && input.length() > 0)
				{
					ses.send_reliable(input.c_str(), input.length() + 1, remote);
				}
			}
		}

		std::cout << "terminating..." << std::endl;
	}

private:
	uuid remote;

	void do_disconnect(network_session& ses)
	{
		if (!remote.is_nil())
		{
			std::cout << "disconnecting from [" << remote.to_string() << "]" << std::endl;
			ses.disconnect(remote);
			memset(&remote, 0, sizeof(remote));
		}
	}
	void do_connect(network_session& ses)
	{
		do_disconnect(ses);

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
	void do_query(network_session& ses)
	{
		std::string host_service_name;
		std::string host_service_port;
		ip_address	host;

		std::cout << "enter an address to query: ";
		std::cin >> host_service_name;
		std::cout << "enter a port to query: ";
		std::cin >> host_service_port;

		host.resolve(host_service_name.c_str(), host_service_port.c_str());

		ses.query(host);
	}
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
	chat_client client;

	std::string local_port;
	std::string is_server;

	std::cout << "enter a port to host on: ";
	std::cin >> local_port;

	ses.create(local_port.c_str(), 0, 1, &client);
	client.loop(ses);

	printf("terminating..\n");

	WSACleanup();

	return 0;
}