#ifndef onyx_network_session_h
#define onyx_network_session_h

#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <algorithm>
#include <stdint.h>
#include <list>
#include <random>

#include "uuid.h"
#include "bit_stream.h"
#include "network_timer.h"
#include "network.h"

enum connection_result : uint32_t
{
	connection_result_succeeded = 0,
	connection_result_invalid_protocol = 1,
	connection_result_invalid_password = 2,
	connection_result_server_full = 2,
};

class message_type
{
public:

	/*
	 * [1] header
	 * [4] protocol_version
	 * [4] password
	 * [16] guid
	 */
	static const uint8_t connection_request = 1;
	/*
	 * [1] header
	 * [16] guid
	 */
	static const uint8_t connection_accepted = 2;
	/*
	 * [1] header
	 * [4] reason
	 */
	static const uint8_t connection_rejected = 3;
	/*
	* [1] header
	*/
	static const uint8_t disconnecting = 4;

	/*
	* [1] header
	*/
	static const uint8_t query = 5;
	/*
	* [1] header
	* [4] protocol_version
	* [4] connected_peers
	* [1] has_password
	*/
	static const uint8_t query_response = 6;

	/*
	 * [1] header
	 * [1] stream_next_desired_message
	 * [1] reliable_next_desired_message
	 * [2] reliable_message_status
	 */
	static const uint8_t ping = 7;
	/*
	 * [1] header
	 */
	static const uint8_t ping_response = 8;

	/*
	 * [1] header
	 * [x] data
	 */
	static const uint8_t unreliable = 9;

	/*
	 * [1] header
	 * [1] message_id
	 * [1] next_desired_message
	 * [2] message_status_bitfield
	 * [x] data
	 */
	static const uint8_t reliable = 10;
	/*
	 * [1] header
	 * [1] next_desired_message
	 * [2] message_status_bitfield
	 */
	static const uint8_t reliable_ack = 11;

	/*
	 * [1] header
	 * [1] message_id
	 * [1] next_desired_message
	 * [2] message_status_bitfield
	 * [x] data
	 */
	static const uint8_t stream = 12;
	/*
	 * [1] header
	 * [1] next_desired_message
	 * [2] message_status_bitfield
	 */
	static const uint8_t stream_ack = 13;
};

class network_session
{
public:
	static const uint32_t maximum_transmission_unit = 400;

	static const uint32_t resend_time = 100000;
	static const uint32_t ping_time = 1000000;
	static const uint32_t timeout_time = 10000000;

	static const uint32_t protocol_version = 0x33366999;

	network_session() { }
	~network_session()
	{
		destroy();
	}

	bool create(
		const char* port_number,
		uint32_t password,
		uint32_t max_connections,
		std::function<void(bit_stream stream, const uuid&)>		message_handler,
		std::function<void(const uuid&)>					peer_joined_handler,
		std::function<void(const uuid&)>					peer_disconnect_handler,
		std::function<void(const ip_address&, bool, bool, uint32_t, uint32_t)>	query_result_handler,
		std::function<void(const uuid&, bool, uint32_t)>						connect_result_handler
		)
	{
		if (!_socket.create(port_number))
		{
			return false;
		}

		std::random_device device;
		std::mt19937 mt(device());
		
		_uuid = random_uuid_generator<std::mt19937>()(mt);
		_max_connections = max_connections;
		_password = password;

		_message_handler = message_handler;
		_peer_joined_handler = peer_joined_handler;
		_peer_disconnected_handler = peer_disconnect_handler;
		_query_result_handler = query_result_handler;
		_connect_attempt_result_handler = connect_result_handler;

		_thread = std::thread(network_session::thread_kernel, this);

		return true;
	}
	void destroy()
	{
		if (_thread_running)
		{
			_session_resource_mutex.lock();

			auto iter = _session_connections.begin();
			auto end = _session_connections.end();

			while (iter != end)
			{
				char disconnect_message[1];
				bit_stream stream(disconnect_message, sizeof(disconnect_message));
				stream.fast_write<uint8_t>(message_type::disconnecting);
				_socket.send(disconnect_message, stream.size(), iter->remote_address);

				++iter;
			}

			_session_resource_mutex.unlock();

			_thread_should_run = false;
			_thread.join();

			deallocate_packets();

			_session_connections.clear();
		}
		else
		{
			deallocate_packets();
		}
	}

