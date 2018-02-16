#include "common.h"

#include <iostream>
#include <fstream>
#include <string>
#include <deque>
#include <cmath>
#include <cstdarg>

#include <ros/ros.h>
#include <ros/topic.h>
#include <ros/duration.h>
#include <std_msgs/Bool.h>
#include <std_srvs/Trigger.h>

#include "Drone.h"

using namespace std;

static const int angular_vel = 15;

static bool action_upon_slam_loss_backtrack (Drone& drone, const std::string& topic,
        trajectory_t& traj, trajectory_t& reverse_traj);
static bool action_upon_slam_loss_spin(Drone& drone, const std::string& topic);
static bool action_upon_slam_loss_reset(Drone& drone, const std::string& topic);

static trajectory_t append_trajectory (trajectory_t first, const trajectory_t& second);
static multiDOFpoint reverse_point(multiDOFpoint mdp);
static float yawFromQuat(geometry_msgs::Quaternion q);

template <class T>
static T last_msg (std::string topic) {
    // Return the last message of a latched topic
    return *(ros::topic::waitForMessage<T>(topic));
}

void update_stats_file(const std::string& stats_file__addr, const std::string& content){
    printf("insed update stats file"); 
    std::ofstream myfile;
    myfile.open(stats_file__addr, std::ofstream::out | std::ofstream::app);
    myfile << content << std::endl;
    myfile.close();
    return;
}


void sigIntHandler(int sig)
{
    ros::shutdown();
    //exit(0);
}

