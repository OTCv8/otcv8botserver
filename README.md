# Websocket bot server for otclientv8
# Version: 1.0

### DISCORD: https://discord.gg/feySup6
### Forum: http://otclient.net

Requirements:
- C++17 compiler
- libz-dev
- https://github.com/uNetworking/uWebSockets
- https://github.com/uNetworking/uSockets
- https://github.com/nlohmann/json/blob/develop/single_include/nlohmann/json.hpp

### Compilation
```g++ main.cpp -std=c++17 -Ofast uWebSockets/uSockets/uSockets.a -IuWebSockets/src -IuWebSockets/uSockets/src -lz -lpthread -o botserver```

### Using it
If you want to use your bot server in bot, before calling BotServer.init set BotServer.url, for example
```BotServer.url = "ws://127.0.0.1:8000/"
BotServer.init(name(), "test_channel")```