	void send_stream(const char* buffer, const uint32_t length, uuid id)
	{
		_session_resource_mutex.lock();

		connection* con = find_connection(id);

		if (con != nullptr)
		{
			packet p = get_free_packet(network_session::maximum_transmission_unit);

			p.buffer_length = std::min(network_session::maximum_transmission_unit - 3, length);

			memcpy(p.buffer + 3, buffer, p.buffer_length);

			p.buffer_length += 3;

			con->stream.queue.push(p);
		}

		_session_resource_mutex.unlock();
	}
	void send_reliable(const char* buffer, const uint32_t length, uuid id)
	{
		_session_resource_mutex.lock();

		connection* con = find_connection(id);

		if (con != nullptr)
		{
			packet p = get_free_packet(network_session::maximum_transmission_unit);

			p.buffer_length = std::min(network_session::maximum_transmission_unit - 5, length);

			memcpy(p.buffer + 5, buffer, p.buffer_length);

			p.buffer_length += 5;

			con->reliable.queue.push(p);
		}

		_session_resource_mutex.unlock();
	}

	void send_unreliable(const char* buffer, const uint32_t length, uuid id)
	{
		_session_resource_mutex.lock();

		connection* con = find_connection(id);

		if (con != nullptr)
		{
			_socket.send(buffer, length, con->remote_address);
		}

		_session_resource_mutex.unlock();
	}

	void try_connect(const ip_address& addr, uint32_t password)
	{
		char connect_request_message[25];

		bit_stream stream(connect_request_message, sizeof(connect_request_message));

		stream.fast_write<uint8_t>(message_type::connection_request);
		stream.fast_write<uint32_t>(network_session::protocol_version);
		stream.fast_write<uint32_t>(password);
		stream.fast_write<uuid>(_uuid);

		_socket.send(connect_request_message, stream.size(), addr);
	}
	void disconnect(uuid id)
	{
		_session_resource_mutex.lock();

		connection* con = find_connection(id);

		if (con != nullptr)
		{
			char disconnect_message[1];
			bit_stream stream(disconnect_message, sizeof(disconnect_message));
			stream.fast_write<uint8_t>(message_type::disconnecting);
			_socket.send(disconnect_message, stream.size(), con->remote_address);

			_session_connections.erase(_session_connections.begin() + (con - _session_connections.data()));
		}

		_session_resource_mutex.unlock();
	}

	uuid find_id(const ip_address& addr)
	{
		_session_resource_mutex.lock();
		connection* con = find_connection(addr);
		_session_resource_mutex.unlock();

		if (con)
			return con->remote_uuid;
		else
			return uuid();
	}

	const uuid& local_id() const { return _uuid; }

private:

	static uint32_t modulus_distance(uint8_t leading_number, uint8_t trailing_number)
	{
		if (leading_number < trailing_number)
		{
			return 1 + leading_number + (reliable_messaging::maximum_sequence_number - trailing_number);
		}
		else
		{
			return leading_number - trailing_number;
		}
	}

	struct packet
	{
	public:
		packet() : buffer(nullptr), buffer_length(0), buffer_capacity(0) { }

		char*		buffer;
		size_t		buffer_length;
		size_t		buffer_capacity;

		bit_stream get_stream() { return bit_stream(buffer, buffer_length); }
	};

	struct stream_messaging
	{
	public:
		stream_messaging() :
			local_next_send(0),
			local_lowest_not_received(0),
			remote_lowest_not_received(0),
			last_ack_time(0),
			last_resend_time(0) { }

		static const uint32_t maximum_sequence_number = 255;
		static const uint32_t window_size = 16;

		uint8_t		local_next_send;
		uint8_t		local_lowest_not_received;
		uint8_t		remote_lowest_not_received;

		uint64_t last_ack_time;
		uint64_t last_resend_time;

		packet				window[window_size];
		std::queue<packet>	queue;

		uint32_t unknown_packet_range() const
		{
			return modulus_distance(local_next_send, remote_lowest_not_received);
		}

		bool is_valid_ack(uint8_t new_rnd) const
		{
			uint32_t dist_old_rnd = modulus_distance(local_next_send, remote_lowest_not_received);
			uint32_t dist_new_rnd = modulus_distance(local_next_send, new_rnd);

			return dist_new_rnd <= dist_old_rnd;
		}

		bool is_valid_receive_packet(uint8_t seq) const
		{
			// assume that the sequence number is never less than the lowest_not_received

			return modulus_distance(seq, local_lowest_not_received) < reliable_messaging::window_size;
		}

