#include "include/network_session.h"

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