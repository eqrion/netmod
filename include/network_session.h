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
	static const uint32_t maximum_transmission_unit = 800;

	static const uint32_t resend_time = 100000;
	static const uint32_t ping_time = 1000000;
	static const uint32_t timeout_time = 10000000;

	static const uint32_t protocol_version = 0x33366999;

	network_session();
	~network_session();

	network_session(const network_session& rhs) = delete;
	network_session(network_session&& rhs) = delete;
	network_session& operator=(const network_session&) = delete;
	network_session& operator=(network_session&&) = delete;

	bool create(
		const char* port_number,
		uint32_t password,
		uint32_t max_connections,
		network_session_handler* handler,
		size_t stream_packet_queue_buffer_size = 4000,
		size_t reliable_packet_queue_buffer_size = 4000,
		bool drop_packets = false
		);
	void destroy();

	void send_unreliable(const char* buffer, const uint32_t length, uuid id);
	void send_reliable(const char* buffer, const uint32_t length, uuid id);
	void send_stream(const char* buffer, const uint32_t length, uuid id);
	
	void update();

	void query(const ip_address& addr);

	void try_connect(const ip_address& addr, uint32_t password);
	void disconnect(uuid id);

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
		connection();
		connection(const connection& rhs);
		connection(connection&& rhs);
		const connection& operator=(connection rhs);

		const ip_address& remote_address() const { return _remote_address; }
		const uuid& remote_uuid() const { return _remote_uuid; }
		bool is_disconnected() const { return _disconnected; }

		void create(network_session* session, const ip_address& remote_address, const uuid& remote_uuid, size_t stream_packet_queue_buffer_size = 4000, size_t reliable_packet_queue_buffer_size = 4000);

		void receive_message(packet* msg, uint64_t current_time);

		void send_unreliable(const char* buffer, const uint32_t length);
		void send_stream(const char* buffer, const uint32_t length);
		void send_reliable(const char* buffer, const uint32_t length);

		void update(uint64_t current_time);

	private:
		static void swap(connection& a, connection& b);

		class stream_messenger
		{
		public:
			stream_messenger();

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

			void create(network_session* session, network_session::connection* connection, size_t packet_queue_buffer_size);
			void receive_ack(uint8_t new_rnd, uint64_t current_time);
			void receive_message(bit_stream& stream, uint64_t current_time);
			void send(const char* buffer, const uint32_t length);
			void update(uint64_t current_time);

		private:
			
			void resend_message(uint32_t seq);

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
			reliable_messenger();

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

			void create(network_session* session, network_session::connection* connection, size_t packet_queue_buffer_size);
			void receive_ack(uint8_t new_rnd, uint16_t new_status, uint64_t current_time);
			void receive_message(bit_stream& stream, uint64_t current_time);
			void send(const char* buffer, const uint32_t length);
			void update(uint64_t current_time);

		private:

			void resend_message(uint32_t seq);

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
	};

	// properties

	uuid					_uuid;
	uint32_t				_password;

	uint32_t				_max_connections;
	std::vector<connection>	_connections;
	size_t					_stream_packet_queue_buffer_size;
	size_t					_reliable_packet_queue_buffer_size;

	network_session_handler*	_handler;
	network_timer				_timer;
	udp_socket					_socket;
	packet						_receive_packet;

	// functions

	connection* find_connection(const ip_address& addr);
	connection* find_connection(const uuid& id);

	void update_connections();

	void receive_packets();
	void handle_unconnected_packet(packet* msg, const ip_address& remote_addr);
	
};

#endif