#include <cmath>
#include <string>
#include <memory>
#include <thread>
#include <getopt.h>
#include <filesystem>
#include <unordered_map>

#include <fmt/core.h>
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <matplotlib-cpp/matplotlibcpp.h>

#include "env.hpp"
#include "utils.hpp"
#include "vehicle.hpp"
#include "vehicle_base.hpp"
#include "planner.hpp"

using std::string;
namespace plt = matplotlibcpp;

static struct option long_options[] = {
    {"rounds", required_argument, 0, 'r'},
    {"output_path", required_argument, 0, 'o'},
    {"log_level", required_argument, 0, 'l'},
    {"config", required_argument, 0, 'c'},
    {"no_animation", no_argument, 0, 'n'},
    {"save_fig", no_argument, 0, 'f'},
};

std::unordered_map<std::string, spdlog::level::level_enum> LOG_LEVEL_DICT =
    {{"trace", spdlog::level::trace}, {"debug", spdlog::level::debug}, {"info", spdlog::level::info},
     {"warn", spdlog::level::warn}, {"err", spdlog::level::err}, {"critical", spdlog::level::critical}};

void run(int rounds_num, std::filesystem::path config_path,
    std::filesystem::path save_path, bool show_animation, bool save_fig) {
    YAML::Node config;
    spdlog::info(fmt::format("config path: {}", config_path.string()));
    try {
        config = YAML::LoadFile(config_path.string());
        // spdlog::info(fmt::format("config parameters:\n{}", YAML::Dump(config)));
    } catch (const YAML::Exception& e) {
        spdlog::error(fmt::format("Error parsing YAML file: {}", e.what()));
        return ;
    }

    // initialize
    double delta_t = config["delta_t"].as<double>();
    double max_simulation_time = config["max_simulation_time"].as<double>();
    double map_size = config["map_size"].as<double>();
    double lane_width = config["lane_width"].as<double>();

    std::shared_ptr<EnvCrossroads> env = std::make_shared<EnvCrossroads>(map_size, lane_width);
    VehicleBase::initialize(env, 5, 2, 8, 2.4);
    MonteCarloTreeSearch::initialize(config);
    Node::initialize(config["max_step"].as<int>(), MonteCarloTreeSearch::calc_cur_value);

    VehicleList vehicles;
    for (const auto& yaml_node : config["vehicle_list"]) {
        std::string vehicle_name = yaml_node.first.as<std::string>();
        std::shared_ptr<Vehicle> vehicle = std::make_shared<Vehicle>(vehicle_name, config);
        vehicles.push_back(vehicle);
    }

    uint64_t succeed_count = 0;
    for (uint64_t iter = 0; iter < rounds_num; ++iter) {
        vehicles.reset();

        spdlog::info(fmt::format("================== Round {} ==================", iter));
        for (auto vehicle : vehicles) {
            spdlog::info(fmt::format("{} >>> init_x: {:.2f}, init_y: {:.2f}, init_v: {:.2f}",
                                vehicle->name, vehicle->state.x, vehicle->state.y, vehicle->state.v));
        }

        double timestamp = 0.0;
        TicToc total_cost_time;
        while (true) {
            if (vehicles.is_all_get_target()) {
                spdlog::info(fmt::format(
                        "Round {:d} successed, simulation time: {:.3f} s, actual timecost: {:.3f} s",
                        iter, timestamp, total_cost_time.toc()));
                ++succeed_count;
                break;
            }

            if ( vehicles.is_any_collision() || timestamp > max_simulation_time) {
                spdlog::info(fmt::format(
                        "Round {:d} failed, simulation time: {:.3f} s, actual timecost: {:.3f} s",
                        iter, timestamp, total_cost_time.toc()));
                break;
            }

            TicToc iter_cost_time;
            std::vector<std::thread> threads;
            for (std::shared_ptr<Vehicle>& vehicle : vehicles) {
                std::thread thread([&vehicle, &vehicles]() {
                    vehicle->excute(vehicles.exclude(vehicle));
                });
                threads.emplace_back(std::move(thread));
            }

            for (auto& thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }

            spdlog::debug(fmt::format(
                "simulation time {:.3f} step cost {:.3f} sec", timestamp, iter_cost_time.toc()));  

            if (show_animation) {
                plt::cla();
                env->draw_env();
                for (std::shared_ptr<Vehicle> vehicle : vehicles) {
                    auto excepted_traj = vehicle->excepted_traj.to_vector();
                    vehicle->draw_vehicle();
                    plt::plot({vehicle->target.x}, {vehicle->target.y}, {{"marker", "x"}, {"color", vehicle->color}});
                    plt::plot(excepted_traj[0], excepted_traj[1], {{"color", vehicle->color}, {"linewidth", "1"}});
                    plt::text(vehicle->vis_text_pos.x, vehicle->vis_text_pos.y + 3,
                                fmt::format("level {:d}", vehicle->level), {{"color", vehicle->color}});
                    plt::text(vehicle->vis_text_pos.x, vehicle->vis_text_pos.y,
                                fmt::format("v = {:.2f} m/s", vehicle->state.v), {{"color", vehicle->color}});
                    plt::text(vehicle->vis_text_pos.x, vehicle->vis_text_pos.y - 3,
                                fmt::format("{}", utils::get_action_name(vehicle->cur_action)), {{"color", vehicle->color}});
                }
                plt::xlim(-map_size, map_size);
                plt::ylim(-map_size, map_size);
                plt::title(fmt::format("Round {} / {}", iter + 1, rounds_num));
                plt::set_aspect_equal();
                plt::pause(0.01);
            }
            timestamp += delta_t;
        }

        plt::clf();
        env->draw_env();

        for (std::shared_ptr<Vehicle>& vehicle : vehicles) {
            for (const State& state : vehicle->footprint) {
                vehicle->state = state;
                vehicle->draw_vehicle(true);
            }
            plt::text(vehicle->vis_text_pos.x, vehicle->vis_text_pos.y + 3,
                        fmt::format("level {:d}", vehicle->level), {{"color", vehicle->color}});
        }
        plt::xlim(-map_size, map_size);
        plt::ylim(-map_size, map_size);
        plt::title(fmt::format("Round {} / {}", iter + 1, rounds_num));
        plt::set_aspect_equal();
        plt::pause(1);
        if (save_fig) {
            plt::save((save_path / ( "Round_" + std::to_string(iter) + ".svg")).string(), 600);
        }
    }

    double succeed_rate = 100 * succeed_count / rounds_num;
    spdlog::info("\n=========================================");
    spdlog::info(fmt::format("Experiment success {}/{}({:.2f}%) rounds.", succeed_count, rounds_num, succeed_rate));
}

