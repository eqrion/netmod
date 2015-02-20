#include "include/network_session.h"

network_session::network_session() { }
network_session::~network_session()
{
	destroy();
}

bool network_session::create(
	const char* port_number,
	uint32_t password,
	uint32_t max_connections,
	network_session_handler* handler,
	size_t stream_packet_queue_buffer_size,
	size_t reliable_packet_queue_buffer_size,
	bool drop_packets
	)
{
	destroy();

	if (!_socket.create(port_number, drop_packets))
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

	_stream_packet_queue_buffer_size = stream_packet_queue_buffer_size;
	_reliable_packet_queue_buffer_size = reliable_packet_queue_buffer_size;

	_receive_packet.buffer = new char[network_session::maximum_transmission_unit];
	_receive_packet.buffer_length = network_session::maximum_transmission_unit;

	return true;
}
void network_session::destroy()
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

void network_session::send_unreliable(const char* buffer, const uint32_t length, uuid id)
{
	if (length > network_session::maximum_transmission_unit)
		return;

	connection* con = find_connection(id);

	if (con != nullptr)
	{
		con->send_unreliable(buffer, length);
	}
}
void network_session::send_reliable(const char* buffer, const uint32_t length, uuid id)
{
	if (length + 3 > network_session::maximum_transmission_unit)
		return;

	connection* con = find_connection(id);

	if (con != nullptr)
	{
		con->send_reliable(buffer, length);
	}
}
void network_session::send_stream(const char* buffer, const uint32_t length, uuid id)
{
	if (length + 3 > network_session::maximum_transmission_unit)
		return;

	connection* con = find_connection(id);

	if (con != nullptr)
	{
		con->send_stream(buffer, length);
	}
}

void network_session::update()
{
	receive_packets();
	update_connections();
}

void network_session::query(const ip_address& addr)
{
	char query_message[1];

	bit_stream stream(query_message, sizeof(query_message));

	stream.fast_write<uint8_t>(message_type::query);

	_socket.send(query_message, stream.size(), addr);
}

void network_session::try_connect(const ip_address& addr, uint32_t password)
{
	char connect_request_message[25];

	bit_stream stream(connect_request_message, sizeof(connect_request_message));

	stream.fast_write<uint8_t>(message_type::connection_request);
	stream.fast_write<uint32_t>(network_session::protocol_version);
	stream.fast_write<uint32_t>(password);
	stream.fast_write<uuid>(_uuid);

	_socket.send(connect_request_message, stream.size(), addr);
}
void network_session::disconnect(uuid id)
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

network_session::connection* network_session::find_connection(const ip_address& addr)
{
	auto iter = _connections.begin();
	auto end = _connections.end();

	while (iter != end)
	{
		if (iter->remote_address() == addr)
			return &*iter;

		++iter;
	}

	return nullptr;
}
network_session::connection* network_session::find_connection(const uuid& id)
{
	auto iter = _connections.begin();
	auto end = _connections.end();

	while (iter != end)
	{
		if (iter->remote_uuid() == id)
			return &*iter;

		++iter;
	}

	return nullptr;
}

void network_session::update_connections()
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

		auto conn = _connections.begin();
		auto endd = _connections.end();

		while (conn != endd)
		{
			if (conn->is_disconnected())
			{
				_handler->on_peer_disconnected(conn->remote_uuid());
				_connections.erase(conn);
				found_disconnected = true;
				break;
			}
			++conn;
		}

		if (!found_disconnected)
			checked_every_connection = true;
	}
}

void network_session::receive_packets()
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
void network_session::handle_unconnected_packet(packet* msg, const ip_address& remote_addr)
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
				_connections.back().create(this, remote_addr, remote_uuid, _stream_packet_queue_buffer_size, _reliable_packet_queue_buffer_size);
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
			_connections.back().create(this, remote_addr, remote_uuid, _stream_packet_queue_buffer_size, _reliable_packet_queue_buffer_size);
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