		bool can_send_message() const
		{
			// assume that the next_send is never less than the remote_lowest_not_received

			return modulus_distance(local_next_send, remote_lowest_not_received) < reliable_messaging::window_size;
		}
	};

	struct reliable_messaging
	{
	public:
		reliable_messaging() :
			local_next_send(0),
			local_lowest_not_received(0),
			local_receive_status(0),
			remote_lowest_not_received(0),
			remote_message_status(0),
			last_ack_time(0),
			last_resend_time(0) { }

		static const uint32_t maximum_sequence_number = 255;
		static const uint32_t window_size = 16;

		/* 
		 *
		 * window_size = 3
		 *
		 * [O] [O]
		 *
		 * [a] [b] [a] [b]
		 * [0] [1] [2] [3]
		 * 
		 */

		uint8_t		local_next_send;
		uint8_t		local_lowest_not_received;
		uint16_t	local_receive_status;

		uint8_t		remote_lowest_not_received;
		uint16_t	remote_message_status;

		uint64_t last_ack_time;
		uint64_t last_resend_time;

		packet				window[window_size];
		std::queue<packet>	queue;
		
		uint32_t unknown_packet_range() const
		{
			return modulus_distance(local_next_send, remote_lowest_not_received);
		}

		bool is_valid_ack(uint8_t new_rnd) const
		{
			uint32_t dist_old_rnd = modulus_distance(local_next_send, remote_lowest_not_received);
			uint32_t dist_new_rnd = modulus_distance(local_next_send, new_rnd);

			return dist_new_rnd <= dist_old_rnd;
		}
		
		bool can_send_message() const
		{
			// assume that the next_send is never less than the remote_lowest_not_received

			return modulus_distance(local_next_send, remote_lowest_not_received) < reliable_messaging::window_size;
		}
	};

	struct connection
	{
	public:
		connection() :
			last_ping_time(0),
			round_trip_delay(0),
			time_stamp_offset(0),
			disconnected(false) { }

		ip_address	remote_address;
		uuid		remote_uuid;

		uint64_t last_ping_time;
		uint64_t round_trip_delay;
		uint64_t time_stamp_offset;

		stream_messaging stream;
		reliable_messaging reliable;

		bool disconnected;		
	};

	// properties

	uuid					_uuid;
	uint32_t				_password;
	uint32_t				_max_connections;

	// os resources

	network_timer			_timer;
	udp_socket				_socket;

	// handlers

	std::function<void(bit_stream, const uuid&)>		_message_handler;
	std::function<void(const uuid&)>					_peer_joined_handler;
	std::function<void(const uuid&)>					_peer_disconnected_handler;
	std::function<void(const ip_address&, bool, bool, uint32_t, uint32_t)>	_query_result_handler;
	std::function<void(const uuid&, bool, uint32_t)>						_connect_attempt_result_handler;

	// session resources

	std::mutex				_session_resource_mutex;
	std::list<packet>		_session_free_packets;
	std::vector<connection>	_session_connections;

	// thread resources

	std::thread		_thread;
	volatile bool	_thread_running;
	volatile bool	_thread_should_run;
	
	// helper functions \not thread safe\

	connection* find_connection(const ip_address& addr)
	{
		auto iter = _session_connections.begin();
		auto end = _session_connections.end();

		while (iter != end)
		{
			if (iter->remote_address == addr)
				return &*iter;

			++iter;
		}

		return nullptr;
	}
	connection* find_connection(const uuid& id)
	{
		auto iter = _session_connections.begin();
		auto end = _session_connections.end();

		while (iter != end)
		{
			if (iter->remote_uuid == id)
				return &*iter;

			++iter;
		}

		return nullptr;
	}

	packet get_free_packet(size_t capacity)
	{
		auto iter = _session_free_packets.begin();
		auto end = _session_free_packets.end();

		while (iter != end)
		{
			if (iter->buffer_capacity == capacity)
			{
				packet result = *iter;

				_session_free_packets.erase(iter);

				return result;
			}

			++iter;
		}

		return allocate_packet(capacity);
	}

	packet allocate_packet(size_t capacity)
	{
		packet message;
		message.buffer = new char[capacity];
		message.buffer_capacity = capacity;
		return message;
	}
	void deallocate_packet(packet& message)
	{
		if (message.buffer != nullptr)
		{
			delete[] message.buffer;
			message.buffer = nullptr;
			message.buffer_capacity = 0;
			message.buffer_length = 0;
		}
	}

