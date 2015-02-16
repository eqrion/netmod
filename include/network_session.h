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
#include "network.h"
#include "circular_allocator.h"

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

class network_session_handler
{
public:
	virtual void on_message_received(bit_stream, const uuid&) = 0;
	virtual void on_peer_joined(const uuid&) = 0;
	virtual void on_peer_disconnected(const uuid&) = 0;
	virtual void query_result_handler(const ip_address&, bool, bool, uint32_t, uint32_t) = 0;
	virtual void connect_result_handler(const uuid&, bool, uint32_t) = 0;
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

	network_session(const network_session& rhs) = delete;
	network_session(network_session&& rhs) = delete;
	network_session& operator=(const network_session&) = delete;
	network_session& operator=(network_session&&) = delete;

	bool create(
		const char* port_number,
		uint32_t password,
		uint32_t max_connections,
		network_session_handler* handler
		)
	{
		destroy();

		if (!_socket.create(port_number))
		{
			return false;
		}
		
		if (handler == nullptr)
		{
			return false;
		}

		std::random_device device;
		std::mt19937 mt(device());
		
		_uuid = random_uuid_generator<std::mt19937>()(mt);
		_max_connections = max_connections;
		_password = password;
		_handler = handler;

		_receive_packet.buffer = new char[network_session::maximum_transmission_unit];
		_receive_packet.buffer_length = network_session::maximum_transmission_unit;

		return true;
	}
	void destroy()
	{
		_socket.destroy();

		if (_receive_packet.buffer != nullptr)
		{
			delete[] _receive_packet.buffer;
			_receive_packet.buffer = 0;
			_receive_packet.buffer_length = 0;
		}

		auto iter = _connections.begin();
		auto end = _connections.end();

		while (iter != end)
		{
			char disconnect_message[1];
			bit_stream stream(disconnect_message, sizeof(disconnect_message));
			stream.fast_write<uint8_t>(message_type::disconnecting);
			_socket.send(disconnect_message, stream.size(), iter->remote_address());

			++iter;
		}

		_connections.clear();
	}

	void send_unreliable(const char* buffer, const uint32_t length, uuid id)
	{
		if (length > network_session::maximum_transmission_unit)
			return;
		
		connection* con = find_connection(id);

		if (con != nullptr)
		{
			con->send_unreliable(buffer, length);
		}
	}
	void send_reliable(const char* buffer, const uint32_t length, uuid id)
	{
		if (length > network_session::maximum_transmission_unit)
			return;

		connection* con = find_connection(id);

		if (con != nullptr)
		{
			con->send_reliable(buffer, length);
		}
	}
	void send_stream(const char* buffer, const uint32_t length, uuid id)
	{
		if (length > network_session::maximum_transmission_unit)
			return;

		connection* con = find_connection(id);

		if (con != nullptr)
		{
			con->send_stream(buffer, length);
		}
	}
	
	void update()
	{
		receive_packets();
		update_connections();
	}

	void query(const ip_address& addr)
	{
		char query_message[1];

		bit_stream stream(query_message, sizeof(query_message));

		stream.fast_write<uint8_t>(message_type::query);

		_socket.send(query_message, stream.size(), addr);
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
		connection* con = find_connection(id);

		if (con != nullptr)
		{
			char disconnect_message[1];
			bit_stream stream(disconnect_message, sizeof(disconnect_message));
			stream.fast_write<uint8_t>(message_type::disconnecting);
			_socket.send(disconnect_message, stream.size(), con->remote_address());

			_connections.erase(_connections.begin() + (con - _connections.data()));
		}
	}

	uuid find_id(const ip_address& addr)
	{
		connection* con = find_connection(addr);

		if (con)
			return con->remote_uuid();
		else
			return uuid();
	}
	const uuid& local_id() const { return _uuid; }

private:
	
	struct packet
	{
	public:
		packet() : buffer(nullptr), buffer_length(0) { }

		char*		buffer;
		size_t		buffer_length;

		bit_stream get_stream() { return bit_stream(buffer, buffer_length); }
	};

