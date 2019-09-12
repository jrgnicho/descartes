/*
 * graph_solver.cpp
 *
 *  Created on: Aug 30, 2019
 *      Author: jrgnicho
 */

#include <console_bridge/console.h>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include "descartes_planner/graph_solver.h"


static const int VIRTUAL_VERTEX_INDEX = -1;

namespace descartes_planner
{

// explicit especializations
template class GraphSolver<float>;
template class GraphSolver<double>;

template<typename FloatT>
descartes_planner::GraphSolver<FloatT>::GraphSolver(typename EdgeEvaluator<FloatT>::ConstPtr edge_evaluator):
  edge_evaluator_(edge_evaluator)
{

}

template<typename FloatT>
descartes_planner::GraphSolver<FloatT>::~GraphSolver()
{

}

template<typename FloatT>
bool descartes_planner::GraphSolver<FloatT>::build(std::vector<typename PointSampler<FloatT>::Ptr>& points)
{
  points_.clear();
  std::copy(points.begin(),points.end(),std::back_inserter(points_));

  // build the graph now
  typename PointSampleGroup<FloatT>::Ptr samples1 = nullptr;
  typename PointSampleGroup<FloatT>::Ptr samples2 = nullptr;

  auto gather_samples = [](typename PointSampleGroup<FloatT>::Ptr& samples,typename  PointSampler<FloatT>::Ptr sampler) -> bool
  {
    if(samples == nullptr)
    {
      samples = sampler->getSamples();
      return samples!=nullptr;
    }

    // preallocating
    samples->num_samples = sampler->getNumSamples();
    samples->num_dofs = sampler->getDofs();
    if(samples->values.size() < samples->num_samples * samples->num_dofs)
    {
      samples->values.resize(samples->num_samples * samples->num_dofs);
    }

    return sampler->getSamples(samples);
  };

  std::size_t vertex_count = 0;
  bool add_virtual_vertex = true;
  std::map<std::size_t, VertexProperties> src_vertices_added;
  std::map<std::size_t, VertexProperties> dst_vertices_added;

  auto add_vertices = [this](const std::map<std::size_t, VertexProperties>& vertex_map)
  {
    std::size_t num_vertices = boost::num_vertices(graph_);
    for(const std::map<std::size_t, VertexProperties>::value_type& kv : vertex_map)
    {
      if(kv.first >= num_vertices)
      {
        auto v = boost::add_vertex(kv.second,graph_);
        CONSOLE_BRIDGE_logInform("Added vertex %lu",kv.first);
      }
      else
      {
/*        typename GraphT::vertex_descriptor v = boost::vertex(kv.first, graph_);
        graph_[v] = kv.second;
        CONSOLE_BRIDGE_logInform("Updated vertex %lu",kv.first);*/
      }
    }
  };

  // convenience function to add vertices to graph

  // clearing graph
  graph_.clear();

  // adding virtual vertex
  boost::add_vertex(graph_);
  vertex_count++;

  // obtaining samples and building graph now
  for(std::size_t i = 1; i < points_.size(); i++)
  {
    std::size_t p1_idx = i -1;
    std::size_t p2_idx = i;

    typename PointSampler<FloatT>::Ptr sampler1 = points_[p1_idx];
    typename PointSampler<FloatT>::Ptr sampler2 = points_[p2_idx];

    // reuse samples of previous point if they were created
    if(samples2 != nullptr)
    {
      samples1 = samples2;
    }
    else
    {
      if(!gather_samples(samples1,sampler1))
      {
        CONSOLE_BRIDGE_logError("No samples were produced for point 1 with index%lu",p1_idx);
        return false;
      }
    }

    // always recompute samples for the next point
    samples2.reset();
    if(!gather_samples(samples2,sampler2))
    {
      CONSOLE_BRIDGE_logError("No samples were produced for point 2 with index %lu",p2_idx);
      return false;
    }

    samples1->point_id = p1_idx;
    samples2->point_id = p2_idx;

    // validating vertex samples
    using SampleMap = std::map< std::size_t, typename PointSampleGroup<FloatT>::Ptr >;
    SampleMap sample_groups = {{p1_idx, samples1}, {p2_idx, samples2}};
    if(!std::all_of(sample_groups.begin(), sample_groups.end(),[](typename SampleMap::value_type& kv){
      if(kv.second == nullptr)
      {
        CONSOLE_BRIDGE_logError("Invalid samples received for point with index %lu",kv.first);
        return false;
      }
      if(kv.second->values.empty())
      {
        CONSOLE_BRIDGE_logError("No valid samples were found in point %lu",kv.first);
        return false;
      }
      return true;
    }))
    {
      return false;
    }

    // evaluate edges
    using EdgeProp = EdgeProperties<FloatT>;
    std::vector< EdgeProperties<FloatT> > edges = edge_evaluator_->evaluate(samples1, samples2);
    src_vertices_added.clear();
    dst_vertices_added.clear();


    CONSOLE_BRIDGE_logInform("Found %lu edges between nodes (%i, %i)",edges.size(),samples1->point_id ,samples2->point_id );

    // check that at least one is valid
    std::size_t num_valid_edges = std::accumulate(edges.begin(), edges.end(),0,[](std::size_t c, const EdgeProperties<FloatT>& edge){
      return c + (edge.valid ? 1 : 0);
    });
    if(num_valid_edges == 0)
    {
      CONSOLE_BRIDGE_logError("Not a single valid edge was found between points (%lu, %lu)",p1_idx,p2_idx);
      return false;
    }
    else
    {
      CONSOLE_BRIDGE_logInform("Point (%lu, %lu) has %lu valid edges out of %lu",p1_idx,p2_idx,num_valid_edges,edges.size());
    }

    // adding vertices
    int num_vertices = samples1->num_samples;
    for(std::size_t v = 0; v < num_vertices; v++)
    {
      boost::add_vertex(graph_);
    }

    // add edges between the current two nodes
    for(EdgeProp& edge: edges)
    {
      typename GraphT::edge_descriptor e;
      bool added;

      std::size_t src_vtx_index = edge.src_vtx.sample_index + vertex_count;
      std::size_t dst_vtx_index = samples1->num_samples + edge.dst_vtx.sample_index + vertex_count;

      // adding edge to virtual vertex first
      if(add_virtual_vertex && (src_vertices_added.count(src_vtx_index) == 0))
      {
        VertexProperties virtual_vertex_props;
        virtual_vertex_props.point_id = VIRTUAL_VERTEX_INDEX;
        virtual_vertex_props.sample_index = 0;
        EdgeProperties<FloatT> virtual_edge= {.src_vtx = virtual_vertex_props, .dst_vtx = edge.src_vtx,
          .valid = true, .weight = (edge.valid ? 0.0 : std::numeric_limits<FloatT>::infinity())};
        boost::tie(e,added) = boost::add_edge(0, src_vtx_index, graph_);
        graph_[e] = virtual_edge;
        CONSOLE_BRIDGE_logInform("Added edge (0, %lu) to virtual vertex",src_vtx_index);
      }

      // now adding edge
/*      if(edge.valid)
      {
        boost::tie(e,added) = boost::add_edge(src_vtx_index, dst_vtx_index, graph_);
        CONSOLE_BRIDGE_logInform("Added edge (%lu, %lu)",src_vtx_index, dst_vtx_index);
        if(!added)
        {
          CONSOLE_BRIDGE_logWarn("Edge (%lu, %lu) has already been added to the graphs",src_vtx_index,
                                  dst_vtx_index);
          return false;
        }
        else
        {
          // setting edge properties
          graph_[e] = edge;
        }
      }
      */

      boost::tie(e,added) = boost::add_edge(src_vtx_index, dst_vtx_index, graph_);
      CONSOLE_BRIDGE_logInform("Added edge (%lu, %lu)",src_vtx_index, dst_vtx_index);
      if(!added)
      {
        CONSOLE_BRIDGE_logWarn("Edge (%lu, %lu) has already been added to the graphs",src_vtx_index,
                                dst_vtx_index);
        return false;
      }
      else
      {
        // setting edge properties
        graph_[e] = edge;
        graph_[e].weight = (edge.valid ? edge.weight : std::numeric_limits<FloatT>::infinity());
      }

      src_vertices_added[src_vtx_index] = edge.src_vtx;
      dst_vertices_added[dst_vtx_index] = edge.dst_vtx;

      // now adding vertices
      //add_new_vertex(src_vertices_added,src_vtx_index, edge.src_vtx);
      //add_new_vertex(dst_vertices_added,dst_vtx_index,edge.dst_vtx);

    }

    // adding virtual vertex first
/*    if(add_virtual_vertex)
    {
      VertexProperties virtual_vertex_props;
      virtual_vertex_props.point_id = VIRTUAL_VERTEX_INDEX;
      virtual_vertex_props.sample_index = 0;
      typename GraphT::edge_descriptor e;
      bool added;
      for(std::map<std::size_t, VertexProperties>::value_type& kv : src_vertices_added)
      {
        EdgeProperties<FloatT> virtual_edge= {.src_vtx = virtual_vertex_props, .dst_vtx = kv.second,
          .valid = true, .weight = 0.0};

        boost::tie(e,added) = boost::add_edge(0, kv.first, graph_);
        graph_[e] = virtual_edge;
      }
    }*/
    add_virtual_vertex = false; // do not add edges for the virtual vertex anymore

    //add_vertices(src_vertices_added);
    //add_vertices(dst_vertices_added);

    // incrementing counter
    vertex_count += src_vertices_added.size();
  }
  return true;
}

template<typename FloatT>
bool descartes_planner::GraphSolver<FloatT>::solve(
    std::vector<typename PointSampleGroup<FloatT>::ConstPtr>& solution_points)
{
  typename GraphT::vertex_descriptor virtual_vertex = vertex(0, graph_), current_vertex;
  std::size_t num_vert = boost::num_vertices(graph_);
  std::vector<typename GraphT::vertex_descriptor> predecessors(num_vert);
  std::vector<FloatT> weights(num_vert, std::numeric_limits<FloatT>::max());

  boost::dijkstra_shortest_paths(graph_, virtual_vertex,
    weight_map(get(&EdgeProperties<FloatT>::weight, graph_))
    .distance_map(boost::make_iterator_property_map(weights.begin(),get(boost::vertex_index, graph_)))
    .predecessor_map(&predecessors[0]));

  // iterating through out edges while inspecting the predecessor
  typedef boost::graph_traits<GraphT> GraphTraits;

  current_vertex = virtual_vertex;
  solution_points.resize(points_.size(), nullptr);
  bool found_next = false;
  CONSOLE_BRIDGE_logInform("Predecessor array size %lu",predecessors.size());

  for(std::size_t i = 0; i < predecessors.size(); i++)
  {
    std::cout<< "[" << i << "]: " <<predecessors[i]<<std::endl;
  }

  do
  {
    found_next = false;
    typename GraphTraits::out_edge_iterator out_i, out_end;
    for(boost::tie(out_i, out_end)= boost::out_edges(current_vertex, graph_); out_i != out_end; out_i++)
    {
      typename GraphTraits::edge_descriptor e = *out_i;
      current_vertex = boost::source(e, graph_);
      typename GraphT::vertex_descriptor targ = boost::target(e, graph_);
      if(predecessors[targ] == current_vertex)
      {
        std::cout<<"Found edge ("<<current_vertex <<", "<<targ<<")"<<std::endl;
        current_vertex = targ;

        // grab sampler
        EdgeProperties<FloatT> edge_props = graph_[e];
        if(edge_props.src_vtx.point_id == VIRTUAL_VERTEX_INDEX)
        {
          CONSOLE_BRIDGE_logInform("Found virtual vertex, skipping ...");
          found_next = true;
          break;
        }
        else if(edge_props.src_vtx.point_id >= points_.size())
        {
          CONSOLE_BRIDGE_logError("Source vertex index %i exceeds point buffer of size %lu",edge_props.src_vtx.point_id, points_.size());
          break;
        }

        typename PointSampler<FloatT>::Ptr  sampler = points_[edge_props.src_vtx.point_id];

        // recompute or retrieve the sample and storing it
        if(solution_points[edge_props.src_vtx.point_id] != nullptr) // can not have more than one solutions
        {
          CONSOLE_BRIDGE_logError("Sample for point %i has already been assigned", edge_props.src_vtx.point_id);
          break;
        }
        typename PointSampleGroup<FloatT>::Ptr sample = sampler->getSample(edge_props.src_vtx.sample_index);
        solution_points[edge_props.src_vtx.point_id] = sample;
        CONSOLE_BRIDGE_logInform("Added %s solution point %i of %lu points",(sample != nullptr ? "valid" : "null"),
                                 edge_props.src_vtx.point_id, solution_points.size());
        found_next = true;
        break;
      }
    }
  } while(found_next);

  for(std::size_t i = 0; i < solution_points.size(); i++)
  {
    typename PointSampleGroup<FloatT>::ConstPtr sample = solution_points[i];
    if(sample == nullptr)
    {
      CONSOLE_BRIDGE_logError("Invalid solution for point %lu was found",i);
      return false;
    }
  }
  return true;
}

} /* namespace descartes_planner */