	void deallocate_packets()
	{
		for (auto iter = _session_free_packets.begin(); iter != _session_free_packets.end(); ++iter)
		{
			deallocate_packet(*iter);
		}
		_session_free_packets.clear();

		for (auto con = _session_connections.begin(); con != _session_connections.end(); ++con)
		{
			for (uint32_t i = 0; i < stream_messaging::window_size; ++i)
			{
				deallocate_packet(con->stream.window[i]);
			}

			while (!con->stream.queue.empty())
			{
				deallocate_packet(con->stream.queue.front());
				con->stream.queue.pop();
			}
			while (!con->reliable.queue.empty())
			{
				deallocate_packet(con->reliable.queue.front());
				con->reliable.queue.pop();
			}
		}
	}

	// networking thread

	static void thread_kernel(network_session* session)
	{
		packet		incoming_message;
		ip_address	incoming_address;

		incoming_message = session->allocate_packet(network_session::maximum_transmission_unit);

		session->_thread_running = true;
		session->_thread_should_run = true;
		
		while (session->_thread_should_run)
		{
			while (
				session->_socket.try_receive(
				incoming_message.buffer,
				incoming_message.buffer_capacity,
				&incoming_message.buffer_length,
				&incoming_address)
				)
			{
				session->_session_resource_mutex.lock();
				connection* con = session->find_connection(incoming_address);

				if (con != nullptr)
				{
					process_incoming_message_from_connected(
						&incoming_message,
						con,
						session
						);
				}
				else
				{
					process_incoming_message_from_unconnected(
						&incoming_message,
						incoming_address,
						session
						);
				}
				session->_session_resource_mutex.unlock();
			}

			update_connections(session);

			std::this_thread::yield();
		}

		session->deallocate_packet(incoming_message);
	}

	// handling incoming messages

