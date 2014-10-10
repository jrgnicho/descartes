/*
 * Software License Agreement (Apache License)
 *
 * Copyright (c) 2014, Southwest Research Institute
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * planning_graph.cpp
 *
 *  Created on: Jun 5, 2014
 *      Author: Dan Solomon
 */

#include "descartes_trajectory_planning/planning_graph.h"

#include <stdio.h>
#include <iomanip>
#include <iostream>
#include <utility>
#include <algorithm>
#include <fstream>

#include <boost/graph/dijkstra_shortest_paths.hpp>

namespace descartes
{
// TODO: add constructor that takes RobotState as param
//	PlanningGraph::PlanningGraph()
//	{
//
//	}

// TODO: optionally take these as params to the constructor
	bool PlanningGraph::insertGraph(std::vector<CartTrajectoryPt> *points, std::vector<CartesianPointRelationship> *cartesian_point_links)
	{
		// validate input
		if(!points || !cartesian_point_links)
		{
			// one or both are null
			return false;
		}
		if(points->size() == 0 || cartesian_point_links->size() == 0)
		{
			// one or both have 0 elements
			return false;
		}
		if(points->size() != cartesian_point_links->size())
		{
			// must be a one-to-one pairing between cartesian point and it's links
			return false;
		}

		// input is valid, copy to local maps that will be maintained by the planning graph
		for(std::vector<CartTrajectoryPt>::iterator point_iter = points->begin(); point_iter != points->end(); point_iter++)
		{
			trajectory_point_map_[point_iter->getID()] = (*point_iter);
		}
		for(std::vector<CartesianPointRelationship>::iterator link_iter = cartesian_point_links->begin(); link_iter != cartesian_point_links->end(); link_iter++)
		{
			cartesian_point_link_[link_iter->id] = *link_iter;
		}

		// after populating maps above (presumably from cartesian trajectory points), calculate (or query) for all joint trajectories
		if(!calculateJointSolutions())
		{
			// failed to get joint trajectories
			return false;
		}

		// after obtaining joint trajectories, calculate each edge weight between adjacent joint trajectories
		// edge list can be local here, it needs to be passed to populate the graph but not maintained afterwards
		std::list<JointEdge> edges;
		if(!calculateEdgeWeights(&edges))
		{
			// failed to get edge weights
			return false;
		}

		// from list of joint trajectories (vertices) and edges (edges), construct the actual graph
		if(!populateGraph(&edges))
		{
			// failed to create graph
			return false;
		}

		// SUCCESS! now go do something interesting with the graph
		return true;
	}

	bool PlanningGraph::getShortestPathJointToJoint(int start_index, int end_index, double &cost, std::list<int> &path)
	{
		size_t num_vert = boost::num_vertices(dg_);

		// initialize vectors to eb used by dijkstra
		std::vector<int> predecessors(num_vert);
		std::vector<double> weights(num_vert, std::numeric_limits<double>::max());
		std::vector<int> vertex_index_map(num_vert);
		for (size_t i=0; i<vertex_index_map.size(); ++i) {
			vertex_index_map[i] = i;
		}

		dijkstra_shortest_paths(
			dg_,
			start_index,
			weight_map(get(&JointEdge::transition_cost, dg_)).distance_map(boost::make_iterator_property_map(weights.begin(),get(boost::vertex_index, dg_))).predecessor_map(&predecessors[0]));

		// actual weight(cost) from start_index to end_index
		double weight = weights[end_index];
		cost = weight;


		if (weight < std::numeric_limits<double>::max())
		{
			// Add the destination point.
			int current = end_index;
			path.push_front(current);

			// Starting from the destination point step through the predecessor map
			// until the source point is reached.
			while (current != start_index)
			{
				current = predecessors[current];
				path.push_front(current);
			}
		}
		return true;
	}

	// TODO: change this to ROS_DEBUG() or whatever is appropriate
	// TODO: optionally output this to a .DOT file (viewable in GraphVIZ or comparable)
	void PlanningGraph::printGraph()
	{
		std::cout << "GRAPH VERTICES (" << num_vertices(dg_) << "): \n";
		std::pair<VertexIterator, VertexIterator> vi = vertices(dg_);
		for(VertexIterator vert_iter = vi.first; vert_iter != vi.second; ++vert_iter)
		{
			DirectedGraph::vertex_descriptor jv = *vert_iter;
			std::cout << "Vertex: " << dg_[jv].index;
			std::pair<OutEdgeIterator, OutEdgeIterator> ei = out_edges(jv, dg_);
			std::cout << " -> {";
			for(OutEdgeIterator outEdge = ei.first; outEdge != ei.second; ++outEdge)
			{
				DirectedGraph::edge_descriptor e = *outEdge;
				std::cout << dg_[e].joint_end << ", ";
			}
			std::cout << "}\n";
		}

		std::cout << "GRAPH EDGES (" << num_edges(dg_) << "): \n";
		//Tried to make this section more clear, instead of using tie, keeping all
		//the original types so it's more clear what is going on
		std::pair<EdgeIterator, EdgeIterator> ei = edges(dg_);
		for(EdgeIterator edge_iter = ei.first; edge_iter != ei.second; ++edge_iter) {
			DirectedGraph::vertex_descriptor jv = source(*edge_iter, dg_);
			DirectedGraph::edge_descriptor e = *out_edges(jv, dg_).first;

			DirectedGraph::edge_descriptor e2 = *edge_iter;

			std::cout << "(" << source(*edge_iter, dg_) << ", " << target(*edge_iter, dg_) << "): cost: " << dg_[e2].transition_cost <<  "\n";
		}

		std::cout << "\n";
	}

