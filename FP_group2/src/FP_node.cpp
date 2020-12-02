// Copyright 2016 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include <algorithm>
#include <vector>

#include <ros/ros.h>

#include <nist_gear/LogicalCameraImage.h>
#include <nist_gear/Order.h>
#include <nist_gear/Proximity.h>
#include <sensor_msgs/LaserScan.h>
#include <sensor_msgs/Range.h>
#include <std_msgs/Float32.h>
#include <std_msgs/String.h>
#include <std_srvs/Trigger.h>
#include <tf2_ros/transform_listener.h>
#include <geometry_msgs/TransformStamped.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h> //--needed for tf2::Matrix3x3
#include <nist_gear/AGVControl.h>
#include "competition.h"
#include "utils.h"
#include "gantry_control.h"

#include <tf2/LinearMath/Quaternion.h>

bool submitOrder(int AVG_id, std::string shipment_type){
    ROS_INFO("[submitOrder] Submitting order via AVG");

    // Create a node to call service from. Would be better to use one existing node
    // rather than creating a new one every time
    ros::NodeHandle node;

    // Create a Service client for the correct service, i.e. '/ariac/agv{AVG_id}'
    ros::ServiceClient avg_client;

    // Assign the service client to the correct service
    if(AVG_id == 1){
        avg_client = node.serviceClient<nist_gear::AGVControl>("/ariac/agv1");
    }else if(AVG_id == 2){
        avg_client = node.serviceClient<nist_gear::AGVControl>("/ariac/agv2");
    }else{
        ROS_ERROR_STREAM("[submitOrder] No AVG with id " << AVG_id <<". Valid ids are 1 and 2 only");
    }

    // Wait for client to start
    if (!avg_client.exists()) {
        avg_client.waitForExistence();
    }

    // Debug what you're doing
    ROS_INFO_STREAM("[submitOrder] Sending AVG " << AVG_id << " to submit order");

    // Create the message and assign the shipment type to it
    nist_gear::AGVControl srv;
    srv.request.shipment_type = shipment_type;

    // Send message and retrieve response
    avg_client.call(srv);
    if (!srv.response.success) {  // If not successful, print out why.
        ROS_ERROR_STREAM("[submitOrder]  Failed to submit: " << srv.response.message);
    } else {
        ROS_INFO("[submitOrder] Submitted");
    }

    return srv.response.success;
}

