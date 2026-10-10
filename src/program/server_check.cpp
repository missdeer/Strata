#include "strata/runtime/server_engine.hpp"
#include "strata/runtime/server_vision.hpp"
#include "strata/runtime/path.hpp"
#include "strata/runtime/server_metrics.hpp"
#include "strata/runtime/server_recovery.hpp"
#include <fstream>
#include <iostream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
using namespace strata::runtime;
int run(const std::vector<std::string> &args) {
    if (args.size() == 3 && args[1] == "--touch") {
        std::ofstream file(utf8_path(args[2]));
        file << "native helper";
        return file ? 0 : 2;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            auto request = parse_text_json(line);
            TextJson result;
            const auto op = request.value("op", "");
            if (op == "normalize_image") {
                auto bytes =
                    NativeVision::normalize(read_text(utf8_path(request.at("input").get<std::string>())));
                std::ofstream out(utf8_path(request.at("output").get<std::string>()), std::ios::binary);
                out.write(bytes.data(), bytes.size());
                if (!out)
                    throw std::runtime_error("could not write image");
                result = {{"bytes", bytes.size()}};
            } else if (op == "engine_args") {
                validate_server_config(request.at("config"));
                result = server_engine_args(request["config"]);
            } else if (op == "metrics")
                result = prometheus_metrics(request.at("metrics"), request.at("latencies"));
            else if (op == "reasoning_repeat")
                result = reasoning_repeat_coverage(request.at("text").get<std::string>());
            else if (op == "effort_prompt") {
                auto tok = Tokenizer::load(utf8_path(request.at("tokenizer").get<std::string>()));
                ChatTemplate chat(utf8_path(request.at("template").get<std::string>()));
                result = effort_prompt(openai_to_messages(request.at("request")), tok, chat, true);
            } else
                throw std::invalid_argument("unknown check operation");
            std::cout << TextJson{{"result", result}}.dump() << std::endl;
        } catch (const std::exception &error) {
            std::cout << TextJson{{"error", error.what()}}.dump() << std::endl;
        }
    }
    return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(path_utf8(std::filesystem::path(argv[i])));
    return run(args);
}
#else
int main(int argc, char **argv) { return run(std::vector<std::string>(argv, argv + argc)); }
#endif