	class connection
	{
	public:
		connection() :
			_session(nullptr),
			_last_ping_time(0),
			_disconnected(false)
		{
		}
		connection(const connection& rhs) :
			_session(rhs._session),
			_remote_address(rhs._remote_address),
			_remote_uuid(rhs._remote_uuid),
			_last_ping_time(rhs._last_ping_time),
			_stream_messenger(rhs._stream_messenger),
			_reliable_messenger(rhs._reliable_messenger),
			_disconnected(rhs._disconnected)
		{
			_stream_messenger.set_connection(this);
			_reliable_messenger.set_connection(this);
		}
		connection(connection&& rhs) :
			_session(rhs._session),
			_remote_address(rhs._remote_address),
			_remote_uuid(rhs._remote_uuid),
			_last_ping_time(rhs._last_ping_time),
			_stream_messenger(std::move(rhs._stream_messenger)),
			_reliable_messenger(std::move(rhs._reliable_messenger)),
			_disconnected(rhs._disconnected)
		{
			_stream_messenger.set_connection(this);
			_reliable_messenger.set_connection(this);
		}
		const connection& operator=(connection rhs)
		{
			swap(*this, rhs);
			return *this;
		}

		const ip_address& remote_address() const { return _remote_address; }
		const uuid& remote_uuid() const { return _remote_uuid; }
		bool is_disconnected() const { return _disconnected; }

		void create(network_session* session, const ip_address& remote_address, const uuid& remote_uuid)
		{
			_session = session;

			_remote_address = remote_address;
			_remote_uuid = remote_uuid;
			
			_last_ping_time = session->_timer.get_microseconds();

			_stream_messenger.create(session, this);
			_reliable_messenger.create(session, this);

			_disconnected = false;
		}

		void receive_message(packet* msg, uint64_t current_time)
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
				_disconnected = true;
			}
			break;

			case message_type::ping:
			{
				if (stream.size() == 5)
				{
					char ping_response[5];
					stream.attach(ping_response, 5);

					stream.fast_write<uint8_t>(message_type::ping_response);
					stream.fast_write<uint8_t>(_stream_messenger.local_low_n_received());
					stream.fast_write<uint8_t>(_reliable_messenger.local_low_n_received());
					stream.fast_write<uint16_t>(_reliable_messenger.local_messages_received());

					_session->_socket.send(ping_response, 5, _remote_address);
				}
			}
			break;
			case message_type::ping_response:
			{
				if (stream.size() == 5)
				{
					_stream_messenger.receive_ack(stream.fast_read<uint8_t>(), current_time);

					uint8_t reliable_remote_lnr = stream.fast_read<uint8_t>();
					uint16_t reliable_remote_ms = stream.fast_read<uint16_t>();

					_reliable_messenger.receive_ack(reliable_remote_lnr, reliable_remote_ms, current_time);
				}
			}
			break;

			case message_type::stream:
			{
				_stream_messenger.receive_message(stream, current_time);
			}
			break;

			case message_type::stream_ack:
			{
				if (stream.size() == 2)
				{
					_stream_messenger.receive_ack(stream.fast_read<uint8_t>(), current_time);
				}
			}
			break;

			case message_type::reliable:
			{
				_reliable_messenger.receive_message(stream, current_time);
			}
			break;

			case message_type::reliable_ack:
			{
				if (stream.size() == 4)
				{
					uint8_t _remote_low_n_received = stream.fast_read<uint8_t>();
					uint16_t _remote_messages_received = stream.fast_read<uint16_t>();

					_reliable_messenger.receive_ack(
						_remote_low_n_received,
						_remote_messages_received,
						current_time
						);
				}
			}
			break;

