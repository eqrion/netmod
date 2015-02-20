#include "include/network_session.h"
#include <iostream>

class chat_server : public network_session_handler
{
public:
	virtual void on_message_received(bit_stream stream, const uuid& id) override
	{
		std::string broadcast = "[" + id.to_string() + "] " + stream.seek() + "\n";
		std::cout << broadcast.c_str();
		outgoing.push(std::pair<uuid, std::string>(id, std::move(broadcast)));
	}

	virtual void on_peer_joined(const uuid& id) override
	{
		remotes.push_back(id);
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

		try
		{
			while (true)
			{
				while (!outgoing.empty())
				{
					std::pair<uuid, std::string> next_send = outgoing.front();

					for (auto iter = remotes.begin(); iter != remotes.end(); ++iter)
					{
						if (*iter == next_send.first)
							continue;
						ses.send_reliable(next_send.second.c_str(), next_send.second.length() + 1, *iter);
					}

					outgoing.pop();
				}

				ses.update();
				std::this_thread::yield();
			}
		}
		catch (const std::system_error& e)
		{
			std::cout << e.what() << std::endl;
		}
	}

private:
	std::vector<uuid>							remotes;
	std::queue<std::pair<uuid, std::string>>	outgoing;
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
	chat_server server;

	std::string local_port;
	std::string is_server;

	std::cout << "enter a port to host on: ";
	std::cin >> local_port;
	
	ses.create(local_port.c_str(), 0, 4, &server);
	server.loop(ses);

	printf("terminating..\n");

	WSACleanup();

	return 0;
}