int main(int argc, char** argv) {
    std::filesystem::path source_file_path(__FILE__);
    std::filesystem::path project_path = source_file_path.parent_path().parent_path();

    int rounds_num = 5;
    std::filesystem::path output_path = project_path / "logs";
    std::filesystem::path config_path = "unprotected_left_turn.yaml";
    bool show_animation = true;
    bool save_flag = false;
    std::string log_level = "info";     // info

    int opt, option_index = 0;
    while ((opt = getopt_long(argc, argv, "r:o:l:c:n:f:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'r':
                rounds_num = std::stoi(optarg);
                break;
            case 'o':
                output_path = utils::absolute_path(optarg);
                break;
            case 'l':
                log_level = std::string(optarg);
                break;
            case 'c':
                config_path = optarg;
                break;
            case 'n':
                show_animation = false;
                break;
            case 'f':
                save_flag = true;
                break;
            default:
                exit(EXIT_FAILURE);
        }
    }

    spdlog::set_level(LOG_LEVEL_DICT[log_level]);
    spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e - %^%l%$ - %v");
    spdlog::info("log level : " + log_level);

    // config file path
    config_path = project_path / "config" / config_path;

    // output path
    if (save_flag) {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
        struct tm now_tm;
        localtime_r(&now_time_t, &now_tm); 
        std::stringstream ss;
        ss << std::put_time(&now_tm, "%Y-%m-%d-%H-%M-%S");
        output_path = output_path / ss.str();
        if (!std::filesystem::exists(output_path)) {
            std::filesystem::create_directories(output_path);
        }
    }

    run(rounds_num, config_path, output_path, show_animation, save_flag);

    return 0;
}
