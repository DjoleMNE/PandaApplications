// This example application loads a URDF world file and simulates two robots
// with physics and contact in a Dynamics3D virtual world. A graphics model of it is also shown using 
// Chai3D.

#include "Sai2Model.h"
#include "Sai2Graphics.h"
#include "Sai2Simulation.h"
#include "Sai2Primitives.h"
#include <dynamics3d.h>
#include "redis/RedisClient.h"
#include "timer/LoopTimer.h"

#include <GLFW/glfw3.h> //must be loaded after loading opengl/glew

#include <iostream>
#include <string>

#include <signal.h>
bool fSimulationRunning = false;
void sighandler(int){fSimulationRunning = false;}

using namespace std;
using namespace Eigen;

const string world_file = "./resources/world.urdf";
const string robot_file = "./resources/panda_no_truck.urdf";
const string robot_name = "TWO_ARM_PANDA";
const string camera_name = "camera_fixed";

const vector<string> object_names = {
	"lone_cable",
};
const int n_objects = object_names.size();
vector<Vector3d> object_positions;
vector<Quaterniond> object_orientations;
// redis keys:
// - write:
const string TIMESTAMP_KEY = "sai2::PandaApplication::simulation::timestamp";
std::string JOINT_ANGLES_KEY  = "sai2::PandaApplication::sensors::q";
std::string JOINT_VELOCITIES_KEY = "sai2::PandaApplication::sensors::dq";

const std::string LEFT_GRIPPER_MODE_KEY  = "sai2::PandaApplication::gripper::left::mode"; // m for move and g for graps
const std::string LEFT_GRIPPER_CURRENT_WIDTH_KEY  = "sai2::PandaApplication::gripper::left::current_width";
const std::string LEFT_GRIPPER_DESIRED_WIDTH_KEY  = "sai2::PandaApplication::gripper::left::desired_width";
const std::string LEFT_GRIPPER_DESIRED_SPEED_KEY  = "sai2::PandaApplication::gripper::left::desired_speed";
const std::string LEFT_GRIPPER_DESIRED_FORCE_KEY  = "sai2::PandaApplication::gripper::left::desired_force";

const string FIX_OBJECT_KEY = "sai2::PandaApplication::fix_object";
bool fix_object = false;
bool fix_object_init = true;

// - read
const std::string TORQUES_COMMANDED_KEY  = "sai2::PandaApplication::actuators::fgc";

RedisClient* redis_client;

double gripper_desired_width = 0.02;
double gripper_desired_speed = 0.07;
double gripper_desired_force = 0.0;
string gripper_mode = "m";

// simulation function prototype
const double sim_frequency = 1000.0;
void simulation(Sai2Model::Sai2Model* robot, Simulation::Sai2Simulation* sim);
// void control(Sai2Model::Sai2Model* robot, Simulation::Sai2Simulation* sim);

Vector2d grippersControl(Sai2Model::Sai2Model* robot, 
		const int joint_index_1, 
		const int joint_index_2, 
		const string mode_redis_key,
		const string current_width_redis_key,
		const string desired_width_redis_key,
		const string desired_speed_redis_key,
		const string desired_force_redis_key);

// callback to print glfw errors
void glfwError(int error, const char* description);

// callback when a key is pressed
void keySelect(GLFWwindow* window, int key, int scancode, int action, int mods);

// callback when a mouse button is pressed
void mouseClick(GLFWwindow* window, int button, int action, int mods);

// flags for scene camera movement
bool fTransXp = false;
bool fTransXn = false;
bool fTransYp = false;
bool fTransYn = false;
bool fTransZp = false;
bool fTransZn = false;
bool fshowCameraPose = false;
bool fRotPanTilt = false;