	static void process_incoming_message_from_unconnected(packet* msg, const ip_address& remote_addr, network_session* session)
	{
		bit_stream stream = bit_stream(msg->buffer, msg->buffer_length);

		if (stream.size() < sizeof(uint8_t))
		{
			return;
		}
		uint8_t message_header = stream.fast_read<uint8_t>();

		switch (message_header)
		{
		case message_type::connection_request:
		{
			if (stream.size() == 25)
			{
				uint32_t protocol_version = stream.fast_read<uint32_t>();
				uint32_t password = stream.fast_read<uint32_t>();
				uuid remote_uuid = stream.fast_read<uuid>();

				uint32_t result = connection_result_succeeded;
				if (protocol_version != network_session::protocol_version)
				{
					result = connection_result_invalid_protocol;
				}
				else if (password != session->_password)
				{
					result = connection_result_invalid_password;
				}
				else if (session->_session_connections.size() >= session->_max_connections)
				{
					result = connection_result_server_full;
				}

				if (result == connection_result_succeeded)
				{
					char connection_accepted_response[17];

					stream.attach(connection_accepted_response, 17);
					stream.fast_write<uint8_t>(message_type::connection_accepted);
					stream.fast_write<uuid>(session->_uuid);

					session->_socket.send(connection_accepted_response, 17, remote_addr);

					uint64_t current_time = session->_timer.get_microseconds();
					connection new_connection;
					new_connection.remote_address = remote_addr;
					new_connection.remote_uuid = remote_uuid;
					new_connection.stream.last_resend_time = current_time;
					new_connection.stream.last_ack_time = current_time;
					new_connection.reliable.last_resend_time = current_time;
					new_connection.reliable.last_ack_time = current_time;
					new_connection.last_ping_time = current_time;
					session->_session_connections.push_back(std::move(new_connection));

					session->_peer_joined_handler(remote_uuid);
				}
				else
				{
					char connection_rejected_response[5];

					stream.attach(connection_rejected_response, 5);
					stream.fast_write<uint8_t>(message_type::connection_rejected);
					stream.fast_write<uint32_t>(result);

					session->_socket.send(connection_rejected_response, 5, remote_addr);
				}
			}
		}
		break;
		case message_type::connection_accepted:
		{
			if (stream.size() == 17)
			{
				uuid remote_uuid = stream.fast_read<uuid>();

				uint64_t current_time = session->_timer.get_microseconds();
				connection new_connection;
				new_connection.remote_address = remote_addr;
				new_connection.remote_uuid = remote_uuid;
				new_connection.stream.last_resend_time = current_time;
				new_connection.stream.last_ack_time = current_time;
				new_connection.reliable.last_resend_time = current_time;
				new_connection.reliable.last_ack_time = current_time;
				new_connection.last_ping_time = current_time;
				session->_session_connections.push_back(std::move(new_connection));

				session->_peer_joined_handler(remote_uuid);
				session->_connect_attempt_result_handler(remote_uuid, true, 0);
			}
		}
		break;
		case message_type::connection_rejected:
		{
			if (stream.size() == 5)
			{
				uint32_t reason = stream.fast_read<uint32_t>();
				session->_connect_attempt_result_handler(uuid(), false, reason);
			}
		}
		break;

		case message_type::query:
		{
			char query_response[14];

			stream.attach(query_response, 14);

			stream.fast_write<uint8_t>(message_type::query_response);
			stream.fast_write<uint32_t>(network_session::protocol_version);
			stream.fast_write<uint32_t>(session->_session_connections.size());
			stream.fast_write<uint32_t>(session->_max_connections);
			stream.fast_write<uint8_t>(session->_password == 0 ? 0 : 1);

			session->_socket.send(query_response, 14, remote_addr);
		}
		break;
		case message_type::query_response:
		{
			if (stream.size() == 14)
			{
				uint32_t protocol_version = stream.fast_read<uint32_t>();
				uint32_t connections = stream.fast_read<uint32_t>();
				uint32_t max_connections = stream.fast_read<uint32_t>();
				uint8_t has_password = stream.fast_read<uint8_t>();

				session->_query_result_handler(
					remote_addr,
					protocol_version == network_session::protocol_version,
					has_password != 0,
					connections,
					max_connections
					);
			}
		}
		break;
		}
	}
	static void process_incoming_message_from_connected(packet* msg, connection* con, network_session* session)
	{
		bit_stream stream = bit_stream(msg->buffer, msg->buffer_length);

		if (stream.size() < sizeof(uint8_t))
		{
			return;
		}
		uint8_t message_header = stream.fast_read<uint8_t>();
		
		switch (message_header)
		{
		case message_type::disconnecting:
		{
			session->_peer_disconnected_handler(con->remote_uuid);

			session->_session_connections.erase(
				session->_session_connections.begin() + (con - session->_session_connections.data())
				);
		}
		break;

		case message_type::ping:
		{
			if (stream.size() == 5)
			{
				char ping_response[5];
				stream.attach(ping_response, 5);

				stream.fast_write<uint8_t>(message_type::ping_response);
				stream.fast_write<uint8_t>(con->stream.local_lowest_not_received);
				stream.fast_write<uint8_t>(con->reliable.local_lowest_not_received);
				stream.fast_write<uint16_t>(con->reliable.local_receive_status);

				session->_socket.send(ping_response, 5, con->remote_address);
			}
		}
		break;
		case message_type::ping_response:
		{
			if (stream.size() == 5)
			{
				process_stream_acknowledgment(stream.fast_read<uint8_t>(), con, session);

				uint8_t reliable_remote_lnr = stream.fast_read<uint8_t>();
				uint16_t reliable_remote_ms = stream.fast_read<uint16_t>();

				process_reliable_acknowledgment(
					reliable_remote_lnr,
					reliable_remote_ms,
					con,
					session
					);
			}
		}
		break;
		
		case message_type::stream:
		{
			if (stream.size() >= 3)
			{
				uint8_t message_id = stream.fast_read<uint8_t>();
			
				process_stream_acknowledgment(stream.fast_read<uint8_t>(), con, session);

				if (message_id == con->stream.local_lowest_not_received)
				{
					++con->stream.local_lowest_not_received;
					session->_message_handler(
						bit_stream(stream.seek(), stream.size() - stream.tell()),
						con->remote_uuid
						);

					char reliable_ack[2];
					stream.attach(reliable_ack, 2);
					stream.fast_write<uint8_t>(message_type::stream_ack);
					stream.fast_write<uint8_t>(con->stream.local_lowest_not_received);

					session->_socket.send(reliable_ack, 2, con->remote_address);
				}
			}
		}
		break;

		case message_type::stream_ack:
		{
			if (stream.size() == 2)
			{
				process_stream_acknowledgment(stream.fast_read<uint8_t>(), con, session);				
			}
		}
		break;

		case message_type::reliable:
		{
			if (stream.size() >= 5)
			{
				uint8_t message_id = stream.fast_read<uint8_t>();

				uint8_t remote_lowest_not_received = stream.fast_read<uint8_t>();
				uint16_t remote_message_status = stream.fast_read<uint16_t>();

				process_reliable_acknowledgment(
				remote_lowest_not_received,
				remote_message_status,
				con,
				session
				);

				uint32_t message_index = modulus_distance(message_id, con->reliable.local_lowest_not_received);
				uint32_t message_flag = 1 << message_index;

				if (
					message_index < reliable_messaging::window_size
					&& !(con->reliable.local_receive_status & message_flag)
					)
				{
					con->reliable.local_receive_status |= message_flag;

					while (con->reliable.local_receive_status & 1)
					{
						++con->reliable.local_lowest_not_received;
						con->reliable.local_receive_status = con->reliable.local_receive_status >> 1;
					}

					session->_message_handler(
						bit_stream(stream.seek(), stream.size() - stream.tell()),
						con->remote_uuid
						);

					char reliable_ack[4];
					stream.attach(reliable_ack, 4);
					stream.fast_write<uint8_t>(message_type::reliable_ack);
					stream.fast_write<uint8_t>(con->reliable.local_lowest_not_received);
					stream.fast_write<uint16_t>(con->reliable.local_receive_status);

					session->_socket.send(reliable_ack, 4, con->remote_address);
				}
			}
		}
		break;

		case message_type::reliable_ack:
		{
			if (stream.size() == 4)
			{
				uint8_t remote_lowest_not_received = stream.fast_read<uint8_t>();
				uint16_t remote_message_status = stream.fast_read<uint16_t>();

				process_reliable_acknowledgment(
					remote_lowest_not_received,
					remote_message_status,
					con,
					session
					);
			}
		}
		break;

		case message_type::unreliable:
		{
			session->_message_handler(
				bit_stream(stream.seek(), stream.size() - stream.tell()),
				con->remote_uuid
				);
		}
		break;
		}

	}