int main(int argc, char ** argv) {
    ros::init(argc, argv, "FP_node");
    ros::NodeHandle node;
    ros::AsyncSpinner spinner(8);
    spinner.start();

    Competition comp(node);
    comp.init();


    int Max_number_of_cameras = 17, Max_number_of_breakbeams = 29;
    std::ostringstream otopic;
    std::string topic;
    std::array<std::array<modelparam, 36>, 17> logicam, logicam2, logicam12;
    std::array < std::array < std::array < part, 10 >, 5 >, 5 > or_details, or_details_new, order_call;
    std::array < std::array < std::array < int, 10 >, 5 >, 5 > order_flag = {0};
    std::array<std::array<int, 2>, 10> completed = {0};
    part faulty_part, faulty_pose;
    bool break_beam;
    std::string c_state = comp.getCompetitionState();
    comp.getClock();

    ros::Subscriber logical_camera_subscriber_[Max_number_of_cameras];


    for (int x = 0; x < Max_number_of_cameras; x++) {
        otopic.str("");
        otopic.clear();
        otopic << "/ariac/logical_camera_" << (x);
        topic = otopic.str();
        logical_camera_subscriber_[x] = node.subscribe<nist_gear::LogicalCameraImage>(topic, 10, boost::bind(
                &Competition::logical_camera_callback, &comp, _1, x));
    }

    ros::Subscriber breakbream_sensor_subscriber_[Max_number_of_breakbeams];
    for (int x = 0; x < Max_number_of_breakbeams; x++) {
        otopic.str("");
        otopic.clear();
        otopic << "/ariac/breakbeam_" << (x);
        topic = otopic.str();
        breakbream_sensor_subscriber_[x] = node.subscribe<nist_gear::Proximity>(topic, 10, boost::bind(
                &Competition::breakbeam_sensor_callback, &comp, _1, x));
    }

    GantryControl gantry(node);
    gantry.init();
    gantry.goToPresetLocation(gantry.start_);
    logicam = comp.getter_logicam_callback();
    order_call = comp.getter_part_callback();
    or_details[0] = order_call[0];
    int on_table_1 = 0, on_table_2 = 0, new_order = 0, index = 0, part_on_belt = 0;
    auto gap_id = comp.check_gaps();
    int x_loop = 0;
    int on_belt = 0;
    std::array<std::array<int, 3>, 5> belt_part_arr = {0};
    comp.HumanDetection();

    // Initialization of variables and functions for move to preset location
    std::map <std::string, std::vector<PresetLocation>> presetLocation;
    std::string location;
    double loc_x, loc_y;
    gantry.initialPositions(presetLocation, comp.gap_nos, comp.Human, comp.Human_detected);
    ROS_INFO_STREAM("\nGap print:");
    for (auto i=0; i<3; i++)
    {
        ROS_INFO_STREAM("\ngap "<<i+1<<" "<<comp.gap_nos[i]);
    }
    comp.PartonBeltCheck(comp.received_orders_, x_loop, logicam, belt_part_arr, on_belt);

    for (int i = comp.received_orders_.size() - 1; i >= 0; i--) {
        for (int j = 0; j < comp.received_orders_[i].shipments.size(); j++) {
            if (completed[i][j] == 1)
                continue;
            if (new_order != 0) {
                new_order = 0;
                on_belt=0;
                belt_part_arr = {0};
                comp.PartonBeltCheck(comp.received_orders_, x_loop, logicam, belt_part_arr, on_belt);
                i = i + 2;
                break;
            }
            for (int k = 0; k < comp.received_orders_[i].shipments[j].products.size(); k++)
            {
                int count = 0, count1 = 0;
                for (auto l = 0; l < comp.received_orders_[i].shipments[j].products.size(); l++)
                {
                    if (order_flag[i][j][l] == 1)
                        count1++;
                }
                if (count1 == comp.received_orders_[i].shipments[j].products.size())
                {
                    ROS_INFO_STREAM("shipment j=" << j << "completed..");
                    if (or_details[i][j][k].agv_id == "agv1")
                    {
                        ROS_INFO_STREAM("\n Submitting Order: " << or_details_new[i][j][k].shipment);
                        submitOrder(1, or_details[i][j][k].shipment);
                    }
                    else if (or_details[i][j][k].agv_id == "agv2")
                    {
                        ROS_INFO_STREAM("\n Submitting Order: " << or_details_new[i][j][k].shipment);
                        submitOrder(2, or_details[i][j][k].shipment);
                    }
                }
                if (new_order)
                    break;
                if (order_flag[i][j][k] != 0)
                    continue;
                if (part_on_belt==0 && on_belt!=0)
                {
                    ROS_INFO_STREAM("Waiting for part to spawn on belt!!");
                    do {
                        //
                    }while(comp.beam_detect[0]==false);
                }
                logicam12 = comp.getter_logicam_callback();
                ROS_INFO_STREAM("\n Print i=" << i << ", j=" << j << ", k=" << k);
                ROS_INFO_STREAM("\n Print comp.received_orders_.size()=" << comp.received_orders_.size());
                ROS_INFO_STREAM("\n Print comp.received_orders_[i].shipments.size()="
                                        << comp.received_orders_[i].shipments.size());
                ROS_INFO_STREAM("\n Print comp.received_orders_[i].shipments[j].products.size()="
                                        << comp.received_orders_[i].shipments[j].products.size());
                ROS_INFO_STREAM("\n AGV ID: " << or_details[i][j][k].agv_id);
                ROS_INFO_STREAM("\n Order shipment name: " << or_details[i][j][k].shipment);
                ROS_INFO_STREAM("\n parts on_belt : " << on_belt);

                //loop to pick up part from belt and place in bins 9 and 14 if required
                if ((part_on_belt < on_belt) && (comp.beam_detect[0]==true || !logicam12[12][0].type.empty()))
                {
                    do
                        {
                        if (part_on_belt!=0)
                        {
                            ROS_INFO_STREAM("Waiting for part to spawn on belt!!");
                            do {
                                //
                            }while(comp.beam_detect[0]==false);
                        }
                        ROS_INFO_STREAM("\n Picking part from belt");
                        gantry.goToPresetLocation(gantry.belta_);
                        ROS_INFO_STREAM("\nWaiting for beam to turn off");
                        do {
                           //
                        } while (comp.beam_detect[0] == true);
                        logicam12 = comp.getter_logicam_callback();
                        if (part_on_belt!=0)
                        {
                            ros::Duration(2).sleep();
                        }
                        ROS_INFO_STREAM("\nWaiting to be detected by camera..");
                        do
                        {
                            logicam12 = comp.getter_logicam_callback();
                        }while(logicam12[12][0].type.empty());
                        ROS_INFO_STREAM("\n Detected by camera. Trying to pick up!!");
                        ros::Duration(0.2).sleep();
                        part belt_part;
                        belt_part.pose = logicam12[12][0].pose;
                        belt_part.type = logicam12[12][0].type;
                        if (logicam12[12][0].type == "piston_rod_part_red" || logicam12[12][0].type == "piston_rod_part_green" || logicam12[12][0].type == "piston_rod_part_blue")
                        {
                            gantry.goToPresetLocation(gantry.beltb1_);
                            do
                                {
                                gantry.activateGripper("left_arm");
                                ROS_INFO_STREAM("\n Trying to pick up!!");
                            } while (!gantry.getGripperState("left_arm").attached);
                            ros::Duration(1).sleep();
                            gantry.goToPresetLocation(gantry.beltb1_);
                            gantry.goToPresetLocation(gantry.belta_);
                            gantry.goToPresetLocation(gantry.start_);
                        }
                        if (logicam12[12][0].type == "pulley_part_red" || logicam12[12][0].type == "pulley_part_green" || logicam12[12][0].type == "pulley_part_blue")
                        {
                            gantry.goToPresetLocation(gantry.beltb2_);
                            do
                            {
                                gantry.activateGripper("left_arm");
                                ROS_INFO_STREAM("\n Trying to pick up!!");
                            } while (!gantry.getGripperState("left_arm").attached);
                            ros::Duration(1).sleep();
                            gantry.goToPresetLocation(gantry.beltb2_);
                            gantry.goToPresetLocation(gantry.belta_);
                            gantry.goToPresetLocation(gantry.start_);
                        }
                        auto i1 = belt_part_arr[part_on_belt][0];
                        auto j1 = belt_part_arr[part_on_belt][1];
                        auto k1 = belt_part_arr[part_on_belt][2];
                        auto target_pose = gantry.getTargetWorldPose(or_details[i1][j1][k1].pose, "agv1");

                        if (or_details[i1][j1][k1].agv_id == "any" && j1==0)
                            or_details[i1][j1][k1].agv_id = "agv1";
                        else if (or_details[i1][j1][k1].agv_id == "any" && j1!=0)
                            or_details[i1][j1][k1].agv_id = "agv2";
                        if (part_on_belt == 0)
                        {
                            gantry.goToPresetLocation(gantry.bin9_);
                            gantry.deactivateGripper("left_arm");
                        }
                        else
                        {
                            gantry.goToPresetLocation(gantry.bin14_);
                            gantry.deactivateGripper("left_arm");
                        }
                        logicam12 = comp.getter_logicam_callback();
                        logicam[2] = logicam12[2];
                        part_on_belt++;
                        ROS_INFO_STREAM("\nPart on belt value has been incremented!!!!!!");
                        gantry.goToPresetLocation(gantry.start1_);
                        order_flag[i1][j1][k1] = 1;
                    } while (part_on_belt < on_belt);
                }

                if (or_details[i][j][k].agv_id == "any" && j==0)
                    or_details[i][j][k].agv_id = "agv1";
                else if (or_details[i][j][k].agv_id == "any" && j!=0)
                    or_details[i][j][k].agv_id = "agv2";

                //loop for checking for picking other parts from their locations
                for (int x = 0; x < 17; x++)
                {
                    if (count == 1)
                        break;
                    for (int y = 0; y < 36; y++)
                    {
                        if (logicam[x][y].type == comp.received_orders_[i].shipments[j].products[k].type && logicam[x][y].Shifted == false) {
                            ROS_INFO_STREAM("\n\nPart being taken " << logicam[x][y].type);
                            ROS_INFO_STREAM("\n\nlogical camera: " << x);
                            gantry.goToPresetLocation(gantry.start_);

//                            ROS_INFO_STREAM("\n Test run of move to preset location function.");
                            location = logicam[x][y].frame;
                            auto location1 = logicam[x][y].frame;
                            ROS_INFO_STREAM("X POSITION " << logicam[x][y].pose.position.x);
                            ROS_INFO_STREAM("Y POSITION " << logicam[x][y].pose.position.y);
                            ROS_INFO_STREAM("Location: " << location);
                            auto target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv1");
                            loc_x = logicam[x][y].pose.position.x;
                            loc_y = logicam[x][y].pose.position.y;
                            gantry.moveToPresetLocation(presetLocation, location, loc_x, loc_y, 1);
                            ROS_INFO_STREAM("update Location: " << location);
                            part my_part;
                            my_part.type = logicam[x][y].type;
                            my_part.pose = logicam[x][y].pose;
                            ros::Duration(2).sleep();
                            gantry.pickPart(my_part);
                            ros::Duration(0.2).sleep();
                            gantry.moveToPresetLocation(presetLocation, location1, loc_x, loc_y, 2);
                            ROS_INFO_STREAM("Approaching AGV's to place object!!!");
                            if (or_details[i][j][k].agv_id == "agv1") {
                                gantry.goToPresetLocation(gantry.agv1_);
                                ROS_INFO_STREAM("\n Waypoint AGV1 reached\n");
                                if (or_details[i][j][k].pose.orientation.x != 0) {
                                    ROS_INFO_STREAM("Part is to be flipped");
                                    gantry.goToPresetLocation(gantry.agv1c_);
                                    ROS_INFO_STREAM("\n Waypoint AGV1 reached\n");
                                    gantry.goToPresetLocation(gantry.agv1flipa_);
                                    gantry.activateGripper("right_arm");
                                    ros::Duration(0.2).sleep();
                                    gantry.deactivateGripper("left_arm");
                                    ROS_INFO_STREAM("Part flipped");
                                    or_details[i][j][k].pose.orientation.x = 0.0;
                                    or_details[i][j][k].pose.orientation.y = 0;
                                    or_details[i][j][k].pose.orientation.z = 0.0;
                                    or_details[i][j][k].pose.orientation.w = 1;
                                    gantry.goToPresetLocation(gantry.agv1flipb_);
                                    gantry.placePartRight(or_details[i][j][k], "agv1");
                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
                                } else
                                    gantry.placePart(or_details[i][j][k], "agv1");
                                logicam[x][y].Shifted = true;
                            } else if (or_details[i][j][k].agv_id == "agv2") {
                                gantry.goToPresetLocation(gantry.agv2_);
                                ROS_INFO_STREAM("\n Waypoint AGV2 reached\n");
                                if (or_details[i][j][k].pose.orientation.x != 0) {
                                    ROS_INFO_STREAM("Part is to be flipped");
                                    gantry.goToPresetLocation(gantry.agv2a_);
                                    ROS_INFO_STREAM("\n Waypoint AGV2 reached\n");
                                    gantry.activateGripper("right_arm");
                                    ros::Duration(0.2).sleep();
                                    gantry.deactivateGripper("left_arm");
                                    ROS_INFO_STREAM("Part flipped");
                                    or_details[i][j][k].pose.orientation.x = 0.0;
                                    or_details[i][j][k].pose.orientation.y = 0;
                                    or_details[i][j][k].pose.orientation.z = 0.0;
                                    or_details[i][j][k].pose.orientation.w = 1;
                                    gantry.goToPresetLocation(gantry.agv2b_);
                                    gantry.placePartRight(or_details[i][j][k], "agv2");
                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
                                } else
                                    gantry.placePart(or_details[i][j][k], "agv2");
                                logicam[x][y].Shifted = true;
                                target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv2");
                            }

                            logicam2 = comp.getter_logicam_callback();
                            ROS_INFO_STREAM("\n After placing.");
                            ROS_INFO_STREAM("\n order name: "<<comp.received_orders_[i].shipments[j].products[k].type);
                            ROS_INFO_STREAM("\n order details: "<<or_details[i][j][k].pose);
                            ROS_INFO_STREAM("\n Target pose: "<<target_pose);
                            auto cam = logicam2[10][on_table_1].pose;
                            if (or_details[i][j][k].agv_id=="agv1")
                            {
                                for (auto ill=0; ill<=on_table_1; ill++)
                                {
                                    if (logicam2[10][ill].type == comp.received_orders_[i].shipments[j].products[k].type)
                                    {
                                        ROS_INFO_STREAM("\n Printing agv1 index value: "<<ill<<"\n Also product type = "<<logicam2[10][ill].type);
                                        index=ill;
                                        break;
                                    }
                                }
                                ROS_INFO_STREAM("\n AGV camera details: "<<logicam2[10][index].pose);
                                cam = logicam2[10][index].pose;
                                faulty_part = comp.quality_sensor_status1();
                            }
                            else if (or_details[i][j][k].agv_id=="agv2")
                            {
                                for (auto ill=0; ill<=on_table_2; ill++)
                                {
                                    if (logicam2[11][ill].type == comp.received_orders_[i].shipments[j].products[k].type)
                                    {
                                        ROS_INFO_STREAM("\n Printing agv2 index value: "<<ill<<"\n Also product type = "<<logicam2[11][ill].type);
                                        index=ill;
                                        break;
                                    }
                                }
                                ROS_INFO_STREAM("\n AGV camera details: "<<logicam2[11][index].pose);
                                cam = logicam2[11][index].pose;
                                faulty_part = comp.quality_sensor_status();
                            }
                            ros::Duration(0.2).sleep();
                            ROS_INFO_STREAM("\n X offset: "<<abs(cam.position.x-target_pose.position.x));
                            ROS_INFO_STREAM("\n Y offset: "<<abs(cam.position.y-target_pose.position.y));

// Faulty part check
                            if(faulty_part.faulty == true)
                            {
                                ROS_INFO_STREAM("Faulty Part detected!!!");
                                faulty_part.type = my_part.type;
                                ROS_INFO_STREAM("\n Trying to compute path for "<<faulty_part.type);
                                ROS_INFO_STREAM("\n Pose at faulty part "<<cam);
                                faulty_part.pose = cam;
                                faulty_part.pose.position.z -= 0.19;
                                ROS_INFO_STREAM("\n Pose for Faulty part "<<faulty_part.pose);
                                if (or_details[i][j][k].agv_id=="agv2")
                                {
                                    gantry.goToPresetLocation(gantry.agv2_);
                                    gantry.pickPart(faulty_part);
                                    gantry.goToPresetLocation(gantry.agv2_);

                                }
                                else if (or_details[i][j][k].agv_id=="agv1")
                                {
                                    gantry.goToPresetLocation(gantry.agv1_);
                                    gantry.pickPart(faulty_part);
                                    gantry.goToPresetLocation(gantry.agv1_);
                                }
                                gantry.goToPresetLocation(gantry.agv_faulty);
                                gantry.deactivateGripper("left_arm");
                                continue;
                            }

//Faulty pose correction
                            else if (abs(cam.position.x-target_pose.position.x)>0.03 || abs(cam.position.y-target_pose.position.y)>0.03)
                            {
                                if (abs(cam.position.x-target_pose.position.x)>0.03)
                                    ROS_INFO_STREAM("\n X offset detected");
                                if (abs(cam.position.y-target_pose.position.y)>0.03)
                                    ROS_INFO_STREAM("\n Y offset detected");
                                ROS_INFO_STREAM("\n Faulty Pose detected for part "<<logicam[x][y].type);
                                faulty_pose.type = my_part.type;
                                ROS_INFO_STREAM("\n Trying to compute path for "<<faulty_pose.type);
                                ROS_INFO_STREAM("\n Faulty pose "<<cam);
                                faulty_pose.pose = cam;
                                faulty_part.pose.position.z -= 0.19;
                                if (or_details[i][j][k].agv_id=="agv2")
                                {
                                    gantry.goToPresetLocation(gantry.agv2c_);
                                    ROS_INFO_STREAM("\n Trying to pick up...");
                                    gantry.pickPart(faulty_pose);
                                    ros::Duration(0.2).sleep();
                                    ROS_INFO_STREAM("\nPart Picked!");
                                    gantry.goToPresetLocation(gantry.agv2_);
                                    gantry.placePart(or_details[i][j][k], "agv2");
                                    ROS_INFO_STREAM("\n Placed!!!");
                                    on_table_2++;
                                }
                                else if (or_details[i][j][k].agv_id=="agv1")
                                {
                                    gantry.goToPresetLocation(gantry.agv1a_);
                                    ROS_INFO_STREAM("\n Trying to pick up...");
                                    gantry.pickPart(faulty_pose);
                                    ros::Duration(0.2).sleep();
                                    ROS_INFO_STREAM("\nPart Picked!");
                                    gantry.goToPresetLocation(gantry.agv1_);
                                    gantry.placePart(or_details[i][j][k], "agv1");
                                    ROS_INFO_STREAM("\n Placed!!!");
                                    on_table_1++;
                                }
                            }
                            else
                            {
                                if (or_details[i][j][k].agv_id=="agv2")
                                    on_table_2++;
                                else if (or_details[i][j][k].agv_id=="agv1")
                                    on_table_1++;
                                ROS_INFO_STREAM("Part has been placed without any problem, moving onto next product!");
                            }
                            auto state = gantry.getGripperState("left_arm");
                            if (state.attached)
                                gantry.goToPresetLocation(gantry.start_);
                            count++;
                            order_flag[i][j][k]=1;

                            //Checking if this is the last product of the shipment, and submitting score
                            if (k==comp.received_orders_[i].shipments[j].products.size()-1)
                            {
                                completed[i][j]=1;
                                if (or_details[i][j][k].agv_id=="agv1")
                                {
                                    ROS_INFO_STREAM("\n Submitting Order: "<<or_details_new[i][j][k].shipment);
                                    submitOrder(1, or_details_new[i][j][k].shipment);
                                }
                                else if (or_details[i][j][k].agv_id=="agv2")
                                {
                                    ROS_INFO_STREAM("\n Submitting Order: "<<or_details_new[i][j][k].shipment);
                                    submitOrder(2, or_details_new[i][j][k].shipment);
                                }

                            }
                            or_details_new = comp.getter_part_callback();
                            ros::Duration(0.2).sleep();
                            ROS_INFO_STREAM("\n Checking for high priority order insertion.. absent? (1 is true) "<<or_details_new[i+1][j][k].shipment.empty());
                            if (!or_details_new[i+1][j][k].shipment.empty())
                            {
                                ROS_INFO_STREAM("\n Order NEW shipment name 1: "<<or_details_new[i+1][j][k].shipment);
                                or_details[i+1]=or_details_new[i+1];
                                ROS_INFO_STREAM("\n Copied info details "<<or_details[i+1][j][k].shipment);
                                i = i+1;
                                ROS_INFO_STREAM("\n Value of i: "<<i);
                                ROS_INFO_STREAM("\n New order size detected.. breaking.. ");
                                new_order++;
                                ROS_INFO_STREAM("\n Value of new: "<<new_order);
                            }
                            break;
                        }

//// - this is for trial purposes
//                            else if (comp.received_orders_[i].shipments[j].products[k].type == "disk_part_red") {
//                                ROS_INFO_STREAM("\n Test run of move to preset location function.");
//                                location = logicam[x][y].frame;
//                                ROS_INFO_STREAM("X POSITION " << logicam[x][y].pose.position.x);
//                                ROS_INFO_STREAM("Y POSITION " << logicam[x][y].pose.position.y);
//                                auto target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv1");
//
//                                loc_x = logicam[x][y].pose.position.x;
//                                loc_y = logicam[x][y].pose.position.y;
//                                gantry.moveToPresetLocation(presetLocation, location, loc_x, loc_y, 1);
//                                part my_part;
//                                my_part.type = logicam[x][y].type;
//                                my_part.pose = logicam[x][y].pose;
//                                gantry.pickPart(my_part);
//                                ros::Duration(0.2).sleep();
//                                gantry.moveToPresetLocation(presetLocation, location, loc_x, loc_y, 2);
//                                if (or_details[i][j][k].agv_id == "agv1") {
//                                    gantry.goToPresetLocation(gantry.agv1_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV1 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv1");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    logicam[x][y].Shifted = true;
//                                } else if (or_details[i][j][k].agv_id == "agv2") {
//                                    gantry.goToPresetLocation(gantry.agv2_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV2 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv2");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    logicam[x][y].Shifted = true;
//                                    target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv2");
//                                }
//                            }
//
//                            else if (comp.received_orders_[i].shipments[j].products[k].type == "piston_part_red") {
//                                ROS_INFO_STREAM("\n Test run of move to preset location function.");
//                                location = logicam[x][y].frame;
//                                ROS_INFO_STREAM("X POSITION " << logicam[x][y].pose.position.x);
//                                ROS_INFO_STREAM("Y POSITION " << logicam[x][y].pose.position.y);
//                                auto target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv1");
//
//                                loc_x = logicam[x][y].pose.position.x;
//                                loc_y = logicam[x][y].pose.position.y;
//                                gantry.moveToPresetLocation(presetLocation, location, loc_x, loc_y, 1);
//                                part my_part;
//                                my_part.type = logicam[x][y].type;
//                                my_part.pose = logicam[x][y].pose;
//                                gantry.pickPart(my_part);
//                                ros::Duration(0.2).sleep();
//                                gantry.moveToPresetLocation(presetLocation, location, loc_x, loc_y, 2);
//                                if (or_details[i][j][k].agv_id == "agv1") {
//                                    gantry.goToPresetLocation(gantry.agv1_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV1 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv1");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    logicam[x][y].Shifted = true;
//                                } else if (or_details[i][j][k].agv_id == "agv2") {
//                                    gantry.goToPresetLocation(gantry.agv2_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV2 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv2");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    logicam[x][y].Shifted = true;
//                                    target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv2");
//                                }
//                            }

//// trial purpose ends here

//                                do{
//                                    comp.breakbeam_sensing();
//                                    ROS_INFO_STREAM("Waiting for human to move from the required position..");
//                                } while (!((comp.beam_seq[10] < comp.beam_seq[9]) &&
//                                           (comp.beam_seq[9] > comp.beam_seq[8]) &&
//                                           (comp.beam_seq[10] > comp.beam_seq[8])));
//                                gantry.goToPresetLocation(gantry.lc5le_);
//                                gantry.goToPresetLocation(gantry.lc5lf_);
//                                gantry.goToPresetLocation(gantry.lc5lg_);
//                                part my_part;
//                                my_part.type = logicam[x][y + j + 1].type;
//                                my_part.pose = logicam[x][y + j + 1].pose;
//                                auto target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv1");
//                                gantry.pickPart(my_part);
//                                ros::Duration(0.2).sleep();
//                                gantry.goToPresetLocation(gantry.lc5lf_);
//                                gantry.goToPresetLocation(gantry.lc5le_);
//                                gantry.goToPresetLocation(gantry.lc5ld_);
//                                gantry.goToPresetLocation(gantry.lc5lc_);
//                                gantry.goToPresetLocation(gantry.lc5lb_);
//                                gantry.goToPresetLocation(gantry.lc5la_);
//                                if (or_details[i][j][k].agv_id == "agv1") {
//                                    gantry.goToPresetLocation(gantry.agv1_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV1 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv1");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    logicam[x][y + j + 1].Shifted = true;
//                                } else if (or_details[i][j][k].agv_id == "agv2") {
//                                    gantry.goToPresetLocation(gantry.agv2_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV2 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv2");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    logicam[x][y + j + 1].Shifted = true;
//                                    target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv2");
//                                }
//                                logicam2 = comp.getter_logicam_callback();
//                                ROS_INFO_STREAM("\n After placing.");
//                                ROS_INFO_STREAM(
//                                        "\n order name: " << comp.received_orders_[i].shipments[j].products[k].type);
//                                ROS_INFO_STREAM("\n order details: " << or_details[i][j][k].pose);
//                                ROS_INFO_STREAM("\n Target pose: " << target_pose);
//                                auto cam = logicam2[10][on_table_1].pose;
//                                ros::Duration(1).sleep();
//                                if (or_details[i][j][k].agv_id == "agv1") {
//                                    for (auto ill = 0; ill <= on_table_1; ill++) {
//                                        if (logicam2[10][ill].type ==
//                                            comp.received_orders_[i].shipments[j].products[k].type) {
//                                            ROS_INFO_STREAM(
//                                                    "\n Printing ILL value: " << ill << "\n Also printing type = "
//                                                                              << logicam2[10][ill].type
//                                                                              << "\nAlso, shipment type"
//                                                                              << comp.received_orders_[i].shipments[j].products[k].type);
//                                            index = ill;
//                                            break;
//                                        }
//                                    }
//                                    ROS_INFO_STREAM("\n AGV camera details: " << logicam2[10][index].pose);
//                                    cam = logicam2[10][index].pose;
//                                    faulty_part = comp.quality_sensor_status1();
//                                    if (faulty_part.faulty == true) {
//                                        ROS_INFO_STREAM("\npart is faulty!!!!!!!");
//                                    }
//                                } else if (or_details[i][j][k].agv_id == "agv2") {
//                                    for (auto ill = 0; ill <= on_table_2; ill++) {
//                                        if (logicam2[11][ill].type ==
//                                            comp.received_orders_[i].shipments[j].products[k].type) {
//                                            ROS_INFO_STREAM(
//                                                    "\n Printing ILL value: " << ill << "\n Also printing type = "
//                                                                              << logicam2[11][ill].type
//                                                                              << "\nAlso, shipment type"
//                                                                              << comp.received_orders_[i].shipments[j].products[k].type);
//                                            index = ill;
//                                            break;
//                                        }
//                                    }
//                                    ROS_INFO_STREAM("\n AGV camera details: " << logicam2[11][on_table_2].pose);
//                                    cam = logicam2[11][index].pose;
//                                    faulty_part = comp.quality_sensor_status();
//                                }
//                                ros::Duration(0.2).sleep();
//                                ROS_INFO_STREAM("\n X offset: " << abs(cam.position.x - target_pose.position.x));
//                                ROS_INFO_STREAM("\n Y offset: " << abs(cam.position.y - target_pose.position.y));
//                                if (faulty_part.faulty == true) {
//                                    ROS_INFO_STREAM("Faulty Part detected!!!");
//                                    faulty_part.type = my_part.type;
//                                    ROS_INFO_STREAM("\n Trying to compute path for " << faulty_part.type);
//                                    ROS_INFO_STREAM("\n Pose at faulty part " << cam);
//                                    faulty_part.pose = cam;
//                                    faulty_part.pose.position.z -= 0.19;
//                                    ROS_INFO_STREAM("\n Pose for Faulty part " << faulty_part.pose);
//                                    if (or_details[i][j][k].agv_id == "agv2") {
//                                        ROS_INFO_STREAM("\n AGV2 Loop");
//                                        gantry.goToPresetLocation(gantry.agv2_);
//                                        gantry.pickPart(faulty_part);
//                                        gantry.goToPresetLocation(gantry.agv2_);
//
//                                    } else if (or_details[i][j][k].agv_id == "agv1") {
//                                        ROS_INFO_STREAM("\n AGV1 Loop");
//                                        gantry.goToPresetLocation(gantry.agv1_);
//                                        gantry.pickPart(faulty_part);
//                                        gantry.goToPresetLocation(gantry.agv1_);
//                                    }
//                                    gantry.goToPresetLocation(gantry.agv_faulty);
//                                    gantry.deactivateGripper("left_arm");
//                                    continue;
//                                } else {
//                                    if (or_details[i][j][k].agv_id == "agv2")
//                                        on_table_2++;
//                                    else if (or_details[i][j][k].agv_id == "agv1")
//                                        on_table_1++;
//                                }
//
//                                auto state = gantry.getGripperState("left_arm");
//                                if (state.attached)
//                                    gantry.goToPresetLocation(gantry.start_);
//                                ROS_INFO_STREAM("\n\nDISK part Green placed: " << x);
//                                or_details_new = comp.getter_part_callback();
//                                ros::Duration(0.2).sleep();
//                                ROS_INFO_STREAM(
//                                        "\n Order NEW shipment name 1: " << or_details_new[i + 1][j][k].shipment);
//                                if (!or_details_new[i + 1][j][k].shipment.empty()) {
//                                    or_details[i + 1] = or_details_new[i + 1];
//                                }
//                                count++;
//                                order_flag[i][j][k] = 1;
//                                if (k == comp.received_orders_[i].shipments[j].products.size() - 1) {
//                                    completed[i][j] = 1;
//                                    if (or_details[i][j][k].agv_id == "agv1") {
//                                        ROS_INFO_STREAM("\n Submitting Order: " << or_details_new[i][j][k].shipment);
//                                        submitOrder(1, or_details_new[i][j][k].shipment);
//                                    } else if (or_details[i][j][k].agv_id == "agv2") {
//                                        ROS_INFO_STREAM("\n Submitting Order: " << or_details_new[i][j][k].shipment);
//                                        submitOrder(2, or_details_new[i][j][k].shipment);
//                                    }
//
//                                }
//                                break;
//                            }

//// Sample from where i need to check

//                            else if (comp.received_orders_[i].shipments[j].products[k].type == "gasket_part_red")
//                            {
//                                ROS_INFO_STREAM("\n Test run of move to preset location function.");
//                                location = logicam[x][y].frame;
////                                ROS_INFO_STREAM(location);
//
//                                ROS_INFO_STREAM("X POSITION "<< logicam[x][y].pose.position.x);
//                                ROS_INFO_STREAM("Y POSITION "<< logicam[x][y].pose.position.y);
//                                auto target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv1");
//
//                                loc_x = logicam[x][y].pose.position.x;
//                                loc_y = logicam[x][y].pose.position.y;
//                                gantry.moveToPresetLocation(presetLocation, location, loc_x, loc_y, 1);
//                                ROS_INFO_STREAM("\n Done running");
//                                part my_part;
//                                my_part.type = logicam[x][y].type;
//                                my_part.pose = logicam[x][y].pose;
//                                gantry.pickPart(my_part);
//                                target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv1");
//                                if (or_details[i][j][k].agv_id == "agv1")
//                                {
//                                    gantry.goToPresetLocation(gantry.agv1_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV1 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv1");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                }
//                                else if (or_details[i][j][k].agv_id == "agv2")
//                                {
//                                    gantry.goToPresetLocation(gantry.agv2_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV2 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv2");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv2");
//                                }
//                            }
//                            else if (comp.received_orders_[i].shipments[j].products[k].type == "gear_part_blue")
//                            {
//                                ROS_INFO_STREAM("\n Test run of move to preset location function.");
//                                location = logicam[x][y].frame;
//                                ROS_INFO_STREAM(location);
//
//                                ROS_INFO_STREAM("X POSITION "<< logicam[x][y].pose.position.x);
//                                ROS_INFO_STREAM("Y POSITION "<< logicam[x][y].pose.position.y);
//                                auto target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv1");
//
//                                loc_x = logicam[x][y].pose.position.x;
//                                loc_y = logicam[x][y].pose.position.y;
//                                gantry.moveToPresetLocation(presetLocation, location, loc_x, loc_y);
//                                ROS_INFO_STREAM("\n Done running");
//                                part my_part;
//                                my_part.type = logicam[x][y].type;
//                                my_part.pose = logicam[x][y].pose;
//                                gantry.pickPart(my_part);
//                            }
//
//                                gantry.goToPresetLocation(gantry.shelf5a_);
//                                gantry.goToPresetLocation(gantry.shelf5b_);
//                                gantry.goToPresetLocation(gantry.shelf5d_);
//                                ROS_INFO_STREAM("\n Before picking.");
//                                ROS_INFO_STREAM("\n order name: "<<comp.received_orders_[i].shipments[j].products[k].type);
//                                ROS_INFO_STREAM("\n order details: "<<or_details[i][j][k].pose);
//                                part my_part;
//                                my_part.type = logicam[x][y].type;
//                                my_part.pose = logicam[x][y].pose;
//                                gantry.goToPresetLocation(gantry.lc4ra_);
//                                gantry.goToPresetLocation(gantry.lc4rb_);
//                                gantry.goToPresetLocation(gantry.lc4rc_);
//                                do {
//                                    comp.breakbeam_sensing();
//                                    ROS_INFO_STREAM("Waiting for human to move from the required position..");
//                                }while(!((comp.beam_seq[15]<comp.beam_seq[14]) && (comp.beam_seq[14]>comp.beam_seq[13]) && (comp.beam_seq[15]>comp.beam_seq[13])));
//                                gantry.goToPresetLocation(gantry.lc4rd_);
//                                gantry.goToPresetLocation(gantry.lc4re_);
////                                gantry.goToPresetLocation(gantry.lc4rf_);
//                                part my_part;
//                                my_part.type = logicam[x][y].type;
//                                my_part.pose =logicam[x][y].pose;
//                                auto target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv1");
//                                ROS_INFO_STREAM("my_part.type is" << my_part.type);
//                                gantry.pickPart(my_part);
//                                ros::Duration(0.2).sleep();
////                                gantry.goToPresetLocation(gantry.lc4rf_);
//                                gantry.goToPresetLocation(gantry.lc4re_);
//                                gantry.goToPresetLocation(gantry.lc4rd_);
//                                gantry.goToPresetLocation(gantry.lc4rc_);
//                                gantry.goToPresetLocation(gantry.lc4rb_);
//                                gantry.goToPresetLocation(gantry.lc4ra_);
//
//                                if (or_details[i][j][k].agv_id=="agv1")
//                                {
//                                    if(or_details[i][j][k].pose.orientation.x != 0)
//                                    {
//                                        ROS_INFO_STREAM("Part is to be flipped");
//                                        gantry.goToPresetLocation(gantry.agv1c_);
//                                        ROS_INFO_STREAM("\n Waypoint AGV1 reached\n");
//                                        gantry.goToPresetLocation(gantry.agv1flipa_);
//                                        gantry.activateGripper("right_arm");
//                                        ros::Duration(0.2).sleep();
//                                        gantry.deactivateGripper("left_arm");
//                                        ROS_INFO_STREAM("Part flipped");
//                                        or_details[i][j][k].pose.orientation.x = 0.0;
//                                        or_details[i][j][k].pose.orientation.y = 0;
//                                        or_details[i][j][k].pose.orientation.z = 0.0;
//                                        or_details[i][j][k].pose.orientation.w = 1;
//                                        gantry.goToPresetLocation(gantry.agv1flipb_);
//                                        gantry.placePartRight(or_details[i][j][k], "agv1");
//                                        ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    }
//                                    else
//                                        gantry.placePart(or_details[i][j][k], "agv1");
//                                    logicam[x][y].Shifted=true;
//                                }
//                                else if (or_details[i][j][k].agv_id=="agv2") {
//                                    if (or_details[i][j][k].pose.orientation.x != 0) {
//                                        ROS_INFO_STREAM("Part is to be flipped");
//                                        gantry.goToPresetLocation(gantry.agv2a_);
//                                        ROS_INFO_STREAM("\n Waypoint AGV2 reached\n");
//                                        gantry.activateGripper("right_arm");
//                                        ros::Duration(0.2).sleep();
//                                        gantry.deactivateGripper("left_arm");
//                                        ROS_INFO_STREAM("Part flipped");
//                                        or_details[i][j][k].pose.orientation.x = 0.0;
//                                        or_details[i][j][k].pose.orientation.y = 0;
//                                        or_details[i][j][k].pose.orientation.z = 0.0;
//                                        or_details[i][j][k].pose.orientation.w = 1;
//                                        gantry.goToPresetLocation(gantry.agv2b_);
//                                        gantry.placePartRight(or_details[i][j][k], "agv2");
//                                        ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    } else
//                                        gantry.placePart(or_details[i][j][k], "agv2");
//                                    logicam[x][y].Shifted = true;
//                                    target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv2");
//                                }
//                                if (or_details[i][j][k].agv_id == "agv1")
//                                {
//                                    gantry.goToPresetLocation(gantry.agv1_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV1 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv1");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    logicam[x][y].Shifted=true;
//                                }
//                                else if (or_details[i][j][k].agv_id == "agv2")
//                                {
//                                    gantry.goToPresetLocation(gantry.agv2_);
//                                    ROS_INFO_STREAM("\n Waypoint AGV2 reached\n");
//                                    gantry.placePart(or_details[i][j][k], "agv2");
//                                    ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                    logicam[x][y].Shifted=true;
//                                    target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv2");
//                                }
//
//                                logicam2 = comp.getter_logicam_callback();
//                                ROS_INFO_STREAM("\n After placing.");
//                                ROS_INFO_STREAM("\n order name: "<<comp.received_orders_[i].shipments[j].products[k].type);
//                                ROS_INFO_STREAM("\n order details: "<<or_details[i][j][k].pose);
//                                ROS_INFO_STREAM("\n Target pose: "<<target_pose);
//                                auto cam = logicam2[10][on_table_1].pose;
//                                if (or_details[i][j][k].agv_id=="agv1")
//                                {
//                                    for (auto ill=0; ill<=on_table_1; ill++)
//                                    {
//                                        if (logicam2[10][ill].type == comp.received_orders_[i].shipments[j].products[k].type)
//                                        {
//                                            ROS_INFO_STREAM("\n Printing ILL value: "<<ill<<"\n Also printing type = "<<logicam2[10][ill].type<<"\nAlso, shipment type"<<comp.received_orders_[i].shipments[j].products[k].type);
//                                            index=ill;
//                                            break;
//                                        }
//                                    }
//                                    ROS_INFO_STREAM("\n AGV camera details: "<<logicam2[10][index].pose);
//                                    cam = logicam2[10][index].pose;
//                                    faulty_part = comp.quality_sensor_status1();
//                                }
//                                else if (or_details[i][j][k].agv_id=="agv2")
//                                {
//                                    for (auto ill=0; ill<=on_table_2; ill++)
//                                    {
//                                        if (logicam2[11][ill].type == comp.received_orders_[i].shipments[j].products[k].type)
//                                        {
//                                            ROS_INFO_STREAM("\n Printing ILL value: "<<ill<<"\n Also printing type = "<<logicam2[11][ill].type<<"\nAlso, shipment type"<<comp.received_orders_[i].shipments[j].products[k].type);
//                                            index=ill;
//                                            break;
//                                        }
//                                    }
//                                    ROS_INFO_STREAM("\n AGV camera details: "<<logicam2[11][on_table_2].pose);
//                                    cam = logicam2[11][index].pose;
//                                    faulty_part = comp.quality_sensor_status();
//                                }
//                                ros::Duration(0.2).sleep();
//                                ROS_INFO_STREAM("\n X offset: "<<abs(cam.position.x-target_pose.position.x));
//                                ROS_INFO_STREAM("\n Y offset: "<<abs(cam.position.y-target_pose.position.y));
//                                if(faulty_part.faulty == true)
//                                {
//                                    ROS_INFO_STREAM("Faulty Part detected!!!");
//                                    faulty_part.type = my_part.type;
//                                    ROS_INFO_STREAM("\n Trying to compute path for "<<faulty_part.type);
//                                    ROS_INFO_STREAM("\n Pose at faulty part "<<cam);
//                                    faulty_part.pose = cam;
//                                    faulty_part.pose.position.z -= 0.19;
//                                    ROS_INFO_STREAM("\n Pose for Faulty part "<<faulty_part.pose);
//                                    if (or_details[i][j][k].agv_id=="agv2")
//                                    {
//                                        gantry.goToPresetLocation(gantry.agv2_);
//                                        gantry.pickPart(faulty_part);
//                                        gantry.goToPresetLocation(gantry.agv2_);
//
//                                    }
//                                    else if (or_details[i][j][k].agv_id=="agv1")
//                                    {
//                                        gantry.goToPresetLocation(gantry.agv1_);
//                                        gantry.pickPart(faulty_part);
//                                        gantry.goToPresetLocation(gantry.agv1_);
//                                    }
//                                    gantry.goToPresetLocation(gantry.agv_faulty);
//                                    gantry.deactivateGripper("left_arm");
//                                    continue;
//                                }
//                                else if (abs(cam.position.x-target_pose.position.x)>0.03 || abs(cam.position.y-target_pose.position.y)>0.03)
//                                {
//                                    if (abs(cam.position.x-target_pose.position.x)>0.03)
//                                        ROS_INFO_STREAM("\n X offset detected");
//                                    if (abs(cam.position.y-target_pose.position.y)>0.03)
//                                        ROS_INFO_STREAM("\n Y offset detected");
//                                    ROS_INFO_STREAM("\n Faulty Pose detected for part "<<logicam[x][y].type);
//                                    faulty_pose.type = my_part.type;
//                                    ROS_INFO_STREAM("\n Trying to compute path for "<<faulty_pose.type);
//                                    ROS_INFO_STREAM("\n Faulty pose "<<cam);
//                                    faulty_pose.pose = cam;
//                                    if (or_details[i][j][k].agv_id=="agv2")
//                                    {
//                                        gantry.goToPresetLocation(gantry.agv2c_);
//                                        ROS_INFO_STREAM("\n Trying to pick up...");
//                                        gantry.pickPart(faulty_pose);
//                                        ROS_INFO_STREAM("\nPart Picked!");
//                                        gantry.goToPresetLocation(gantry.agv2_);
//                                        gantry.placePart(or_details[i][j][k], "agv2");
//                                        ROS_INFO_STREAM("\n Placed!!!");
//                                        on_table_2++;
//                                    }
//                                    else if (or_details[i][j][k].agv_id=="agv1")
//                                    {
//                                        gantry.goToPresetLocation(gantry.agv1a_);
//                                        ROS_INFO_STREAM("\n Trying to pick up...");
//                                        gantry.pickPart(faulty_pose);
//                                        ROS_INFO_STREAM("\nPart Picked!");
//                                        gantry.goToPresetLocation(gantry.agv1_);
//                                        gantry.placePart(or_details[i][j][k], "agv1");
//                                        ROS_INFO_STREAM("\n Placed!!!");
//                                        on_table_1++;
//                                    }
//                                }
//                                else
//                                {
//                                    if (or_details[i][j][k].agv_id=="agv2")
//                                        on_table_2++;
//                                    else if (or_details[i][j][k].agv_id=="agv1")
//                                        on_table_1++;
//                                }
//                                auto state = gantry.getGripperState("left_arm");
//                                if (state.attached)
//                                    gantry.goToPresetLocation(gantry.start_);
//                                count++;
//                                order_flag[i][j][k]=1;
//                                if (k==comp.received_orders_[i].shipments[j].products.size()-1)
//                                {
//                                    completed[i][j]=1;
//                                    if (or_details[i][j][k].agv_id=="agv1")
//                                    {
//                                        ROS_INFO_STREAM("\n Submitting Order: "<<or_details_new[i][j][k].shipment);
//                                        submitOrder(1, or_details_new[i][j][k].shipment);
//                                    }
//                                    else if (or_details[i][j][k].agv_id=="agv2")
//                                    {
//                                        ROS_INFO_STREAM("\n Submitting Order: "<<or_details_new[i][j][k].shipment);
//                                        submitOrder(2, or_details_new[i][j][k].shipment);
//                                    }
//
//                                }
//                                or_details_new = comp.getter_part_callback();
//                                ros::Duration(0.2).sleep();
//                                ROS_INFO_STREAM("\n Order NEW shipment name 1: "<<or_details_new[i+1][j][k].shipment);
//                                ROS_INFO_STREAM("\n is it empty??? "<<or_details_new[i+1][j][k].shipment.empty());
//                                if (!or_details_new[i+1][j][k].shipment.empty())
//                                {
//                                    or_details[i+1]=or_details_new[i+1];
//                                    ROS_INFO_STREAM("\n Copied info details "<<or_details[i+1][j][k].shipment);
//                                    i = i+1;
//                                    ROS_INFO_STREAM("\n Value of i: "<<i);
//                                    ROS_INFO_STREAM("\n New order size detected.. breaking.. ");
//                                    new_order++;
//                                    ROS_INFO_STREAM("\n Value of new: "<<new_order);
//                                }
//                                break;
//                            }

//                            else if (part_on_belt == 1) {
//                                if (comp.received_orders_[i].shipments[j].products[k].type == "pulley_part_green") {
//                                    part_on_belt++;
//                                    gantry.goToPresetLocation(gantry.start_);
//                                    gantry.goToPresetLocation(gantry.bin3_);
//                                    part my_part;
//                                    my_part.type = logicam12[0][y].type;
//                                    my_part.pose = logicam12[0][y].pose;
//                                    auto target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv1");
//                                    gantry.pickPart(my_part);
//                                    ros::Duration(0.2).sleep();
//                                    gantry.goToPresetLocation(gantry.bin3_);
//                                    ros::Duration(0.2).sleep();
//                                    gantry.goToPresetLocation(gantry.start_);
//                                    if (or_details[i][j][k].agv_id == "agv1") {
//                                        if (or_details[i][j][k].pose.orientation.x != 0) {
//                                            ROS_INFO_STREAM("Part is to be flipped");
//                                            gantry.goToPresetLocation(gantry.agv1_);
//                                            ROS_INFO_STREAM("\n Waypoint AGV1 reached\n");
//                                            gantry.goToPresetLocation(gantry.agv1flipa_);
//                                            gantry.activateGripper("right_arm");
//                                            ros::Duration(0.2).sleep();
//                                            gantry.deactivateGripper("left_arm");
//                                            ROS_INFO_STREAM("Part flipped");
//                                            or_details[i][j][k].pose.orientation.x = 0.0;
//                                            or_details[i][j][k].pose.orientation.y = 0;
//                                            or_details[i][j][k].pose.orientation.z = 0.0;
//                                            or_details[i][j][k].pose.orientation.w = 1;
//                                            gantry.goToPresetLocation(gantry.agv1flipb_);
//                                            gantry.placePartRight(or_details[i][j][k], "agv1");
//                                            ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                        } else
//                                            gantry.placePart(or_details[i][j][k], "agv1");
//                                        logicam[x][y].Shifted = true;
//                                    } else if (or_details[i][j][k].agv_id == "agv2") {
//                                        if (or_details[i][j][k].pose.orientation.x != 0) {
//                                            ROS_INFO_STREAM("Part is to be flipped");
//                                            gantry.goToPresetLocation(gantry.agv2a_);
//                                            ROS_INFO_STREAM("\n Waypoint AGV2 reached\n");
//                                            gantry.activateGripper("right_arm");
//                                            ros::Duration(0.2).sleep();
//                                            gantry.deactivateGripper("left_arm");
//                                            ROS_INFO_STREAM("Part flipped");
//                                            or_details[i][j][k].pose.orientation.x = 0.0;
//                                            or_details[i][j][k].pose.orientation.y = 0;
//                                            or_details[i][j][k].pose.orientation.z = 0.0;
//                                            or_details[i][j][k].pose.orientation.w = 1;
//                                            gantry.goToPresetLocation(gantry.agv2b_);
//                                            gantry.placePartRight(or_details[i][j][k], "agv2");
//                                            ROS_INFO_STREAM("\n Object placed!!!!!!!!!!\n");
//                                        } else
//                                            gantry.placePart(or_details[i][j][k], "agv2");
//                                        logicam[x][y].Shifted = true;
//                                        target_pose = gantry.getTargetWorldPose(or_details[i][j][k].pose, "agv2");
//                                    }
//                                    logicam2 = comp.getter_logicam_callback();
//                                    ROS_INFO_STREAM("\n After placing.");
//                                    ROS_INFO_STREAM("\n order name: "
//                                                            << comp.received_orders_[i].shipments[j].products[k].type);
//                                    ROS_INFO_STREAM("\n order details: " << or_details[i][j][k].pose);
//                                    ROS_INFO_STREAM("\n Target pose: " << target_pose);
//                                    auto cam = logicam2[10][on_table_1].pose;
//                                    if (or_details[i][j][k].agv_id == "agv1") {
//                                        for (auto ill = 0; ill <= on_table_1; ill++) {
//                                            if (logicam2[10][ill].type ==
//                                                comp.received_orders_[i].shipments[j].products[k].type) {
//                                                ROS_INFO_STREAM(
//                                                        "\n Printing ILL value: " << ill << "\n Also printing type = "
//                                                                                  << logicam2[10][ill].type
//                                                                                  << "\nAlso, shipment type"
//                                                                                  << comp.received_orders_[i].shipments[j].products[k].type);
//                                                index = ill;
//                                                break;
//                                            }
//                                        }
//                                        ROS_INFO_STREAM("\n AGV camera details: " << logicam2[10][index].pose);
//                                        cam = logicam2[10][index].pose;
//                                        faulty_part = comp.quality_sensor_status1();
//                                    } else if (or_details[i][j][k].agv_id == "agv2") {
//                                        for (auto ill = 0; ill <= on_table_2; ill++) {
//                                            if (logicam2[11][ill].type ==
//                                                comp.received_orders_[i].shipments[j].products[k].type) {
//                                                ROS_INFO_STREAM(
//                                                        "\n Printing ILL value: " << ill << "\n Also printing type = "
//                                                                                  << logicam2[11][ill].type
//                                                                                  << "\nAlso, shipment type"
//                                                                                  << comp.received_orders_[i].shipments[j].products[k].type);
//                                                index = ill;
//                                                break;
//                                            }
//                                        }
//                                        ROS_INFO_STREAM("\n AGV camera details: " << logicam2[11][on_table_2].pose);
//                                        cam = logicam2[11][index].pose;
//                                        faulty_part = comp.quality_sensor_status();
//                                    }
//                                    ros::Duration(0.2).sleep();
//                                    ROS_INFO_STREAM("\n X offset: " << abs(cam.position.x - target_pose.position.x));
//                                    ROS_INFO_STREAM("\n Y offset: " << abs(cam.position.y - target_pose.position.y));
//                                    if (faulty_part.faulty == true) {
//                                        ROS_INFO_STREAM("Faulty Part detected!!!");
//                                        faulty_part.type = my_part.type;
//                                        ROS_INFO_STREAM("\n Trying to compute path for " << faulty_part.type);
//                                        ROS_INFO_STREAM("\n Pose at faulty part " << cam);
//                                        faulty_part.pose = cam;
//                                        faulty_part.pose.position.z -= 0.19;
//                                        ROS_INFO_STREAM("\n Pose for Faulty part " << faulty_part.pose);
//                                        if (or_details[i][j][k].agv_id == "agv2") {
//                                            gantry.goToPresetLocation(gantry.agv2_);
//                                            gantry.pickPart(faulty_part);
//                                            gantry.goToPresetLocation(gantry.agv2_);
//                                        } else if (or_details[i][j][k].agv_id == "agv1") {
//                                            gantry.goToPresetLocation(gantry.agv1_);
//                                            gantry.pickPart(faulty_part);
//                                            gantry.goToPresetLocation(gantry.agv1_);
//                                        }
//                                        gantry.goToPresetLocation(gantry.agv_faulty);
//                                        gantry.deactivateGripper("left_arm");
//                                        continue;
//                                    } else if (abs(cam.position.x - target_pose.position.x) > 0.03 ||
//                                               abs(cam.position.y - target_pose.position.y) > 0.03) {
//                                        if (abs(cam.position.x - target_pose.position.x) > 0.03)
//                                            ROS_INFO_STREAM("\n X offset detected");
//                                        if (abs(cam.position.y - target_pose.position.y) > 0.03)
//                                            ROS_INFO_STREAM("\n Y offset detected");
//                                        ROS_INFO_STREAM("\n Faulty Pose detected for part " << logicam[x][y].type);
//                                        faulty_pose.type = my_part.type;
//                                        ROS_INFO_STREAM("\n Trying to compute path for " << faulty_pose.type);
//                                        ROS_INFO_STREAM("\n Faulty pose " << cam);
//                                        faulty_pose.pose = cam;
//                                        if (or_details[i][j][k].agv_id == "agv2") {
//                                            gantry.goToPresetLocation(gantry.agv2c_);
//                                            ROS_INFO_STREAM("\n Trying to pick up...");
//                                            gantry.pickPart(faulty_pose);
//                                            ROS_INFO_STREAM("\nPart Picked!");
//                                            gantry.goToPresetLocation(gantry.agv2_);
//                                            gantry.placePart(or_details[i][j][k], "agv2");
//                                            ROS_INFO_STREAM("\n Placed!!!");
//                                            on_table_2++;
//                                        } else if (or_details[i][j][k].agv_id == "agv1") {
//                                            gantry.goToPresetLocation(gantry.agv1a_);
//                                            ROS_INFO_STREAM("\n Trying to pick up...");
//                                            gantry.pickPart(faulty_pose);
//                                            ROS_INFO_STREAM("\nPart Picked!");
//                                            gantry.goToPresetLocation(gantry.agv1_);
//                                            gantry.placePart(or_details[i][j][k], "agv1");
//                                            ROS_INFO_STREAM("\n Placed!!!");
//                                            on_table_1++;
//                                        }
//                                    } else {
//                                        if (or_details[i][j][k].agv_id == "agv2")
//                                            on_table_2++;
//                                        else if (or_details[i][j][k].agv_id == "agv1")
//                                            on_table_1++;
//                                    }
//
//                                    auto state = gantry.getGripperState("left_arm");
//                                    if (state.attached)
//                                        gantry.goToPresetLocation(gantry.start_);
//                                    ROS_INFO_STREAM("\n\nDISK part Green placed: " << x);
//                                    or_details_new = comp.getter_part_callback();
//                                    ros::Duration(0.2).sleep();
//                                    ROS_INFO_STREAM(
//                                            "\n Order NEW shipment name 1: " << or_details_new[i + 1][j][k].shipment);
//                                    if (!or_details_new[i + 1][j][k].shipment.empty()) {
//                                        or_details[i + 1] = or_details_new[i + 1];
//                                    }
//                                    count++;
//                                    order_flag[i][j][k] = 1;
//                                    if (k == comp.received_orders_[i].shipments[j].products.size() - 1) {
//                                        completed[i][j] = 1;
//                                        if (or_details[i][j][k].agv_id == "agv1") {
//                                            ROS_INFO_STREAM(
//                                                    "\n Submitting Order: " << or_details_new[i][j][k].shipment);
//                                            submitOrder(1, or_details_new[i][j][k].shipment);
//                                        } else if (or_details[i][j][k].agv_id == "agv2") {
//                                            ROS_INFO_STREAM(
//                                                    "\n Submitting Order: " << or_details_new[i][j][k].shipment);
//                                            submitOrder(2, or_details_new[i][j][k].shipment);
//                                        }
//
//                                    }
//                                    break;
//                                }
//                            }
//                        }
                    }

                }
            }
        }
        gantry.goToPresetLocation(gantry.start_);


        comp.endCompetition();
        spinner.stop();
        ros::shutdown();
        return 0;
    }
}