	bool PlanningGraph::calculateJointSolutions()
	{
		if(trajectory_point_map_.size() == 0)
		{
			// no points to check
			return false;
		}

		if( joint_solutions_map_.size() > 0)
		{
			// existing joint solutions... clear the list?
			joint_solutions_map_.clear();
		}
		if( trajectory_point_to_joint_solutions_map_.size() > 0)
		{
			// existing map from trajectorys to joint solutions... clear the list?
			trajectory_point_to_joint_solutions_map_.clear();
		}

		int counter_joint_solutions = 0;
		// for each TrajectoryPt, get the available joint solutions
		for(std::map<int, CartTrajectoryPt>::iterator trajectory_iter = trajectory_point_map_.begin(); trajectory_iter != trajectory_point_map_.end(); trajectory_iter++)
		{
			std::list<int> *traj_solutions = new std::list<int>();
			std::vector<std::vector<double> > joint_poses;
			trajectory_iter->second.getJointPoses(joint_poses, *robot_state_);

			for(std::vector<std::vector<double> >::iterator joint_pose_iter = joint_poses.begin(); joint_pose_iter != joint_poses.end(); joint_pose_iter++)
			{
				traj_solutions->push_back(counter_joint_solutions);
				joint_solutions_map_[counter_joint_solutions] = *joint_pose_iter;

				counter_joint_solutions++;
			}
			trajectory_point_to_joint_solutions_map_[trajectory_iter->first] == *traj_solutions;
		}

		return true;
	}

	bool PlanningGraph::calculateEdgeWeights(std::list<JointEdge> *edges)
	{
		if(trajectory_point_to_joint_solutions_map_.size() == 0)
		{
			// no cartesian->joint mappings
			return false;
		}
		if(cartesian_point_link_.size() == 0)
		{
			// no linkings of cartesian points
			return false;
		}
		if(joint_solutions_map_.size() == 0)
		{
			// no joint solutions to calculate transitions for
			return false;
		}

		for(std::map<int, CartesianPointRelationship>::iterator cart_link_iter = cartesian_point_link_.begin(); cart_link_iter != cartesian_point_link_.end(); cart_link_iter++)
		{
			int start_cart_id = cart_link_iter->first; // should be equal to cart_link_iter->second.id
			int end_cart_id = cart_link_iter->second.id_next;

			std::list<int> start_joint_ids = trajectory_point_to_joint_solutions_map_[start_cart_id];
			std::list<int> end_joint_ids = trajectory_point_to_joint_solutions_map_[end_cart_id];

			for(std::list<int>::iterator start_joint_iter = start_joint_ids.begin(); start_joint_iter != start_joint_ids.end(); start_joint_iter++)
			{
				for(std::list<int>::iterator end_joint_iter = end_joint_ids.begin(); end_joint_iter != end_joint_ids.end(); end_joint_iter++)
				{
					double transition_cost;
					// TODO: Make a call to somewhere that takes to JointTrajectoryPts (std::vector<double>) to get a single weight value back
					std::vector<double> start_joint = joint_solutions_map_[*start_joint_iter];
					std::vector<double> end_joint = joint_solutions_map_[*end_joint_iter];
					transition_cost = RandomDouble(.5, 5.0);
					JointEdge *edge = new JointEdge();
					edge->joint_start = *start_joint_iter;
					edge->joint_end = *end_joint_iter;
					edge->transition_cost = transition_cost;

					edges->push_back(*edge);
				}
			}
		}

		return true;
	}

	bool PlanningGraph::populateGraph(std::list<JointEdge> *edges)
	{
		if(joint_solutions_map_.size() == 0)
		{
			// no joints (vertices)
			return false;
		}
		if(edges->size() == 0)
		{
			// no edges
			return false;
		}

		for(std::map<int, std::vector<double> >::iterator joint_iter = joint_solutions_map_.begin(); joint_iter != joint_solutions_map_.end(); joint_iter++)
		{
			DirectedGraph::vertex_descriptor v = boost::add_vertex(dg_);
			dg_[v].index = joint_iter->first;
		}
		for(std::list<JointEdge>::iterator edge_iter = edges->begin(); edge_iter != edges->end(); edge_iter++)
		{
			DirectedGraph::edge_descriptor e; bool b;
			// add graph links for structure
			boost::tie(e, b) = boost::add_edge(edge_iter->joint_start, edge_iter->joint_end, dg_);
			// populate edge fields
			dg_[e].transition_cost = edge_iter->transition_cost;
			dg_[e].joint_start = edge_iter->joint_start;
			dg_[e].joint_end = edge_iter->joint_end;
		}

		return true;
	}

	// TODO: delete this once a call to get real transition costs is made
	double PlanningGraph::randomDouble(double min, double max)
	{
		double ret = (double)rand() / RAND_MAX;
		return min + ret * (max - min);
	}
} /* namespace descartes */