	static void process_stream_acknowledgment(uint8_t new_rnd, connection* con, network_session* session)
	{
		// ensure the new_rnd has either remained the same or acknowledged some packets

		if (con->stream.is_valid_ack(new_rnd))
		{
			con->stream.last_ack_time = session->_timer.get_microseconds();
			
			if (new_rnd < con->stream.remote_lowest_not_received)
			{
				for (uint32_t i = con->stream.remote_lowest_not_received; i <= stream_messaging::maximum_sequence_number; ++i)
				{
					uint32_t message_index = i % stream_messaging::window_size;

					if (con->stream.window[message_index].buffer != nullptr)
					{
						session->_session_free_packets.push_back(
							con->stream.window[message_index]
							);

						con->stream.window[message_index] = packet();
					}
				}
				for (uint32_t i = 0; i < new_rnd; ++i)
				{
					uint32_t message_index = i % stream_messaging::window_size;

					if (con->stream.window[message_index].buffer != nullptr)
					{
						session->_session_free_packets.push_back(
							con->stream.window[message_index]
							);

						con->stream.window[message_index] = packet();
					}
				}
			}
			else
			{
				for (uint32_t i = con->stream.remote_lowest_not_received; i < new_rnd; ++i)
				{
					uint32_t message_index = i % stream_messaging::window_size;

					if (con->stream.window[message_index].buffer != nullptr)
					{
						session->_session_free_packets.push_back(
							con->stream.window[message_index]
							);

						con->stream.window[message_index] = packet();
					}
				}
			}

			con->stream.remote_lowest_not_received = new_rnd;
		}
	}
	static void process_reliable_acknowledgment(uint8_t new_rnd, uint16_t new_status, connection* con, network_session* session)
	{
		// ensure the new_rnd has either remained the same or acknowledged some packets

		if (con->reliable.is_valid_ack(new_rnd))
		{
			con->reliable.last_ack_time = session->_timer.get_microseconds();

			if (new_rnd < con->reliable.remote_lowest_not_received)
			{
				for (uint32_t i = con->reliable.remote_lowest_not_received; i <= reliable_messaging::maximum_sequence_number; ++i)
				{
					uint32_t message_index = i % reliable_messaging::window_size;

					if (con->reliable.window[message_index].buffer != nullptr)
					{
						session->_session_free_packets.push_back(
							con->reliable.window[message_index]
							);

						con->reliable.window[message_index] = packet();
					}
				}
				for (uint32_t i = 0; i < new_rnd; ++i)
				{
					uint32_t message_index = i % reliable_messaging::window_size;

					if (con->reliable.window[message_index].buffer != nullptr)
					{
						session->_session_free_packets.push_back(
							con->reliable.window[message_index]
							);

						con->reliable.window[message_index] = packet();
					}
				}
			}
			else
			{
				for (uint32_t i = con->reliable.remote_lowest_not_received; i < new_rnd; ++i)
				{
					uint32_t message_index = i % reliable_messaging::window_size;

					if (con->reliable.window[message_index].buffer != nullptr)
					{
						session->_session_free_packets.push_back(
							con->reliable.window[message_index]
							);

						con->reliable.window[message_index] = packet();
					}
				}
			}

			con->reliable.remote_message_status = new_status;
			con->reliable.remote_lowest_not_received = new_rnd;
		}
	}