int main() {
	cout << "Loading URDF world model file: " << world_file << endl;

	// start redis client
	redis_client = new RedisClient();
	redis_client->connect();

	// load graphics scene
	auto graphics = new Sai2Graphics::Sai2Graphics(world_file, false);
	Eigen::Vector3d camera_pos, camera_lookat, camera_vertical;
	graphics->getCameraPose(camera_name, camera_pos, camera_vertical, camera_lookat);
	graphics->_world->setBackgroundColor(160.0/255.0, 187.0/255.0, 232.0/255.0);
	// graphics->_world->setUseCulling(true,true);
	graphics->getCamera(camera_name)->setClippingPlanes(0.01, 50.0);
	// graphics->getCamera(camera_name)->setUseMultipassTransparency(true);

	// load robots
	Affine3d T_world_robot = Affine3d::Identity();
	T_world_robot.translation() << 1.3, -0.3, 7;
	T_world_robot.linear() = AngleAxisd(M_PI, Vector3d::UnitZ()).toRotationMatrix();
	auto robot = new Sai2Model::Sai2Model(robot_file, false, T_world_robot);
	robot->updateKinematics();

	// load simulation world
	auto sim = new Simulation::Sai2Simulation(world_file, false);
	sim->setCollisionRestitution(0);
	sim->setCoeffFrictionStatic(15.0);

	// read joint positions, velocities, update model
	sim->getJointPositions(robot_name, robot->_q);
	sim->getJointVelocities(robot_name, robot->_dq);
	robot->updateKinematics();

	// read objects initial positions
	for(int i=0 ; i< n_objects ; i++)
	{
		Vector3d obj_pos = Vector3d::Zero();
		Quaterniond obj_ori = Quaterniond::Identity();
		sim->getObjectPosition(object_names[i], obj_pos, obj_ori);
		object_positions.push_back(obj_pos);
		object_orientations.push_back(obj_ori);
	}

	/*------- Set up visualization -------*/
	// set up error callback
	glfwSetErrorCallback(glfwError);

	// initialize GLFW
	glfwInit();

	// retrieve resolution of computer display and position window accordingly
	GLFWmonitor* primary = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(primary);

	// information about computer screen and GLUT display window
	int screenW = mode->width;
	int screenH = mode->height;
	int windowW = 0.8 * screenH;
	int windowH = 0.5 * screenH;
	int windowPosY = (screenH - windowH) / 2;
	int windowPosX = windowPosY;

	// create window and make it current
	glfwWindowHint(GLFW_VISIBLE, 0);
	GLFWwindow* window = glfwCreateWindow(windowW, windowH, "SAI2.0 - PandaApplications", NULL, NULL);
	glfwSetWindowPos(window, windowPosX, windowPosY);
	glfwShowWindow(window);
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);

	// set callbacks
	glfwSetKeyCallback(window, keySelect);
	glfwSetMouseButtonCallback(window, mouseClick);

	// cache variables
	double last_cursorx, last_cursory;

	if(!redis_client->exists(JOINT_ANGLES_KEY))
	{
		redis_client->setEigenMatrixJSON(JOINT_ANGLES_KEY, VectorXd::Zero(robot->dof()));
	}
	if(!redis_client->exists(JOINT_VELOCITIES_KEY))
	{
		redis_client->setEigenMatrixJSON(JOINT_VELOCITIES_KEY, VectorXd::Zero(robot->dof()));
	}

	fSimulationRunning = true;
	thread sim_thread(simulation, robot, sim);
	// thread control_thread(control, robot, sim);

	// while window is open:
	while (!glfwWindowShouldClose(window))
	{
		// robot->updateKinematics();

		// update graphics. this automatically waits for the correct amount of time
		int width, height;
		glfwGetFramebufferSize(window, &width, &height);
		graphics->updateGraphics(robot_name, robot);
		// graphics->updateObjectGraphics(object_name, object_position, object_orientation);
		for(int i=0 ; i< n_objects ; i++)
		{
			graphics->updateObjectGraphics(object_names[i], object_positions[i], object_orientations[i]);
		}
		graphics->render(camera_name, width, height);

		// swap buffers
		glfwSwapBuffers(window);

		// wait until all GL commands are completed
		glFinish();

		// check for any OpenGL errors
		GLenum err;
		err = glGetError();
		assert(err == GL_NO_ERROR);

		// poll for events
		glfwPollEvents();

		// move scene camera as required
		graphics->getCameraPose(camera_name, camera_pos, camera_vertical, camera_lookat);
		Eigen::Vector3d cam_depth_axis;
		cam_depth_axis = camera_lookat - camera_pos;
		cam_depth_axis.normalize();
		Eigen::Vector3d cam_up_axis;
		// cam_up_axis = camera_vertical;
		// cam_up_axis.normalize();
		cam_up_axis << 0.0, 0.0, 1.0; //TODO: there might be a better way to do this
		Eigen::Vector3d cam_roll_axis = (camera_lookat - camera_pos).cross(cam_up_axis);
		cam_roll_axis.normalize();
		Eigen::Vector3d cam_lookat_axis = camera_lookat;
		cam_lookat_axis.normalize();
		if (fTransXp) {
			camera_pos = camera_pos + 0.05*cam_roll_axis;
			camera_lookat = camera_lookat + 0.05*cam_roll_axis;
		}
		if (fTransXn) {
			camera_pos = camera_pos - 0.05*cam_roll_axis;
			camera_lookat = camera_lookat - 0.05*cam_roll_axis;
		}
		if (fTransYp) {
			// camera_pos = camera_pos + 0.05*cam_lookat_axis;
			camera_pos = camera_pos + 0.05*camera_vertical;
			camera_lookat = camera_lookat + 0.05*camera_vertical;
		}
		if (fTransYn) {
			// camera_pos = camera_pos - 0.05*cam_lookat_axis;
			camera_pos = camera_pos - 0.05*camera_vertical;
			camera_lookat = camera_lookat - 0.05*camera_vertical;
		}
		if (fTransZp) {
			camera_pos = camera_pos + 0.1*cam_depth_axis;
			camera_lookat = camera_lookat + 0.1*cam_depth_axis;
		}	    
		if (fTransZn) {
			camera_pos = camera_pos - 0.1*cam_depth_axis;
			camera_lookat = camera_lookat - 0.1*cam_depth_axis;
		}
		if (fshowCameraPose) {
			cout << endl;
			cout << "camera position : " << camera_pos.transpose() << endl;
			cout << "camera lookat : " << camera_lookat.transpose() << endl;
			cout << endl;
		}
		if (fRotPanTilt) {
			// get current cursor position
			double cursorx, cursory;
			glfwGetCursorPos(window, &cursorx, &cursory);
			//TODO: might need to re-scale from screen units to physical units
			double compass = 0.006*(cursorx - last_cursorx);
			double azimuth = 0.006*(cursory - last_cursory);
			double radius = (camera_pos - camera_lookat).norm();
			Eigen::Matrix3d m_tilt; m_tilt = Eigen::AngleAxisd(azimuth, -cam_roll_axis);
			camera_pos = camera_lookat + m_tilt*(camera_pos - camera_lookat);
			Eigen::Matrix3d m_pan; m_pan = Eigen::AngleAxisd(compass, -cam_up_axis);
			camera_pos = camera_lookat + m_pan*(camera_pos - camera_lookat);
		}
		graphics->setCameraPose(camera_name, camera_pos, cam_up_axis, camera_lookat);
		glfwGetCursorPos(window, &last_cursorx, &last_cursory);
	}

	// stop simulation
	fSimulationRunning = false;
	sim_thread.join();
	// control_thread.join();

	// destroy context
	glfwDestroyWindow(window);

	// terminate
	glfwTerminate();

	return 0;
}

