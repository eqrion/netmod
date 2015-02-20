#include "include/network_session.h"

network_session::connection::reliable_messenger::reliable_messenger() :
	_session(nullptr),
	_connection(nullptr),
	_local_low_n_sent(0),
	_local_low_n_received(0),
	_local_messages_received(0),
	_remote_low_n_received(0),
	_remote_messages_received(0),
	_last_ack_time(0),
	_last_resend_time(0) { }

void network_session::connection::reliable_messenger::create(network_session* session, network_session::connection* connection, size_t packet_queue_buffer_size)
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

	_allocator.create(packet_queue_buffer_size);

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

void network_session::connection::reliable_messenger::receive_ack(uint8_t new_rnd, uint16_t new_status, uint64_t current_time)
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
void network_session::connection::reliable_messenger::receive_message(bit_stream& stream, uint64_t current_time)
{
	if (stream.size() >= 5)
	{
		uint8_t message_id = stream.fast_read<uint8_t>();

		uint8_t remote_low_n_received = stream.fast_read<uint8_t>();
		uint16_t remote_messages_received = stream.fast_read<uint16_t>();

		receive_ack(remote_low_n_received, remote_messages_received, current_time);

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

void network_session::connection::reliable_messenger::send(const char* buffer, const uint32_t length)
{
	packet p;
	p.buffer_length = length + 5;
	p.buffer = _allocator.push_back(p.buffer_length);

	memcpy(p.buffer + 5, buffer, length);

	_queue.push(p);
}

void network_session::connection::reliable_messenger::update(uint64_t current_time)
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

void network_session::connection::reliable_messenger::resend_message(uint32_t seq)
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