	// handling outgoing messaging

	static void update_connections(network_session* session)
	{
		session->_session_resource_mutex.lock();

		auto con = session->_session_connections.begin();
		auto end = session->_session_connections.end();

		while (con != end)
		{
			uint64_t current_time = session->_timer.get_microseconds();
			uint64_t time_since_last_ping = current_time - con->last_ping_time;
			uint64_t time_since_last_stream_ack = current_time - con->stream.last_ack_time;
			uint64_t time_since_last_reliable_ack = current_time - con->reliable.last_ack_time;

			// disconnect from the remote if we haven't gotten an acknowledgment in a while

			if (time_since_last_stream_ack > network_session::timeout_time && 
				time_since_last_reliable_ack > network_session::timeout_time)
			{
				con->disconnected = true;
				++con;

				continue;
			}

			// update stream messaging

			update_stream_messaging(&*con, session);

			// update reliable messaging

			update_reliable_messaging(&*con, session);

			// ping the remote if we haven't pinged them in a while

			if (time_since_last_ping > network_session::ping_time)
			{
				con->last_ping_time = current_time;

				char ping_message[5];
				bit_stream stream(ping_message, 5);

				stream.fast_write<uint8_t>(message_type::ping);
				stream.fast_write<uint8_t>(con->stream.local_lowest_not_received);
				stream.fast_write<uint8_t>(con->reliable.local_lowest_not_received);
				stream.fast_write<uint16_t>(con->reliable.local_receive_status);

				session->_socket.send(ping_message, 5, con->remote_address);
			}

			++con;
		}

		// prune out the recently disconnected connections

		bool checked_every_connection = false;
		while (!checked_every_connection)
		{
			bool found_disconnected = false;

			auto con = session->_session_connections.begin();
			auto end = session->_session_connections.end();

			while (con != end)
			{
				if (con->disconnected)
				{
					session->_peer_disconnected_handler(con->remote_uuid);
					session->_session_connections.erase(con);
					found_disconnected = true;
					break;
				}
				++con;
			}

			if (!found_disconnected)
				checked_every_connection = true;
		}

		session->_session_resource_mutex.unlock();
	}