//------------------------------------------------------------------------------
void simulation(Sai2Model::Sai2Model* robot, Simulation::Sai2Simulation* sim) {

	int dof = robot->dof();
	int controlled_dof = dof-2;
	VectorXd command_torques_simulation = VectorXd::Zero(dof);
	VectorXd command_torques_control = VectorXd::Zero(controlled_dof);
	VectorXd q_control = VectorXd::Zero(controlled_dof);
	VectorXd dq_control = VectorXd::Zero(controlled_dof);
	redis_client->setEigenMatrixJSON(TORQUES_COMMANDED_KEY, command_torques_control);

	int gripper_index_la_1 = robot->jointId("left_arm_finger_joint1");
	int gripper_index_la_2 = robot->jointId("left_arm_finger_joint2");

	redis_client->set(FIX_OBJECT_KEY,"0");

	redis_client->set(LEFT_GRIPPER_DESIRED_WIDTH_KEY, to_string(0.04));
	redis_client->set(LEFT_GRIPPER_DESIRED_SPEED_KEY, to_string(0));
	redis_client->set(LEFT_GRIPPER_DESIRED_FORCE_KEY, to_string(0));
	redis_client->set(LEFT_GRIPPER_MODE_KEY, "m");

	vector<int> controller_handled_joints;
	for(int i=0 ; i<dof ; i++)
	{
		if( i != gripper_index_la_1 && 
			i != gripper_index_la_2)
		{
			controller_handled_joints.push_back(i);
		}
	}

	// pos of left hand
	Vector3d init_pos_object = Vector3d::Zero();
	Vector3d curr_pos_object = Vector3d::Zero();
	Vector3d init_pos_lh = Vector3d::Zero();
	Vector3d curr_pos_lh = Vector3d::Zero();
	Quaterniond init_rot_object = Quaterniond(1,0,0,0);
	Quaterniond curr_rot_object = Quaterniond(1,0,0,0);
	Quaterniond init_rot_lh = Quaterniond(1,0,0,0);
	Quaterniond curr_rot_lh = Quaterniond(1,0,0,0);

	Matrix3d tmp_rot = Matrix3d::Identity();

	unsigned long long simulation_counter = 0;

	// create a timer
	LoopTimer timer;
	timer.initializeTimer();
	timer.setLoopFrequency(sim_frequency); 
	bool fTimerDidSleep = true;
	double start_time = timer.elapsedTime(); //secs
	double last_time = start_time;


	while (fSimulationRunning) {
		fTimerDidSleep = timer.waitForNextLoop();

		if(redis_client->get(FIX_OBJECT_KEY) == "1")
		{
			fix_object = true;
			if(fix_object_init)
			{
				init_pos_object = object_positions[0];
				init_rot_object = object_orientations[0];
				robot->positionInWorld(init_pos_lh, "left_arm_link7", Vector3d::Zero());
				robot->rotationInWorld(tmp_rot, "left_arm_link7");
				init_rot_lh = Quaterniond(tmp_rot);
			}
			fix_object_init = false;
			robot->positionInWorld(curr_pos_lh, "left_arm_link7", Vector3d::Zero());
			robot->rotationInWorld(tmp_rot, "left_arm_link7");
			curr_rot_lh = Quaterniond(tmp_rot);

			curr_pos_object = curr_pos_lh + (curr_rot_lh * init_rot_lh.inverse()).toRotationMatrix() * (init_pos_object - init_pos_lh);
			curr_rot_object = curr_rot_lh * init_rot_lh.inverse() * init_rot_object;

			sim->setObjectPosition(object_names[0], curr_pos_object, curr_rot_object);

			// if(simulation_counter % 500 == 0)
			// {
			// 	cout << "current rotation :\n" << curr_rot_lh.toRotationMatrix() << endl;
			// 	cout << "diff rot in world :\n" << (curr_rot_lh * init_rot_lh.inverse()).toRotationMatrix() << endl << endl;
			// 	cout << "init rot object :\n" << init_rot_object.toRotationMatrix() << endl;
			// }

		}

		// read arm torques from redis
		command_torques_control = redis_client->getEigenMatrixJSON(TORQUES_COMMANDED_KEY);
		for(int i=0 ; i<controlled_dof ; i++)
		{
			command_torques_simulation(controller_handled_joints[i]) = command_torques_control(i); 
		}

		// // compute gripper torques
		Vector2d left_gripper_torques = grippersControl(robot, gripper_index_la_1, gripper_index_la_2, 
														LEFT_GRIPPER_MODE_KEY, LEFT_GRIPPER_CURRENT_WIDTH_KEY,
														LEFT_GRIPPER_DESIRED_WIDTH_KEY, LEFT_GRIPPER_DESIRED_SPEED_KEY, LEFT_GRIPPER_DESIRED_FORCE_KEY);

		command_torques_simulation(gripper_index_la_1) = left_gripper_torques(0);
		command_torques_simulation(gripper_index_la_2) = left_gripper_torques(1);

		// set torques to simulation
		sim->setJointTorques(robot_name, command_torques_simulation);

		// integrate forward
		double curr_time = timer.elapsedTime();
		double loop_dt = curr_time - last_time; 
		sim->integrate(1/sim_frequency);
		// sim->integrate(loop_dt);

		// read joint positions, velocities, update model
		sim->getJointPositions(robot_name, robot->_q);
		sim->getJointVelocities(robot_name, robot->_dq);
		robot->updateKinematics();

		// get object positions from simulation
		for(int i=0 ; i< n_objects ; i++)
		{
			sim->getObjectPosition(object_names[i], object_positions[i], object_orientations[i]);
		}

		for(int i=0 ; i<controlled_dof ; i++)
		{
			q_control(i) = robot->_q(controller_handled_joints[i]);
			dq_control(i) = robot->_dq(controller_handled_joints[i]);
		}

		// write new robot state to redis
		redis_client->setEigenMatrixJSON(JOINT_ANGLES_KEY, q_control);
		redis_client->setEigenMatrixJSON(JOINT_VELOCITIES_KEY, dq_control);
		redis_client->set(TIMESTAMP_KEY, to_string(curr_time));

		//update last time
		last_time = curr_time;

		simulation_counter++;
	}

	// gripper_thread.join();

	double end_time = timer.elapsedTime();
	std::cout << "\n";
	std::cout << "Simulation Loop run time  : " << end_time << " seconds\n";
	std::cout << "Simulation Loop updates   : " << timer.elapsedCycles() << "\n";
	std::cout << "Simulation Loop frequency : " << timer.elapsedCycles()/end_time << "Hz\n";
}

