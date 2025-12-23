# Web interface to [MTR](https://www.bitwizard.nl/mtr/)

This project wraps MTR through a websockets interface, and a web frontend to display the trace.

## Backend

There are C and Python backends.  Either one can be used.

### C

Requires [jansson](https://github.com/akheron/jansson) and [libwebsockets](https://libwebsockets.org).

```
make
./backend
```

### Python

Requires `asyncio`, `aiohttp`, and `aiohttp_core`.

```
./backend.py
```

## Frontend

Browse to `http://localhost:8080`