	static void update_stream_messaging(connection* con, network_session* session)
	{
		uint64_t current_time = session->_timer.get_microseconds();

		bit_stream stream;
		while (!con->stream.queue.empty() && con->stream.can_send_message())
		{
			// reset the resend time on the connection because we are sending a message

			con->stream.last_resend_time = current_time;

			// move the queued message to the window

			uint32_t message_index = con->stream.local_next_send % stream_messaging::window_size;

			con->stream.window[message_index] = con->stream.queue.front();
			con->stream.queue.pop();

			// write the packet header and send it

			stream.attach(
				con->stream.window[message_index].buffer,
				con->stream.window[message_index].buffer_length
				);

			stream.fast_write<uint8_t>(message_type::stream);
			stream.fast_write<uint8_t>(con->stream.local_next_send);
			stream.fast_write<uint8_t>(con->stream.local_lowest_not_received);

			session->_socket.send(
				con->stream.window[message_index].buffer,
				con->stream.window[message_index].buffer_length,
				con->remote_address
				);

			// advance the window forward, let it wrap around

			++con->stream.local_next_send;
		}

		uint64_t time_since_last_resend = current_time - con->stream.last_resend_time;

		// resend messages if we have unacknowledged messages and haven't sent a reliable message in a while

		if (
			time_since_last_resend > network_session::resend_time &&
			con->stream.unknown_packet_range() > 0
			)
		{
			con->stream.last_resend_time = current_time;

			if (con->stream.local_next_send < con->stream.remote_lowest_not_received)
			{
				for (uint32_t i = con->stream.remote_lowest_not_received; i <= stream_messaging::maximum_sequence_number; ++i)
				{
					resend_stream_message(i, con, session);
				}
				for (uint32_t i = 0; i < con->stream.local_next_send; ++i)
				{
					resend_stream_message(i, con, session);
				}
			}
			else
			{
				for (uint32_t i = con->stream.remote_lowest_not_received; i < con->stream.local_next_send; ++i)
				{
					resend_stream_message(i, con, session);
				}
			}
		}
	}
	static void update_reliable_messaging(connection* con, network_session* session)
	{
		uint64_t current_time = session->_timer.get_microseconds();

		bit_stream reliable;
		while (!con->reliable.queue.empty() && con->reliable.can_send_message())
		{
			// reset the resend time on the connection because we are sending a message

			con->reliable.last_resend_time = current_time;

			// move the queued message to the window

			uint32_t message_index = con->reliable.local_next_send % reliable_messaging::window_size;

			con->reliable.window[message_index] = con->reliable.queue.front();
			con->reliable.queue.pop();

			// write the packet header and send it

			reliable.attach(
				con->reliable.window[message_index].buffer,
				con->reliable.window[message_index].buffer_length
				);

			reliable.fast_write<uint8_t>(message_type::reliable);
			reliable.fast_write<uint8_t>(con->reliable.local_next_send);
			reliable.fast_write<uint8_t>(con->reliable.local_lowest_not_received);
			reliable.fast_write<uint16_t>(con->reliable.local_receive_status);

			session->_socket.send(
				con->reliable.window[message_index].buffer,
				con->reliable.window[message_index].buffer_length,
				con->remote_address
				);

			// advance the window forward, let it wrap around

			++con->reliable.local_next_send;
		}

		uint64_t time_since_last_resend = current_time - con->reliable.last_resend_time;

		// resend messages if we have unacknowledged messages and haven't sent a reliable message in a while

		if (
			time_since_last_resend > network_session::resend_time &&
			con->reliable.unknown_packet_range() > 0
			)
		{
			con->reliable.last_resend_time = current_time;
			uint32_t message_flag = 1;

			if (con->reliable.local_next_send < con->reliable.remote_lowest_not_received)
			{
				for (uint32_t i = con->reliable.remote_lowest_not_received; i <= reliable_messaging::maximum_sequence_number; ++i)
				{
					if (!(con->reliable.remote_message_status & message_flag))
					{
						resend_reliable_message(i, con, session);
					}
					message_flag = message_flag << 1;
				}
				for (uint32_t i = 0; i < con->reliable.local_next_send; ++i)
				{
					if (!(con->reliable.remote_message_status & message_flag))
					{
						resend_reliable_message(i, con, session);
					}
					message_flag = message_flag << 1;
				}
			}
			else
			{
				for (uint32_t i = con->reliable.remote_lowest_not_received; i < con->reliable.local_next_send; ++i)
				{
					if (!(con->reliable.remote_message_status & message_flag))
					{
						resend_reliable_message(i, con, session);
					}
					message_flag = message_flag << 1;
				}
			}
		}
	}

	static void resend_stream_message(uint32_t seq, connection* con, network_session* session)
	{
		uint32_t message_index = seq % stream_messaging::window_size;

		// we need to update the next desired field of the header, it may have changed

		bit_stream stream = con->stream.window[message_index].get_stream();
		stream.skip(2);
		stream.fast_write<uint8_t>(con->stream.local_lowest_not_received);

		session->_socket.send(
			con->stream.window[message_index].buffer,
			con->stream.window[message_index].buffer_length,
			con->remote_address
			);
	}
	static void resend_reliable_message(uint32_t seq, connection* con, network_session* session)
	{
		uint32_t message_index = seq % reliable_messaging::window_size;

		// we need to update the next desired field of the header, it may have changed

		bit_stream reliable = con->reliable.window[message_index].get_stream();
		reliable.skip(2);
		reliable.fast_write<uint8_t>(con->reliable.local_lowest_not_received);
		reliable.fast_write<uint16_t>(con->reliable.local_receive_status);

		session->_socket.send(
			con->reliable.window[message_index].buffer,
			con->reliable.window[message_index].buffer_length,
			con->remote_address
			);
	}

};

#endif