//------------------------------------------------------------------------------
Vector2d grippersControl(Sai2Model::Sai2Model* robot, 
		const int joint_index_1, 
		const int joint_index_2, 
		const string mode_redis_key,
		const string current_width_redis_key,
		const string desired_width_redis_key,
		const string desired_speed_redis_key,
		const string desired_force_redis_key)
{
	Vector2d gripper_torques = Vector2d::Zero();

	double max_width = 0.08;

	// controller gains and force
	double kp_gripper = 400.0;
	double kv_gripper = 30.0;
	double gripper_behavior_force = 0;

	// gripper state
	double gripper_center_point = (robot->_q(joint_index_1) + robot->_q(joint_index_2))/2.0;
	double gripper_center_point_velocity = (robot->_dq(joint_index_1) + robot->_dq(joint_index_2))/2.0;

	double gripper_width = (robot->_q(joint_index_1) - robot->_q(joint_index_2))/2;
	double gripper_opening_speed = (robot->_dq(joint_index_1) - robot->_dq(joint_index_2))/2;

	redis_client->set(current_width_redis_key, to_string(gripper_width));

	// compute gripper torques
	if(!fix_object)
	{
		gripper_desired_width = stod(redis_client->get(desired_width_redis_key));
		gripper_desired_speed = stod(redis_client->get(desired_speed_redis_key));
		gripper_desired_force = stod(redis_client->get(desired_force_redis_key));
		gripper_mode = redis_client->get(mode_redis_key);
	}
	if(gripper_desired_width > max_width)
	{
		gripper_desired_width = max_width;
		redis_client->setCommandIs(desired_width_redis_key, std::to_string(max_width));
		std::cout << "WARNING : Desired gripper " << joint_index_1 << " " << joint_index_2 << " width higher than max width. saturating to max width\n" << std::endl;
	}
	if(gripper_desired_width < 0)
	{
		gripper_desired_width = 0;
		redis_client->setCommandIs(desired_width_redis_key, std::to_string(0));
		std::cout << "WARNING : Desired gripper " << joint_index_1 << " " << joint_index_2 << " width lower than 0. saturating to 0\n" << std::endl;
	}
	if(gripper_desired_speed < 0)
	{
		gripper_desired_speed = 0;
		redis_client->setCommandIs(desired_speed_redis_key, std::to_string(0));
		std::cout << "WARNING : Desired gripper " << joint_index_1 << " " << joint_index_2 << " speed lower than 0. saturating to 0\n" << std::endl;
	} 
	if(gripper_desired_force < 0)
	{
		gripper_desired_force = 0;
		redis_client->setCommandIs(desired_force_redis_key, std::to_string(0));
		std::cout << "WARNING : Desired gripper " << joint_index_1 << " " << joint_index_2 << " force lower than 0. saturating to 0\n" << std::endl;
	}

	double gripper_constraint_force = -200.0*gripper_center_point - 20.0*gripper_center_point_velocity;

	if(gripper_mode == "m")
	{
		gripper_behavior_force = -kp_gripper*(gripper_width - gripper_desired_width) - kv_gripper*(gripper_opening_speed - gripper_desired_speed);
	}
	else if(gripper_mode == "g")
	{
		gripper_behavior_force = -gripper_desired_force;
	}
	else
	{
		cout << "gripper mode not recognized\n" << endl;
	}

	gripper_torques(0) = gripper_constraint_force + gripper_behavior_force;
	gripper_torques(1) = gripper_constraint_force - gripper_behavior_force;

	return gripper_torques;

}

