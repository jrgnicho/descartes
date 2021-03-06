/*
 * common.h
 *
 *  Created on: Aug 30, 2019
 *      Author: Jorge Nicho
 */

#ifndef INCLUDE_DESCARTES_PLANNER_COMMON_H_
#define INCLUDE_DESCARTES_PLANNER_COMMON_H_

#include <vector>

#include <memory>

#include <console_bridge/console.h>

namespace descartes_planner
{
  template <typename FloatT = float>
  struct PointData
  {

    virtual ~PointData()
    {

    }

    int point_id;
    std::vector<FloatT> values;   /** the values for the point*/

    typedef typename std::shared_ptr<PointData> Ptr;
    typedef typename std::shared_ptr<const PointData> ConstPtr;

    virtual typename PointData<FloatT>::ConstPtr interpolate(FloatT t, typename PointData<FloatT>::ConstPtr p) const
    {
      typename PointData<FloatT>::Ptr sample = std::make_shared<PointData<FloatT>>();
      sample->values.resize(values.size());
      sample->point_id = -1;
      for(std::size_t i = 0; i < values.size(); i++)
      {
        sample->values[i] = values[i] + t*(p->values[i] - values[i]);
      }
      return sample;
    }
  };

  template <typename FloatT = float>
  struct PointSampleGroup
  {
    virtual ~PointSampleGroup(){}

    std::size_t num_samples;      /** the number for samples */
    std::size_t num_dofs;         /** the number of dofs per sample */
    std::vector<FloatT> values;   /** the samples data where each sample contains `num_dofs` elements*/

    int point_id;
    typedef typename std::shared_ptr<PointSampleGroup> Ptr;
    typedef typename std::shared_ptr<const PointSampleGroup> ConstPtr;

    /**
     * @brief gets a single sample
     * @param sample_idx  sub index within sample group
     * @return a pointer to sample group, nullptr when out index is out bounds.
     */
    virtual typename PointData<FloatT>::ConstPtr at(std::size_t sample_idx)
    {
      if(sample_idx >= this->num_samples)
      {
        return nullptr;
      }
      typename PointData<FloatT>::Ptr sample = std::make_shared<PointData<FloatT>>();
      sample->point_id = this->point_id;

      auto start_loc = std::next(this->values.begin(),sample_idx * this->num_dofs);
      auto end_loc = std::next(start_loc, this->num_dofs);
      sample->values.assign(start_loc, end_loc);
      return sample;
    }
  };

  /**
   * @class descartes_planner::PointSampler
   * @brief the base class for trajectory point samples,  actual implementations should
   * know the details of the robot such as ik solvers, joint limits, dofs, etc
   */
  template <typename FloatT = float>
  class PointSampler
  {
  public:
    PointSampler(){}
    virtual ~PointSampler(){ }

    /**
     * @brief this method generates the samples, does not store them
     */
    virtual typename PointSampleGroup<FloatT>::Ptr generate() = 0;

    /**
     * @brief method used by the sparse planner, gets the closets samples to the requested point
     * @param ref_point  The requested point
     * @return  A sample group containing the closest points
     */
    virtual typename PointSampleGroup<FloatT>::Ptr getClosest(typename PointData<FloatT>::ConstPtr ref_point)
    {
      CONSOLE_BRIDGE_logWarn("The function %s has not been implemented", __PRETTY_FUNCTION__);
      return nullptr;
    }

    typedef typename std::shared_ptr<PointSampler<FloatT> > Ptr;
    typedef typename std::shared_ptr<const PointSampler<FloatT> > ConstPtr;
  };

  /**
   * @class descartes_planner::ProxySampler
   * @brief Convenience class that is used to hold a pre-computed sample group
   */
  template<typename FloatT>
  class ProxySampler: public PointSampler<FloatT>
  {
  public:
    /**
     * @brief Default constructor that stores the sample group passed
     * @param sample_group The sample group to store and return through the generate() method
     */
    ProxySampler(typename PointSampleGroup<FloatT>::Ptr sample_group):
      sample_group_(sample_group)
    {

    }

    /**
     * @brief alternative constructor that creates an internal sample group that holds a single sample point
     * @param sample_point  The sample point from which the sample group is created
     */
    ProxySampler(typename PointData<FloatT>::ConstPtr sample_point)
    {
      typename PointSampleGroup<FloatT>::Ptr sample_group = std::make_shared<PointSampleGroup<FloatT>>();
      sample_group->point_id = sample_point->point_id;
      sample_group->num_samples = 1;
      sample_group->num_dofs = sample_point->values.size();
      sample_group->values = sample_point->values;
      sample_group_ = sample_group;
    }

    ~ProxySampler()
    {

    }

    typename PointSampleGroup<FloatT>::Ptr generate() override
    {
      return sample_group_;
    }

  private:
    typename PointSampleGroup<FloatT>::Ptr sample_group_;
  };

  struct VertexProperties
  {
    virtual ~VertexProperties(){}
    std::size_t point_id = 0;
    std::size_t sample_index = 0;
  };

  template <typename FloatT = float>
  struct EdgeProperties
  {
    FloatT weight = 0.0;
    bool valid;
    VertexProperties src_vtx;
    VertexProperties dst_vtx;
  };

  template <typename FloatT = float>
  class EdgeEvaluator
  {
  public:

    virtual ~EdgeEvaluator(){}

    /**
     * @brief evaluates all the edges between the samples of s1 and s2.
     * @param s1  A point sample group with n1 samples
     * @param s2  A point sample group with n2 samples
     * @param exclude_s1 list of indices in sample group 1 to exclude from the evaluation
     * @param exclude_s2 list of indices in sample group 2 to exclude from the evaluation
     * @return  Returns a n1 x n2 vector of EdgeProperties objects
     */
    virtual std::vector< EdgeProperties<FloatT> > evaluate(typename PointSampleGroup<FloatT>::ConstPtr s1,
                                                            typename PointSampleGroup<FloatT>::ConstPtr s2,
                                                            const std::vector<std::size_t>& exclude_s1 = {},
                                                            const std::vector<std::size_t>& exclude_s2 = {}) const = 0;

    typedef typename std::shared_ptr<EdgeEvaluator> Ptr;
    typedef typename std::shared_ptr<const EdgeEvaluator> ConstPtr;
  };

  template <typename FloatT = float>
  class SamplesContainer
  {
  public:
    SamplesContainer(){}
    virtual ~SamplesContainer(){ }

    /**
     * @brief this method should set the size of the internal buffer and clear previous data
     * @param n
     */
    virtual void allocate(std::size_t n) = 0;

    virtual void clear() = 0;

    virtual bool has(std::size_t idx) = 0;

    virtual std::size_t size() = 0;

    virtual typename PointSampleGroup<FloatT>::Ptr& at(std::size_t idx) = 0;

    virtual const typename PointSampleGroup<FloatT>::Ptr& at(std::size_t idx) const = 0;

    virtual typename PointSampleGroup<FloatT>::Ptr& operator[](std::size_t idx) = 0;

    virtual const typename PointSampleGroup<FloatT>::Ptr& operator[](std::size_t idx) const = 0;

  };

}

#endif /* INCLUDE_DESCARTES_PLANNER_COMMON_H_ */