void action_upon_panic(Drone& drone) {
    const std::string panic_topic = "/panic_topic";

    // Move backwards at 1 m/s
    float yaw = drone.get_yaw();
    double vx = -std::sin(yaw*M_PI/180);
    double vy = -std::cos(yaw*M_PI/180);

    bool panicking = true;
    while (panicking) {
        drone.fly_velocity(vx, vy, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ROS_INFO("Panicking..");

        panicking = last_msg<std_msgs::Bool>(panic_topic).data;
    }

    // Stop afterwards
    drone.fly_velocity(0, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ROS_INFO("Done panicking!");
}

void action_upon_future_col(Drone& drone) {
    drone.fly_velocity(0, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // scan_around(drone, 30);
}

static bool action_upon_slam_loss_reset(Drone& drone, const std::string& topic) {
    ros::NodeHandle nh;
	ros::ServiceClient reset_client = nh.serviceClient<std_srvs::Trigger>("/slam_reset");
    std_srvs::Trigger srv;

    // Reset the SLAM map
    if (reset_client.call(srv)) {
        ROS_INFO("SLAM resetted succesfully");
    } else {
        ROS_ERROR("Failed to reset SLAM");
    }

    // Move around a little to initialize SLAM
    drone.fly_velocity(-0.5, 0, 0, 2);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    drone.fly_velocity(0.5, 0, 0, 4);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    drone.fly_velocity(-0.5, 0, 0, 2);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std_msgs::Bool is_lost = last_msg<std_msgs::Bool>(topic);
    return !is_lost.data;
}

static bool action_upon_slam_loss_spin(Drone& drone, const std::string& topic) {
    float init_yaw = drone.get_yaw();

    // Spin around until we re-localize
    for (int i = angular_vel; i <= 360; i += angular_vel) {
        // Turn slightly
        int angle = init_yaw + i;

        auto start_turn = std::chrono::system_clock::now();
        drone.set_yaw(angle <= 180 ? angle : angle - 360);

        auto end_turn = start_turn + std::chrono::seconds(1);
        std::this_thread::sleep_until(end_turn);

        // Check whether SLAM is back
        std_msgs::Bool is_lost = last_msg<std_msgs::Bool>(topic);

        if (!is_lost.data)
            return true;
    }

    return false;
}

static bool action_upon_slam_loss_backtrack (Drone& drone, const std::string& topic, trajectory_t& traj, trajectory_t& reverse_traj) {
    const double safe_speed = 0.5;

    while (reverse_traj.size() > 1) {
        follow_trajectory(drone, reverse_traj, traj, face_backward, safe_speed, false);

        // Check whether SLAM is back
        std_msgs::Bool is_lost = last_msg<std_msgs::Bool>(topic);
        if (!is_lost.data)
            return true;
    }
    ROS_INFO("done");

    return false;
}

bool action_upon_slam_loss (Drone& drone, slam_recovery_method slm...) {
    va_list args;
    va_start(args, slm);

    const std::string lost_topic = "/slam_lost";
    bool success;

    // Stop the drone
    drone.fly_velocity(0, 0, 0);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    if (slm == spin) {
        success = action_upon_slam_loss_spin(drone, lost_topic);
    } else if (slm == backtrack) {
        trajectory_t& traj = *(va_arg(args, trajectory_t*));
        trajectory_t& reverse_traj = *(va_arg(args, trajectory_t*));
        success = action_upon_slam_loss_backtrack(drone, lost_topic, traj, reverse_traj);
    } else if (slm == reset) {
        success = action_upon_slam_loss_reset(drone, lost_topic);
    }

    va_end(args);

    return success;
}

float distance(float x, float y, float z) {
  return std::sqrt(x*x + y*y + z*z);
}

void scan_around(Drone &drone, int angle) {
    float init_yaw = drone.get_yaw();
    ROS_INFO("Scanning around from %f degrees...", init_yaw);

    if (angle > 90) {
		ROS_INFO("we don't have support for angles greater than 90");
        exit(0);
	}

    drone.set_yaw(init_yaw+angle <= 180 ? init_yaw + angle : init_yaw + angle - 360);
    drone.set_yaw(init_yaw);
    drone.set_yaw(init_yaw-angle >= -180 ? init_yaw - angle : init_yaw - angle + 360);
    drone.set_yaw(init_yaw);
}

void spin_around(Drone &drone) {
    drone.fly_velocity(0, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    ROS_INFO("Spinning around...");

    float init_yaw = drone.get_yaw();
    for (int i = 0; i <= 360; i += 90) {
        int angle = init_yaw + i;
        drone.set_yaw(angle <= 180 ? angle : angle - 360);
    }
}

// Follows trajectory, popping commands off the front of it and returning those commands in reverse order
void follow_trajectory(Drone& drone, trajectory_t& traj,
        trajectory_t& reverse_traj, yaw_strategy_t yaw_strategy,
        float max_speed, bool check_position, float time) {

    trajectory_t reversed_commands;

    while (time > 0 && traj.size() > 1) {
        multiDOFpoint p = traj.front();
        multiDOFpoint p_next = traj[1];

        // Calculate the positions we should be at
        double p_x = p.transforms[0].translation.x;
        double p_y = p.transforms[0].translation.y;
        double p_z = p.transforms[0].translation.z;

        // Calculate the velocities we should be flying at
        double v_x = p.velocities[0].linear.x;
        double v_y = p.velocities[0].linear.y;
        double v_z = p.velocities[0].linear.z;

        if (check_position) {
            auto pos = drone.position();
            v_x += 0.05*(p_x-pos.x);
            v_y += 0.05*(p_y-pos.y);
            v_z += 0.2*(p_z-pos.z);
        }

        // Calculate the yaw we should be flying with
        float yaw = yawFromQuat(p.transforms[0].rotation);
        if (yaw_strategy == ignore_yaw)
            yaw = YAW_UNCHANGED;
        else if (yaw_strategy == face_forward)
            yaw = FACE_FORWARD;
        else if (yaw_strategy == face_backward) {
            yaw = FACE_BACKWARD;
        }

        // Make sure we're not going over the maximum speed
        double speed = std::sqrt(v_x*v_x + v_y*v_y + v_z*v_z);
        double scale = 1;
        if (speed > max_speed) {
            scale = max_speed / speed;

            v_x *= scale;
            v_y *= scale;
            v_z *= scale;
        }

        // Calculate the time for which these flight commands should run
        double segment_length = (p_next.time_from_start - p.time_from_start).toSec();
        double flight_time = segment_length <= time ? segment_length : time;
        double scaled_flight_time = flight_time / scale;

        // Fly for flight_time seconds
        auto segment_start_time = std::chrono::system_clock::now();
        drone.fly_velocity(v_x, v_y, v_z, yaw, scaled_flight_time+0.1); 

        std::this_thread::sleep_until(segment_start_time + std::chrono::duration<double>(scaled_flight_time));

        // Push completed command onto stack
        reversed_commands.push_front(reverse_point(p));

        // Update trajectory
        traj.front().time_from_start += ros::Duration(flight_time);
        if (traj.front().time_from_start >= p_next.time_from_start)
            traj.pop_front();

        time -= flight_time;
    }

    reverse_traj = append_trajectory(reversed_commands, reverse_traj);
}

static multiDOFpoint reverse_point(multiDOFpoint mdp) {
    multiDOFpoint result = mdp;

    result.time_from_start = -mdp.time_from_start;

    result.velocities[0].linear.x = -mdp.velocities[0].linear.x;
    result.velocities[0].linear.y = -mdp.velocities[0].linear.y;
    result.velocities[0].linear.z = -mdp.velocities[0].linear.z;
    
    return result;
}

static trajectory_t append_trajectory (trajectory_t first, const trajectory_t& second) {
    /*
    if (first.size() == 0)
        return second;

    ros::Duration time_step(0.5);
    if (second.size() > 1) {
        time_step = second[1].time_from_start - second[0].time_from_start;
    } else if (first.size() > 1) {
        time_step = first[1].time_from_start - first[0].time_from_start;
    }

    ros::Duration time_shift = first.back().time_from_start;
    time_shift -= second.front().time_from_start;
    time_shift += time_step;

    for (auto mdp : second) {
        mdp.time_from_start += time_shift;
        first.push_back(mdp);
    }
    */

    first.insert(first.end(), second.begin(), second.end());
    return first;
}

static float yawFromQuat(geometry_msgs::Quaternion q)
{
	float roll, pitch, yaw;

	// Formulas for roll, pitch, yaw
	// roll = atan2(2*(q.w*q.x + q.y*q.z), 1 - 2*(q.x*q.x + q.y*q.y) );
	// pitch = asin(2*(q.w*q.y - q.z*q.x));
	yaw = atan2(2*(q.w*q.z + q.x*q.y), 1 - 2*(q.y*q.y + q.z*q.z));
    yaw = (yaw*180)/3.14159265359;

    return (yaw <= 180 ? yaw : yaw - 360);
}


void update_stats(Drone& drone, const std::string& fname, std::string state){

    auto static flight_stats = drone.getFlightStats();

}

void output_flight_summary(msr::airlib::FlightStats init, msr::airlib::FlightStats end, std::string mission_status,
                           double cpu_compute_enenrgy, double gpu_compute_enenrgy,
                           const std::string& fname){
    //auto flight_stats = drone.getFlightStats();
    stringstream stats_ss;
    stats_ss << endl<<"{"<<endl;
    stats_ss<<  "  \"mission_status\": " << mission_status<<"," << endl;
    stats_ss << "  \"StateOfCharge\": " << init.state_of_charge  - end.state_of_charge << "," << endl;
    stats_ss << "  \"initial_voltage\": " << init.voltage << "," << endl;
    stats_ss << "  \"end_voltage\": " << end.voltage << "," << endl;
    stats_ss << "  \"energy_consumed\": " << end.energy_consumed - init.energy_consumed << "," << endl;
    stats_ss << "  \"distance_travelled\": " << end.distance_traveled - init.distance_traveled<< "," << endl;
    stats_ss << "  \"flight_time\": " << end.flight_time -init.flight_time<< "," << endl;
    stats_ss << "  \"collision_count\": " << end.collision_count  - init.collision_count << "," << endl;
    stats_ss << "  \"cpu_compute_enenrgy\": " << cpu_compute_enenrgy << "," << endl;
    stats_ss << "  \"gpu_compute_enenrgy\": " << gpu_compute_enenrgy << ",";
    //stats_ss << "}" << endl;

    update_stats_file(fname, stats_ss.str());
}

