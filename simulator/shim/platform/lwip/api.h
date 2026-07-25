/**
 * Stand-in for lwIP's api.h.
 *
 * The simulator serves the HTTP API over the host's own sockets, so lwIP is
 * not built. discovery.h includes this header for the netconn API it does not
 * expose in its own interface, so nothing here needs a definition.
 */
#pragma once
