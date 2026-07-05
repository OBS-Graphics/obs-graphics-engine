# Third-Party Licenses

## Cairo
License: GNU Lesser General Public License v2.1
https://cairographics.org/

## Pango
License: GNU Lesser General Public License v2.0+
https://pango.gnome.org/

## HarfBuzz
License: MIT ("Old MIT")
https://github.com/harfbuzz/harfbuzz

## Lua 5.4
License: MIT
https://www.lua.org/

## sol2
License: MIT
https://github.com/ThePhD/sol2

## curl
License: curl license (MIT/X derivative)
https://github.com/curl/curl
Statically linked into `engine` as the HTTP(S) client backing the Lua `http.*` table (lua_http.cpp); only built when `ENABLE_LUA_SCRIPTING` is on.

## OpenSSL
License: Apache License 2.0
https://www.openssl.org/
TLS backend for curl. System-installed on Linux/macOS; installed via vcpkg on Windows — not vendored through CPM.

## cpp-httplib
License: MIT
https://github.com/yhirose/cpp-httplib
Test-only dependency (local mock HTTP server in tests/test_script_http.cpp); not linked into `engine`.

## nlohmann/json
License: MIT
https://github.com/nlohmann/json

## stb (stb_image)
License: MIT or Public Domain (dual-licensed, your choice)
https://github.com/nothings/stb

## qr.hpp
Adapted from https://github.com/nth-eye/qr
License: BSD-3-Clause
