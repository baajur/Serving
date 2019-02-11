#ifndef BAIDU_PADDLE_SERVING_PREDICTOR_SERVER_H
#define BAIDU_PADDLE_SERVING_PREDICTOR_SERVER_H

#include "common/inner_common.h"

namespace baidu {
namespace paddle_serving {
namespace predictor {

class ServerManager {
public: 
    typedef google::protobuf::Service Service;
    ServerManager();

    static ServerManager& instance() {
        static ServerManager server; 
        return server;
    }

    static bool reload_starting() {
        return _s_reload_starting;
    }

    static void stop_reloader() {
        _s_reload_starting = false;
    }

    int add_service_by_format(const std::string& format);

    int start_and_wait();

private:
    int _start_reloader();
    
    int _wait_reloader();

    static void* _reload_worker(void* args);
    
    bool _compare_string_piece_without_case(
            const base::StringPiece& s1, const char* s2);

    void _set_server_option_by_protocol(const ::base::StringPiece& protocol_type);

    baidu::rpc::ServerOptions _options;
    baidu::rpc::Server _server;
    boost::unordered_map<std::string, Service*> _format_services;
    THREAD_T _reload_thread;
    static volatile bool _s_reload_starting;
};

} // predictor
} // paddle_serving
} // baidu

#endif