//------------------------------------------------------------------------------

void glfwError(int error, const char* description) {
	cerr << "GLFW Error: " << description << endl;
	exit(1);
}

//------------------------------------------------------------------------------

void keySelect(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	bool set = (action != GLFW_RELEASE);
	switch(key) {
		case GLFW_KEY_ESCAPE:
			// exit application
			glfwSetWindowShouldClose(window,GL_TRUE);
			break;
		case GLFW_KEY_RIGHT:
			fTransXp = set;
			break;
		case GLFW_KEY_LEFT:
			fTransXn = set;
			break;
		case GLFW_KEY_UP:
			fTransYp = set;
			break;
		case GLFW_KEY_DOWN:
			fTransYn = set;
			break;
		case GLFW_KEY_A:
			fTransZp = set;
			break;
		case GLFW_KEY_Z:
			fTransZn = set;
			break;
		case GLFW_KEY_S:
			fshowCameraPose = set;
			break;
		default:
			break;
	}
}

//------------------------------------------------------------------------------

void mouseClick(GLFWwindow* window, int button, int action, int mods) {
	bool set = (action != GLFW_RELEASE);
	//TODO: mouse interaction with robot
	switch (button) {
		// left click pans and tilts
		case GLFW_MOUSE_BUTTON_LEFT:
			fRotPanTilt = set;
			// NOTE: the code below is recommended but doesn't work well
			// if (fRotPanTilt) {
			// 	// lock cursor
			// 	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
			// } else {
			// 	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
			// }
			break;
		// if right click: don't handle. this is for menu selection
		case GLFW_MOUSE_BUTTON_RIGHT:
			//TODO: menu
			break;
		// if middle click: don't handle. doesn't work well on laptops
		case GLFW_MOUSE_BUTTON_MIDDLE:
			break;
		default:
			break;
	}
}

