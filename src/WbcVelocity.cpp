#include "WbcVelocity.hpp"
#include "ExtendedConstraint.hpp"
#include <kdl/utilities/svd_eigen_HH.hpp>
#include <base/logging.h>

using namespace std;

namespace wbc{

WbcVelocity::WbcVelocity() :
    configured_(false),
    no_robot_joints_(0),
    temp_(Eigen::VectorXd(6)){
}

WbcVelocity::~WbcVelocity(){
    clear();
}

void WbcVelocity::clear(){

    for(uint prio = 0; prio < constraint_vector_.size(); prio++ ){
        for(uint j = 0; j < constraint_vector_[prio].size(); j++)
            delete constraint_vector_[prio][j];
        constraint_vector_[prio].clear();
    }

    jac_map_.clear();
    constraint_map_.clear();
    constraint_vector_.clear();
    joint_index_map_.clear();
    no_robot_joints_ = 0;
    task_frame_ids.clear();
}

bool WbcVelocity::configure(const std::vector<ConstraintConfig> &config,
                            const std::vector<std::string> &joint_names){

    //Erase constraints, jacobians and joint indices
    clear();

    no_robot_joints_ = joint_names.size();

    // Create joint index map. This defines the order of the joints in the task Jacobians computed here. Note
    // that the order of joints in the task frames can be different.
    for(uint i = 0; i < joint_names.size(); i++)
        joint_index_map_[joint_names[i]] = i;

    // Create Constraints and sort them by priority
    int max_prio = 0;
    for(uint i = 0; i < config.size(); i++)
    {
        if(config[i].priority < 0)
        {
            LOG_ERROR("Constraint Priorities must be >= 0. Constraint priority of constraint '%s'' is %i", config[i].name.c_str(), config[i].priority);
            return false;
        }
        if(config[i].priority > max_prio)
            max_prio = config[i].priority;
    }
    constraint_vector_.resize(max_prio + 1);
    for(uint i = 0; i < config.size(); i++)
    {
        ExtendedConstraint* constraint = new ExtendedConstraint(config[i], joint_names);
        constraint_vector_[config[i].priority].push_back(constraint);

        //Also put Constraints in a map that associates them with their names (for easier access)
        if(constraint_map_.count(config[i].name) != 0)
        {
            LOG_ERROR("Constraint with name %s already exists! Constraint names must be unique", config[i].name.c_str());
            return false;
        }
        constraint_map_[config[i].name] = constraint;
    }

    //Erase empty priorities
    for(uint prio = 0; prio < constraint_vector_.size(); prio++)
    {
        if(constraint_vector_[prio].empty())
        {
            constraint_vector_.erase(constraint_vector_.begin() + prio, constraint_vector_.begin() + prio + 1);
            prio--;
        }
    }

    n_constraints_per_prio_ = std::vector<int>(constraint_vector_.size(), 0);
    n_prios_ = n_constraints_per_prio_.size();
    for(uint prio = 0; prio < constraint_vector_.size(); prio++)
    {
        for(uint j = 0; j < constraint_vector_[prio].size(); j++)
            n_constraints_per_prio_[prio] += constraint_vector_[prio][j]->y_ref.size();
    }

    //Create TF map
    for(ConstraintMap::iterator it = constraint_map_.begin(); it != constraint_map_.end(); it++)
    {
        if(it->second->config.type == cart){

            std::string tf_name = it->second->config.root;
            if(jac_map_.count(tf_name) == 0)
            {
                KDL::Jacobian jac(joint_index_map_.size());
                jac.data.setZero();
                jac_map_[tf_name] = jac;
                task_frame_ids.push_back(tf_name);
            }

            tf_name = it->second->config.tip;
            if(jac_map_.count(tf_name) == 0)
            {
                KDL::Jacobian jac(joint_index_map_.size());
                jac.data.setZero();
                jac_map_[tf_name] = jac;
                task_frame_ids.push_back(tf_name);
            }

            tf_name = it->second->config.ref_frame;
            if(jac_map_.count(tf_name) == 0)
            {
                KDL::Jacobian jac(joint_index_map_.size());
                jac.data.setZero();
                jac_map_[tf_name] = jac;
                task_frame_ids.push_back(tf_name);
            }
        }
    }

    LOG_DEBUG("Joint Index Map: \n");
    for(JointIndexMap::iterator it = joint_index_map_.begin(); it != joint_index_map_.end(); it++)
        LOG_DEBUG_S<<it->first.c_str()<<": "<<it->second;

    LOG_DEBUG("\nTask Frames: \n");
    for(size_t i = 0; i < task_frame_ids.size(); i++)
        LOG_DEBUG_S<<task_frame_ids[i];

    LOG_DEBUG("\nConstraint Map: \n");
    for(ConstraintMap::iterator it = constraint_map_.begin(); it != constraint_map_.end(); it++){
        if(it->second->config.type == cart)
            LOG_DEBUG_S<<it->first.c_str()<<": prio: "<<it->second->config.priority<<", type: "<<it->second->config.type
                      <<" root: '"<<it->second->config.root<<"'' tip: '"<<it->second->config.tip<<"'' ref frame: '"<<it->second->config.ref_frame<<"'";
        else
            LOG_DEBUG_S<<it->first.c_str()<<": prio: "<<it->second->config.priority<<", type: "<<it->second->config.type;
    }

    configured_ = true;

    return true;
}

Constraint* WbcVelocity::constraint(const std::string &name)
{
    if(!configured_)
        throw std::runtime_error("WbcVelocity::update: Configure has not been called yet");

    if(constraint_map_.count(name) == 0)
    {
        LOG_ERROR("No such constraint: %s", name.c_str());
        throw std::invalid_argument("Invalid constraint name");
    }
    return constraint_map_[name];
}

void WbcVelocity::prepareEqSystem(const TaskFrameMap &task_frames,
                                  std::vector<LinearEquationSystem> &equations)
{
    if(!configured_)
        throw std::runtime_error("WbcVelocity::update: Configure has not been called yet");

    //Check if all required task frames are in input vector
    for(size_t i = 0; i < task_frame_ids.size(); i++)
    {
        if(task_frames.count(task_frame_ids[i]) == 0){
            LOG_ERROR("Wbc config requires Task Frame %s, but this task frame is not in task_frame vector", task_frame_ids[i].c_str());
            throw std::invalid_argument("Incomplete task frame input");
        }
    }

    //update task frame map and create full robot Jacobians
    for(TaskFrameMap::const_iterator it = task_frames.begin(); it != task_frames.end(); it++)
    {
        const std::string& tf_name = it->first;
        const TaskFrame& tf = it->second;

        //IMPORTANT: Fill in columns of task frame Jacobian into the correct place of the full robot Jacobian using the joint_index_map
        for(uint j = 0; j < tf.joint_names.size(); j++)
        {
            const std::string& jt_name =  tf.joint_names[j];

            if(joint_index_map_.count(jt_name) == 0)
            {
                LOG_ERROR("Joint with id %s does not exist in joint index map. Check your joint_names configuration!", jt_name.c_str());
                throw std::invalid_argument("Invalid joint name");
            }

            uint idx = joint_index_map_[jt_name];
            jac_map_[tf_name].setColumn(idx, tf.jacobian.getColumn(j));
        }
    }

    if(equations.size() != n_prios_)
        equations.resize(n_prios_);

    //Walk through all priorities and update equation system
    for(uint prio = 0; prio < n_prios_; prio++)
    {
        uint n_vars_prio = n_constraints_per_prio_[prio]; //Number of constraint variables on the whole priority
        equations[prio].resize(n_vars_prio, no_robot_joints_);

        //Walk through all tasks of current priority
        uint row_index = 0;
        for(uint i = 0; i < constraint_vector_[prio].size(); i++)
        {
            ExtendedConstraint *constraint = constraint_vector_[prio][i];
            const uint n_vars = constraint->no_variables;

            //Check task timeout
            if(constraint->config.timeout > 0){
                double diff = (base::Time::now() - constraint->last_ref_input).toSeconds();

                if(diff > constraint->config.timeout)
                    constraint->constraint_timed_out = 1;
                else
                    constraint->constraint_timed_out = 0;
            }
            else
                constraint->constraint_timed_out = 0;

            if(constraint->config.type == cart)
            {
                const std::string &tf_root_name = constraint->config.root;
                const std::string &tf_tip_name = constraint->config.tip;
                const std::string &tf_ref_name = constraint->config.ref_frame;

                const TaskFrame& tf_root = task_frames.find(tf_root_name)->second;
                const TaskFrame& tf_tip = task_frames.find(tf_tip_name)->second;
                const TaskFrame& tf_ref_frame = task_frames.find(tf_ref_name)->second;

                //Create constraint jacobian
                constraint->pose_tip_in_root = tf_root.pose.Inverse() * tf_tip.pose;
                constraint->jac_helper.data.setIdentity();
                constraint->jac_helper.changeRefPoint(-constraint->pose_tip_in_root.p);
                constraint->jac_helper.changeRefFrame(tf_root.pose);

                //Invert constraint Jacobian
                KDL::svd_eigen_HH(constraint->jac_helper.data, constraint->Uf, constraint->Sf, constraint->Vf, temp_);

                for (unsigned int j = 0; j < constraint->Sf.size(); j++)
                {
                    if (constraint->Sf(j) > 0)
                        constraint->Uf.col(j) *= 1 / constraint->Sf(j);
                    else
                        constraint->Uf.col(j).setZero();
                }
                constraint->H = (constraint->Vf * constraint->Uf.transpose());

                const KDL::Jacobian& jac_root =  jac_map_[tf_root_name];
                const KDL::Jacobian& jac_tip =  jac_map_[tf_tip_name];

                ///// A = J^(-1) *J_tf_tip - J^(-1) * J_tf_root:
                constraint->A = constraint->H.block(0, 0, n_vars, 6) * jac_tip.data
                        -(constraint->H.block(0, 0, n_vars, 6) * jac_root.data);

                //Convert input twist from ref_frame to root of the kinematic chain
                //IMPORTANT: In KDL there are two ways to transform a twist:
                //    - KDL::Frame*KDL::Twist transforms both the reference point in which the twist is expressed AND the reference frame
                //    - KDL::Rotation*KDL::Twist transform only the reference frame!
                //    - We use KDL::Rotation*KDL::Twist here!
                //    - The difference is that with second method, after transformation, the rotational components of the twist will act like a rotation around the origin
                //      of ref_frame, expressed in the root frame of the robot. With the first method the ref_frame would rotate around the root
                //      frame (e.g. like a satellite rotates around earth), which means that rotational components, would produce translational
                //      velocities after transformation to root frame. If the twist has only translational components there is no difference between
                //      the two methods
                constraint->pose_ref_frame_in_root = tf_root.pose.Inverse() * tf_ref_frame.pose;
                KDL::Twist tmp_twist;
                for(uint i = 0; i < 6; i++)
                    tmp_twist(i) = constraint->y_ref(i);
                tmp_twist = constraint->pose_ref_frame_in_root * tmp_twist;
                for(uint i = 0; i < 6; i++)
                    constraint->y_ref_root(i) = tmp_twist(i);
            }
            else if(constraint->config.type == jnt){
                for(uint i = 0; i < constraint->config.joint_names.size(); i++){

                    //Joint space constraints: constraint matrix has only ones and Zeros
                    //IMPORTANT: The joint order in the constraints might be different than in wbc.
                    //Thus, for joint space constraints, the joint indices have to be mapped correctly.
                    const std::string &joint_name = constraint->config.joint_names[i];
                    if(joint_index_map_.count(joint_name) == 0)
                    {
                        LOG_ERROR("Constraint %s contains joint %s, but this joint has not been configured in joint names", constraint->config.name.c_str(), joint_name.c_str());
                        throw std::invalid_argument("Invalid Constraint config");
                    }
                    uint idx = joint_index_map_[joint_name];
                    constraint->A(i,idx) = 1.0;
                    constraint->y_ref_root = constraint->y_ref; // In joint space, y_ref is of yourse equal to y_ref_root
                }
            }

            constraint->time = base::Time::now();

            // If the activation value is zero, also set reference to zero. Activation is usually used to switch between different
            // task phases and we don't want to store the "old" reference value, in case we switch on the constraint again
            if(!constraint->activation){
                constraint->y_ref.setZero();
                constraint->y_ref_root.setZero();
            }
            // Insert constraints into equation system of current priority at the correct position
            equations[prio].W_row.segment(row_index, n_vars) = constraint->weights * constraint->activation * (!constraint->constraint_timed_out);
            equations[prio].A.block(row_index, 0, n_vars, no_robot_joints_) = constraint->A;
            equations[prio].y_ref.segment(row_index, n_vars) = constraint->y_ref_root;

            row_index += n_vars;
        }
    }
}

std::vector<std::string> WbcVelocity::jointNames(){
    std::vector<std::string> joint_names(joint_index_map_.size());
    for(JointIndexMap::iterator it = joint_index_map_.begin(); it != joint_index_map_.end(); it++)
        joint_names[it->second] = it->first;
    return joint_names;
}

void WbcVelocity::getConstraintVector(std::vector<ConstraintsPerPrio>& constraints)
{
    if(constraints.size() != n_prios_)
        constraints.resize(n_prios_);
    for(uint prio = 0; prio < n_prios_; prio++)
    {
        if(constraints[prio].size() != constraint_vector_[prio].size())
            constraints[prio].resize(constraint_vector_[prio].size());

        for(uint i = 0; i < constraints[prio].size(); i++)
            constraints[prio][i] = *constraint_vector_[prio][i];
    }
}

}