			case message_type::unreliable:
			{
				_session->_handler->on_message_received(
					bit_stream(stream.seek(), stream.size() - stream.tell()),
					_remote_uuid
					);
			}
			break;
			}

		}

		void send_unreliable(const char* buffer, const uint32_t length)
		{
			_session->_socket.send(buffer, length, _remote_address);
		}
		void send_stream(const char* buffer, const uint32_t length)
		{
			_stream_messenger.send(buffer, length);
		}
		void send_reliable(const char* buffer, const uint32_t length)
		{
			_reliable_messenger.send(buffer, length);
		}

		void update(uint64_t current_time)
		{
			uint64_t time_since_last_ping = current_time - _last_ping_time;
			uint64_t time_since_last_stream_ack = current_time - _stream_messenger.last_ack_time();
			uint64_t time_since_last_reliable_ack = current_time - _reliable_messenger.last_ack_time();

			// disconnect from the remote if we haven't gotten an acknowledgment in a while

			if (time_since_last_stream_ack > network_session::timeout_time &&
				time_since_last_reliable_ack > network_session::timeout_time)
			{
				_disconnected = true;
				return;
			}

			// update stream messaging

			_stream_messenger.update(current_time);

			// update reliable messaging

			_reliable_messenger.update(current_time);

			// ping the remote if we haven't pinged them in a while

			if (time_since_last_ping > network_session::ping_time)
			{
				_last_ping_time = current_time;

				char ping_message[5];
				bit_stream stream(ping_message, 5);

				stream.fast_write<uint8_t>(message_type::ping);
				stream.fast_write<uint8_t>(_stream_messenger.local_low_n_received());
				stream.fast_write<uint8_t>(_reliable_messenger.local_low_n_received());
				stream.fast_write<uint16_t>(_reliable_messenger.local_messages_received());

				_session->_socket.send(ping_message, 5, _remote_address);
			}
		}

	private:

		class stream_messenger
		{
		public:
			stream_messenger() :
				_session(nullptr),
				_connection(nullptr),
				_local_low_n_sent(0),
				_local_low_n_received(0),
				_remote_low_n_received(0),
				_last_ack_time(0),
				_last_resend_time(0) { }

			static const uint32_t maximum_sequence_number = 255;
			static const uint32_t window_size = 16;

			static uint32_t modulus_distance(uint8_t leading_number, uint8_t trailing_number)
			{
				if (leading_number < trailing_number)
				{
					return 1 + leading_number + (stream_messenger::maximum_sequence_number - trailing_number);
				}
				else
				{
					return leading_number - trailing_number;
				}
			}

			uint8_t local_low_n_sent() const { return _local_low_n_sent; }
			uint8_t local_low_n_received() const { return _local_low_n_received; }
			uint64_t last_ack_time() const { return _last_ack_time; }

			void set_connection(network_session::connection* connection) { _connection = connection; }
			void set_session(network_session* session) { _session = session; }

			void create(network_session* session, network_session::connection* connection)
			{
				_session = session;
				_connection = connection;

				_local_low_n_sent = 0;
				_local_low_n_received = 0;

				_remote_low_n_received = 0;

				uint64_t current_time = session->_timer.get_microseconds();
				_last_ack_time = current_time;
				_last_resend_time = current_time;

				_allocator.create(4000);

				for (uint32_t i = 0; i < window_size; ++i)
				{
					_window[i].buffer = nullptr;
					_window[i].buffer_length = 0;
				}

				while (!_queue.empty())
				{
					_queue.pop();
				}
			}

			void receive_ack(uint8_t new_rnd, uint64_t current_time)
			{
				// ensure the new_rnd has either remained the same or acknowledged some packets

				uint32_t dist_old_rnd = modulus_distance(_local_low_n_sent, _remote_low_n_received);
				uint32_t dist_new_rnd = modulus_distance(_local_low_n_sent, new_rnd);

				if (dist_new_rnd <= dist_old_rnd)
				{
					_last_ack_time = current_time;

					if (new_rnd < _remote_low_n_received)
					{
						for (uint32_t i = _remote_low_n_received; i <= stream_messenger::maximum_sequence_number; ++i)
						{
							uint32_t message_index = i % stream_messenger::window_size;

							_allocator.pop_front();
							_window[message_index] = packet();
						}
						for (uint32_t i = 0; i < new_rnd; ++i)
						{
							uint32_t message_index = i % stream_messenger::window_size;

							_allocator.pop_front();
							_window[message_index] = packet();
						}
					}
					else
					{
						for (uint32_t i = _remote_low_n_received; i < new_rnd; ++i)
						{
							uint32_t message_index = i % stream_messenger::window_size;

							_allocator.pop_front();
							_window[message_index] = packet();
						}
					}

					_remote_low_n_received = new_rnd;
				}
			}
			void receive_message(bit_stream& stream, uint64_t current_time)
			{
				if (stream.size() >= 3)
				{
					uint8_t message_id = stream.fast_read<uint8_t>();

					receive_ack(stream.fast_read<uint8_t>(), current_time);

					if (message_id == _local_low_n_received)
					{
						++_local_low_n_received;
						_session->_handler->on_message_received(
							bit_stream(stream.seek(), stream.size() - stream.tell()),
							_connection->_remote_uuid
							);

						char reliable_ack[2];
						stream.attach(reliable_ack, 2);
						stream.fast_write<uint8_t>(message_type::stream_ack);
						stream.fast_write<uint8_t>(_local_low_n_received);

						_session->_socket.send(reliable_ack, 2, _connection->_remote_address);
					}
				}
			}

			void send(const char* buffer, const uint32_t length)
			{
				packet p;
				p.buffer_length = length + 3;
				p.buffer = _allocator.push_back(p.buffer_length);

				memcpy(p.buffer + 3, buffer, length);

				_queue.push(p);
			}

			void update(uint64_t current_time)
			{
				bit_stream stream;
				while (!_queue.empty() && modulus_distance(_local_low_n_sent, _remote_low_n_received) < stream_messenger::window_size)
				{
					// reset the resend time on the connection because we are sending a message

					_last_resend_time = current_time;

					// move the queued message to the window

					uint32_t message_index = _local_low_n_sent % stream_messenger::window_size;

					_window[message_index] = _queue.front();
					_queue.pop();

					// write the packet header and send it

					stream.attach(
						_window[message_index].buffer,
						_window[message_index].buffer_length
						);

					stream.fast_write<uint8_t>(message_type::stream);
					stream.fast_write<uint8_t>(_local_low_n_sent);
					stream.fast_write<uint8_t>(_local_low_n_received);

					_session->_socket.send(
						_window[message_index].buffer,
						_window[message_index].buffer_length,
						_connection->_remote_address
						);

					// advance the window forward, let it wrap around

					++_local_low_n_sent;
				}

				uint64_t time_since_last_resend = current_time - _last_resend_time;

				// resend messages if we have unacknowledged messages and haven't sent a reliable message in a while

				if (
					time_since_last_resend > network_session::resend_time &&
					modulus_distance(_local_low_n_sent, _remote_low_n_received) > 0
					)
				{
					_last_resend_time = current_time;

					if (_local_low_n_sent < _remote_low_n_received)
					{
						for (uint32_t i = _remote_low_n_received; i <= stream_messenger::maximum_sequence_number; ++i)
						{
							resend_message(i);
						}
						for (uint32_t i = 0; i < _local_low_n_sent; ++i)
						{
							resend_message(i);
						}
					}
					else
					{
						for (uint32_t i = _remote_low_n_received; i < _local_low_n_sent; ++i)
						{
							resend_message(i);
						}
					}
				}
			}

		private:
			
			void resend_message(uint32_t seq)
			{
				uint32_t message_index = seq % stream_messenger::window_size;

				// we need to update the next desired field of the header, it may have changed

				bit_stream stream = _window[message_index].get_stream();
				stream.skip(2);
				stream.fast_write<uint8_t>(_local_low_n_received);

				_session->_socket.send(
					_window[message_index].buffer,
					_window[message_index].buffer_length,
					_connection->_remote_address
					);
			}

			network_session*				_session;
			network_session::connection*	_connection;

			uint8_t		_local_low_n_sent;
			uint8_t		_local_low_n_received;
			uint8_t		_remote_low_n_received;

			uint64_t _last_ack_time;
			uint64_t _last_resend_time;

			circular_allocator	_allocator;

			packet				_window[window_size];
			std::queue<packet>	_queue;
		};

		class reliable_messenger
		{
		public:
			reliable_messenger() :
				_session(nullptr),
				_connection(nullptr),
				_local_low_n_sent(0),
				_local_low_n_received(0),
				_local_messages_received(0),
				_remote_low_n_received(0),
				_remote_messages_received(0),
				_last_ack_time(0),
				_last_resend_time(0) { }

			static const uint32_t maximum_sequence_number = 255;
			static const uint32_t window_size = 16;

			static uint32_t modulus_distance(uint8_t leading_number, uint8_t trailing_number)
			{
				if (leading_number < trailing_number)
				{
					return 1 + leading_number + (reliable_messenger::maximum_sequence_number - trailing_number);
				}
				else
				{
					return leading_number - trailing_number;
				}
			}

			uint8_t local_low_n_sent() const { return _local_low_n_sent; }
			uint8_t local_low_n_received() const { return _local_low_n_received; }
			uint16_t local_messages_received() const { return _local_messages_received; }
			uint64_t last_ack_time() const { return _last_ack_time; }

			void set_connection(network_session::connection* connection) { _connection = connection; }
			void set_session(network_session* session) { _session = session; }

			void create(network_session* session, network_session::connection* connection)
			{
				_session = session;
				_connection = connection;

				_local_low_n_sent = 0;
				_local_low_n_received = 0;
				_local_messages_received = 0;

				_remote_low_n_received = 0;
				_remote_messages_received = 0;

				uint64_t current_time = session->_timer.get_microseconds();
				_last_ack_time = current_time;
				_last_resend_time = current_time;

				_allocator.create(4000);

				for (uint32_t i = 0; i < window_size; ++i)
				{
					_window[i].buffer = nullptr;
					_window[i].buffer_length = 0;
				}

				while (!_queue.empty())
				{
					_queue.pop();
				}
			}

			void receive_ack(uint8_t new_rnd, uint16_t new_status, uint64_t current_time)
			{
				// ensure the new_rnd has either remained the same or acknowledged some packets

				uint32_t dist_old_rnd = modulus_distance(_local_low_n_sent, _remote_low_n_received);
				uint32_t dist_new_rnd = modulus_distance(_local_low_n_sent, new_rnd);

				if (dist_new_rnd <= dist_old_rnd)
				{
					_last_ack_time = current_time;

					if (new_rnd < _remote_low_n_received)
					{
						for (uint32_t i = _remote_low_n_received; i <= reliable_messenger::maximum_sequence_number; ++i)
						{
							uint32_t message_index = i % reliable_messenger::window_size;

							_allocator.pop_front();
							_window[message_index] = packet();
						}
						for (uint32_t i = 0; i < new_rnd; ++i)
						{
							uint32_t message_index = i % reliable_messenger::window_size;

							_allocator.pop_front();
							_window[message_index] = packet();
						}
					}
					else
					{
						for (uint32_t i = _remote_low_n_received; i < new_rnd; ++i)
						{
							uint32_t message_index = i % reliable_messenger::window_size;

							_allocator.pop_front();
							_window[message_index] = packet();
						}
					}

					_remote_messages_received = new_status;
					_remote_low_n_received = new_rnd;
				}
			}
			void receive_message(bit_stream& stream, uint64_t current_time)
			{
				if (stream.size() >= 5)
				{
					uint8_t message_id = stream.fast_read<uint8_t>();

					uint8_t _remote_low_n_received = stream.fast_read<uint8_t>();
					uint16_t _remote_messages_received = stream.fast_read<uint16_t>();

					receive_ack(_remote_low_n_received, _remote_messages_received, current_time);

					uint32_t message_index = modulus_distance(message_id, _local_low_n_received);
					uint32_t message_flag = 1 << message_index;

					if (
						message_index < reliable_messenger::window_size
						&& !(_local_messages_received & message_flag)
						)
					{
						_local_messages_received |= message_flag;

						while (_local_messages_received & 1)
						{
							++_local_low_n_received;
							_local_messages_received = _local_messages_received >> 1;
						}

						_session->_handler->on_message_received(
							bit_stream(stream.seek(), stream.size() - stream.tell()),
							_connection->_remote_uuid
							);

						char reliable_ack[4];
						stream.attach(reliable_ack, 4);
						stream.fast_write<uint8_t>(message_type::reliable_ack);
						stream.fast_write<uint8_t>(_local_low_n_received);
						stream.fast_write<uint16_t>(_local_messages_received);

						_session->_socket.send(reliable_ack, 4, _connection->_remote_address);
					}
				}
			}

			void send(const char* buffer, const uint32_t length)
			{
				packet p;
				p.buffer_length = length + 5;
				p.buffer = _allocator.push_back(p.buffer_length);

				memcpy(p.buffer + 5, buffer, length);

				_queue.push(p);
			}

			void update(uint64_t current_time)
			{
				bit_stream reliable;
				while (!_queue.empty() && modulus_distance(_local_low_n_sent, _remote_low_n_received) < reliable_messenger::window_size)
				{
					// reset the resend time on the connection because we are sending a message

					_last_resend_time = current_time;

					// move the queued message to the window

					uint32_t message_index = _local_low_n_sent % reliable_messenger::window_size;

					_window[message_index] = _queue.front();
					_queue.pop();

					// write the packet header and send it

					reliable.attach(
						_window[message_index].buffer,
						_window[message_index].buffer_length
						);

					reliable.fast_write<uint8_t>(message_type::reliable);
					reliable.fast_write<uint8_t>(_local_low_n_sent);
					reliable.fast_write<uint8_t>(_local_low_n_received);
					reliable.fast_write<uint16_t>(_local_messages_received);

					_session->_socket.send(
						_window[message_index].buffer,
						_window[message_index].buffer_length,
						_connection->_remote_address
						);

					// advance the window forward, let it wrap around

					++_local_low_n_sent;
				}

				uint64_t time_since_last_resend = current_time - _last_resend_time;

				// resend messages if we have unacknowledged messages and haven't sent a reliable message in a while

				if (
					time_since_last_resend > network_session::resend_time &&
					modulus_distance(_local_low_n_sent, _remote_low_n_received) > 0
					)
				{
					_last_resend_time = current_time;
					uint32_t message_flag = 1;

					if (_local_low_n_sent < _remote_low_n_received)
					{
						for (uint32_t i = _remote_low_n_received; i <= reliable_messenger::maximum_sequence_number; ++i)
						{
							if (!(_remote_messages_received & message_flag))
							{
								resend_message(i);
							}
							message_flag = message_flag << 1;
						}
						for (uint32_t i = 0; i < _local_low_n_sent; ++i)
						{
							if (!(_remote_messages_received & message_flag))
							{
								resend_message(i);
							}
							message_flag = message_flag << 1;
						}
					}
					else
					{
						for (uint32_t i = _remote_low_n_received; i < _local_low_n_sent; ++i)
						{
							if (!(_remote_messages_received & message_flag))
							{
								resend_message(i);
							}
							message_flag = message_flag << 1;
						}
					}
				}
			}

		private:

			void resend_message(uint32_t seq)
			{
				uint32_t message_index = seq % reliable_messenger::window_size;

				// we need to update the next desired field of the header, it may have changed

				bit_stream reliable = _window[message_index].get_stream();
				reliable.skip(2);
				reliable.fast_write<uint8_t>(_local_low_n_received);
				reliable.fast_write<uint16_t>(_local_messages_received);

				_session->_socket.send(
					_window[message_index].buffer,
					_window[message_index].buffer_length,
					_connection->_remote_address
					);
			}

			network_session*				_session;
			network_session::connection*	_connection;

			uint8_t		_local_low_n_sent;
			uint8_t		_local_low_n_received;
			uint16_t	_local_messages_received;

			uint8_t		_remote_low_n_received;
			uint16_t	_remote_messages_received;

			uint64_t _last_ack_time;
			uint64_t _last_resend_time;

			circular_allocator	_allocator;

			packet				_window[window_size];
			std::queue<packet>	_queue;
		};

		network_session*	_session;

		ip_address			_remote_address;
		uuid				_remote_uuid;

		uint64_t			_last_ping_time;

		stream_messenger	_stream_messenger;
		reliable_messenger	_reliable_messenger;

		bool				_disconnected;

		static void swap(connection& a, connection& b)
		{
			std::swap(a._session, b._session);
			std::swap(a._remote_address, b._remote_address);
			std::swap(a._remote_uuid, b._remote_uuid);
			std::swap(a._last_ping_time, b._last_ping_time);
			std::swap(a._stream_messenger, b._stream_messenger);
			std::swap(a._reliable_messenger, b._reliable_messenger);
			std::swap(a._disconnected, b._disconnected);

			a._stream_messenger.set_connection(&a);
			b._stream_messenger.set_connection(&b);

			a._reliable_messenger.set_connection(&a);
			b._reliable_messenger.set_connection(&b);
		}
	};

	// properties

	uuid					_uuid;
	uint32_t				_password;

	uint32_t				_max_connections;
	std::vector<connection>	_connections;

	network_session_handler*	_handler;
	network_timer				_timer;
	udp_socket					_socket;
	packet						_receive_packet;

	// functions

	connection* find_connection(const ip_address& addr);
	connection* find_connection(const uuid& id);

	void update_connections()
	{
		auto con = _connections.begin();
		auto end = _connections.end();

		while (con != end)
		{
			uint64_t current_time = _timer.get_microseconds();

			con->update(current_time);

			++con;
		}

		// prune out the recently disconnected connections

		bool checked_every_connection = false;
		while (!checked_every_connection)
		{
			bool found_disconnected = false;

			auto con = _connections.begin();
			auto end = _connections.end();

			while (con != end)
			{
				if (con->is_disconnected())
				{
					_handler->on_peer_disconnected(con->remote_uuid());
					_connections.erase(con);
					found_disconnected = true;
					break;
				}
				++con;
			}

			if (!found_disconnected)
				checked_every_connection = true;
		}
	}

	void receive_packets()
	{
		ip_address incoming_address;

		while (
			_socket.try_receive(
			_receive_packet.buffer,
			network_session::maximum_transmission_unit,
			&_receive_packet.buffer_length,
			&incoming_address)
			)
		{
			connection* con = find_connection(incoming_address);

			if (con != nullptr)
			{
				con->receive_message(&_receive_packet, _timer.get_microseconds());

				if (con->is_disconnected())
				{
					_handler->on_peer_disconnected(con->remote_uuid());

					_connections.erase(
						_connections.begin() + (con - _connections.data())
						);
				}
			}
			else
			{
				handle_unconnected_packet(&_receive_packet, incoming_address);
			}
		}
	}
	void handle_unconnected_packet(packet* msg, const ip_address& remote_addr)
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
				else if (password != _password)
				{
					result = connection_result_invalid_password;
				}
				else if (_connections.size() >= _max_connections)
				{
					result = connection_result_server_full;
				}

				if (result == connection_result_succeeded)
				{
					char connection_accepted_response[17];

					stream.attach(connection_accepted_response, 17);
					stream.fast_write<uint8_t>(message_type::connection_accepted);
					stream.fast_write<uuid>(_uuid);
					_socket.send(connection_accepted_response, 17, remote_addr);

					_connections.push_back(connection());
					_connections.back().create(this, remote_addr, remote_uuid);
					_handler->on_peer_joined(remote_uuid);
				}
				else
				{
					char connection_rejected_response[5];

					stream.attach(connection_rejected_response, 5);
					stream.fast_write<uint8_t>(message_type::connection_rejected);
					stream.fast_write<uint32_t>(result);

					_socket.send(connection_rejected_response, 5, remote_addr);
				}
			}
		}
		break;
		case message_type::connection_accepted:
		{
			if (stream.size() == 17)
			{
				uuid remote_uuid = stream.fast_read<uuid>();

				_connections.push_back(connection());
				_connections.back().create(this, remote_addr, remote_uuid);
				_handler->on_peer_joined(remote_uuid);

				_handler->connect_result_handler(remote_uuid, true, 0);
			}
		}
		break;
		case message_type::connection_rejected:
		{
			if (stream.size() == 5)
			{
				uint32_t reason = stream.fast_read<uint32_t>();
				_handler->connect_result_handler(uuid(), false, reason);
			}
		}
		break;

		case message_type::query:
		{
			char query_response[14];

			stream.attach(query_response, 14);

			stream.fast_write<uint8_t>(message_type::query_response);
			stream.fast_write<uint32_t>(network_session::protocol_version);
			stream.fast_write<uint32_t>(_connections.size());
			stream.fast_write<uint32_t>(_max_connections);
			stream.fast_write<uint8_t>(_password == 0 ? 0 : 1);

			_socket.send(query_response, 14, remote_addr);
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

				_handler->query_result_handler(
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
	
};

#endif