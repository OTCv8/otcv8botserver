  
#include <App.h>
#include "json.hpp"

#include <chrono>
#include <mutex>
#include <algorithm>
#include <thread>
#include <iostream>
#include <map>
#include <set>
#include <atomic>
#include <ctime>

const int THREADS = 2;
const int PORT = 8000;
const bool SSL = false;

using ChannelsMap = std::map<std::string, std::set<uWS::WebSocket<SSL, true>*>>;
using StringPtr = std::shared_ptr<std::string>;

struct PerSocketData {
    std::string name;
    std::string channel;
    int64_t lastPing = 0;
    int64_t lastPingSent = 0;
    int64_t packets = 0;
    time_t packetsTime = 0;
};

struct PerThreadData {
    uWS::Loop* loop = nullptr;
    ChannelsMap* channels = nullptr;
};
    
std::atomic<int64_t> connections = 0;
std::atomic<int64_t> exceptions = 0;
std::atomic<int64_t> blocked = 0;
std::atomic<int64_t> packets = 0;
std::atomic<int64_t> messageId = 0;
std::vector<std::mutex> mutexes(THREADS);
std::vector<std::thread *> threads(THREADS, nullptr);
std::vector<PerThreadData> threads_data(THREADS);

std::mutex globalMutex;
std::map<std::string, std::multiset<std::string>> globalChannels;

int64_t millis()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void dispatchMessage(const StringPtr& channel, const StringPtr& message)
{
    for(int index = 0; index < THREADS; ++index) {    
        std::lock_guard<std::mutex> lock(mutexes[index]);
        if(!threads_data[index].loop) {
            continue;
        }

        threads_data[index].loop->defer([index, channel, message] {
            auto it = threads_data[index].channels->find(*channel);
            if(it != threads_data[index].channels->end()) {
                for(auto& ws : it->second) {
                    ws->send(*message, uWS::TEXT);
                }
            }
        });
    }        
}

void sendPing()
{
    static std::string pingStr = "{\"type\":\"ping\",\"ping\":";
    for(int index = 0; index < THREADS; ++index) {    
        std::lock_guard<std::mutex> lock(mutexes[index]);
        if(!threads_data[index].loop) {
            continue;
        }

        threads_data[index].loop->defer([index] {
            for(auto& channel : *(threads_data[index].channels)) {
                for(auto& ws : channel.second) {
                    PerSocketData* userData = (PerSocketData*)(ws->getUserData());
                    userData->lastPingSent = millis();
                    ws->send(pingStr + std::to_string(userData->lastPing) + "}", uWS::TEXT);                    
                }
            }
        });
    }       
}

void processMessage(uWS::WebSocket<SSL, true>* ws, std::string_view& raw_message, ChannelsMap* channels) 
{
    packets += 1;
    PerSocketData* userData = (PerSocketData*)(ws->getUserData());
    auto msg = nlohmann::json::parse(raw_message);
    std::string type = msg["type"];

    if(userData->name.empty() || userData->channel.empty()) {
        if(type != "init") {
            ws->end();
            return;
        }
        
        userData->name = msg["name"];
        userData->channel = msg["channel"];
        userData->lastPing = 0;
        userData->lastPingSent = millis();

        if(userData->name.empty() || userData->channel.empty() || userData->name.size() > 30 || userData->channel.size() > 30) {
            blocked += 1;
            ws->end();
            return;
        }

        auto it = channels->find(userData->channel);
        if(it != channels->end() && it->second.size() > 50) {
            blocked += 1;
            ws->end();
            return;
        } 
        
        (*channels)[userData->channel].insert(ws);
        globalMutex.lock();
        globalChannels[userData->channel].insert(userData->name);
        globalMutex.unlock();
        return;
    }

    if(userData->packetsTime < time(nullptr)) {
        userData->packetsTime = time(nullptr) + 1;
        userData->packets = 0;
    }
    userData->packets += 1;
    // 100 packets per 2s
    if(userData->packets > 100 || raw_message.size() > 8 * 1024) {
        blocked += 1;
        ws->end();
        return;                                
    }

    if(type == "ping") {
        userData->lastPing = millis() - userData->lastPingSent;                        
        return;
    }

    if(type != "message") {
        ws->end();
        return;                        
    }

    std::string topic = msg["topic"];
    if(topic.empty() || topic.size() > 30) {
        ws->end();
        return;                                
    }

    nlohmann::json response;
    response["type"] = "message";
    response["id"] = ++messageId;
    response["name"] = userData->name;
    response["topic"] = topic;
    
    // special topics
    if(topic == "list") { // list of all users
        std::set<std::string> users;
        globalMutex.lock();
        auto it = globalChannels.find(userData->channel);
        if(it != globalChannels.end()) {
            users = std::set<std::string>(it->second.begin(), it->second.end());
        }
        globalMutex.unlock();
        response["message"] = users;
        ws->send(response.dump(), uWS::TEXT);
        return;
    }
    
    // relay message
    response["message"] = msg["message"];
    dispatchMessage(std::make_shared<std::string>(userData->channel), std::make_shared<std::string>(response.dump()));    
}

int main() 
{    
    std::cout << "Starting otcv8 bot server on port " << PORT << " using " << THREADS << " threads..." << std::endl;
    
    for(int index = 0; index < THREADS; ++index) {    
        threads[index] = new std::thread([index] {
            ChannelsMap* channels = new ChannelsMap();

            mutexes[index].lock();
            threads_data[index].loop = uWS::Loop::get();
            threads_data[index].channels = channels;
            mutexes[index].unlock();            
            
            uWS::TemplatedApp<SSL>().ws<PerSocketData>("/*", {
                .compression = uWS::SHARED_COMPRESSOR,
                .maxPayloadLength = 64 * 1024,
                .idleTimeout = 12,
                .maxBackpressure = 256 * 1024,
                .upgrade = nullptr,
                .open = [](uWS::WebSocket<SSL, true> *ws) {
                    connections += 1;
                },
                .message = [&channels](auto *ws, std::string_view message, uWS::OpCode opCode) {
                    if(opCode != uWS::TEXT || message.size() > 64 * 1024) {
                        return;
                    }
                    
                    try {
                        processMessage(ws, message, channels);
                    } catch(...) {
                        exceptions += 1;
                        ws->end();
                    }
                },
                .drain = nullptr,
                .ping = nullptr,
                .pong = nullptr,                
                .close = [&channels](uWS::WebSocket<SSL, true> *ws, int code, std::string_view message) {
                    std::string& name = ((PerSocketData*)(ws->getUserData()))->name;
                    std::string& channel = ((PerSocketData*)(ws->getUserData()))->channel;
                    if(!channel.empty()) {
                        auto it = channels->find(channel);
                        if(it != channels->end()) {
                            it->second.erase(ws);
                            if(it->second.empty()) {
                                channels->erase(it);
                            }
                        }
                    }
                    globalMutex.lock();
                    auto it = globalChannels.find(channel);
                    if(it != globalChannels.end()) {
                        auto uit = it->second.find(name);
                        if(uit != it->second.end()) {
                            it->second.erase(uit);
                        }
                        if(it->second.empty()) {
                            globalChannels.erase(it);
                        }
                    }                    
                    globalMutex.unlock();
                    connections -= 1;
                }
            }).listen(PORT, [index](auto *token) {
                if (!token) {
                    std::cout << "Thread " << index << " failed to listen on port " << PORT << std::endl;
                }
            }).run();
            
            mutexes[index].lock();
            threads_data[index].loop = nullptr;
            threads_data[index].channels = nullptr;
            delete channels;
            mutexes[index].unlock();
        });
    };
    
    std::this_thread::sleep_for(std::chrono::seconds(1)); // give it 1s to start
    std::cout << "Server online!" << std::endl;

    bool working = true;
    while(working) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "Connections: " << connections << " Packets: " << packets << " Exceptions: " << exceptions << " Blocked: " << blocked << std::endl;
        
        // send ping
        sendPing();
        
        working = false;
        for(int index = 0; index < THREADS; ++index) {
            std::lock_guard<std::mutex> lock(mutexes[index]);
            if(!threads_data[index].loop) {
                if(threads[index]) {
                    threads[index]->join();
                    delete threads[index];
                    threads[index] = nullptr;
                }
                continue;
            }
            working = true;
        }        
    }
    return 0;
}
