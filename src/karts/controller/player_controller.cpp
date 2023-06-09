//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2004-2015 Steve Baker <sjbaker1@airmail.net>
//  Copyright (C) 2006-2015 Joerg Henrichs, Steve Baker
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "karts/controller/player_controller.hpp"

#include "config/user_config.hpp"
#include "input/input_manager.hpp"
#include "items/attachment.hpp"
#include "items/item.hpp"
#include "items/powerup.hpp"
#include "karts/abstract_kart.hpp"
#include "karts/kart_properties.hpp"
#include "karts/skidding.hpp"
#include "karts/rescue_animation.hpp"
#include "modes/world.hpp"
#include "network/game_setup.hpp"
#include "network/rewind_manager.hpp"
#include "network/network_config.hpp"
#include "network/network_player_profile.hpp"
#include "network/network_string.hpp"
#include "race/history.hpp"
#include "states_screens/race_gui_base.hpp"
#include "utils/constants.hpp"
#include "utils/log.hpp"
#include "utils/string_utils.hpp"
#include "utils/translation.hpp"
#include "modes/soccer_world.hpp"

#include <cstdlib>

#include <cmath>
#include <fstream>
#include <chrono>  // for std::chrono::seconds
#include <thread>  // for std::this_thread::sleep_for
using namespace std;

PlayerController::PlayerController(AbstractKart *kart)
    : Controller(kart), jump_value1(8.0),
      jump_value2(8.0),
      jump_value3_sta(-2.5),
      jump_value3_mov(-6.0),
      jump_value4_sta(0.5),
      jump_value4_mov(1.0),
      jump_distance_threshold_static(25.0),
      jump_distance_threshold_moving(18.0),
      jump_distance_max_fastjumping(11.0),
      jump_karty_threshold(3.0),
      jump_ball_height_min(2.0),
      jump_ball_height_max(20.0),
      jump_ball_vel_min(10.0),
      jump_limit(70)
{
    m_penalty_ticks = 0;
}   // PlayerController

//-----------------------------------------------------------------------------
/** Destructor for a player kart.
 */
PlayerController::~PlayerController()
{
}   // ~PlayerController


void read_values(std::string filename, std::initializer_list<float*> store_target, std::string documentation){
    fstream file;

    file.open(filename, ios::in); // Open file for reading

    if (file.good())  {
        for (float* target: store_target){
            file >> *target;
        }
    }
    else
    {
        file.open(filename, ios::out);                                                                                                                    // Create file if it does not exist
        std::string separator = "";
        for (float* target: store_target){
            file << *target;
            separator = " ";
        }
        file << endl;
        file << documentation;
    }
    file.close();
}

//-----------------------------------------------------------------------------
/** Resets the player kart for a new or restarted race.
 */
void PlayerController::reset()
{
    m_steer_val_l   = 0;
    m_steer_val_r   = 0;
    m_steer_val     = 0;
    m_prev_brake    = 0;
    m_prev_accel    = 0;
    m_prev_nitro    = false;
    m_penalty_ticks = 0;

    read_values("jumpheight.txt", {&jump_value1, &jump_value2}, "The first value represents the jump height when the kart is static, while second value represents the jump height when the kart is moving");
    read_values("jumplimit.txt", {&jump_limit}, "This value represents the maximum jump height the kart can reach before further jumping is disabled");
    read_values("dist.txt", {&jump_distance_threshold_static, &jump_distance_threshold_moving,  &jump_distance_max_fastjumping,  &jump_karty_threshold}, 
        "The first value represents the min distance between the ball and the kart for the ball interception to work when the kart is static, the second value represents the min distance between the ball and the kart for the ball interception to work when the kart is moving, the third value is the distance_max_fastjumping.The last value represents max height of the kart for interception to work"); 
    read_values("ball.txt",{&jump_ball_height_min, &jump_ball_height_max, &jump_ball_vel_min},"This value represents the min and max ball height for interception to work. The last is the ball_vel_min for fast jumping to work.");
    read_values("yoffset.txt", {&jump_value3_sta, &jump_value3_mov}, "The 1st for stationary and the 2nd for moving kart. These values are a fixing for the projectile equations. It is added to the height of the ball at the moment of interception. It is correlated to the value in tim2.txt");
    read_values("tim2.txt", {&jump_value4_sta, &jump_value4_mov}, "The 1st for sta and the 2nd for moving. These values represent the time (in seconds) it takes the kart to intercept the ball. It is correlated with the value in yoffset.txt");

}   // reset

// ----------------------------------------------------------------------------
/** Resets the state of control keys. This is used after the in-game menu to
 *  avoid that any keys pressed at the time the menu is opened are still
 *  considered to be pressed.
 */
void PlayerController::resetInputState()
{
    m_steer_val_l           = 0;
    m_steer_val_r           = 0;
    m_steer_val             = 0;
    m_prev_brake            = 0;
    m_prev_accel            = 0;
    m_prev_nitro            = false;
    m_controls->reset();
}   // resetInputState

// ----------------------------------------------------------------------------
/** This function interprets a kart action and value, and set the corresponding
 *  entries in the kart control data structure. This function handles esp.
 *  cases like 'press left, press right, release right' - in this case after
 *  releasing right, the steering must switch to left again. Similarly it
 *  handles 'press left, press right, release left' (in which case still
 *  right must be selected). Similarly for braking and acceleration.
 *  This function can be run in two modes: first, if 'dry_run' is set,
 *  it will return true if this action will cause a state change. This
 *  is sued in networking to avoid sending events to the server (and then
 *  to other clients) if they are just (e.g. auto) repeated events/
 *  \param action  The action to be executed.
 *  \param value   If 32768, it indicates a digital value of 'fully set'
 *                 if between 1 and 32767, it indicates an analog value,
 *                 and if it's 0 it indicates that the corresponding button
 *                 was released.
 *  \param dry_run If set, it will only test if the parameter will trigger
 *                 a state change. If not set, the appropriate actions
 *                 (i.e. input state change) will be done.
 *  \return        If dry_run is set, will return true if this action will
 *                 cause a state change. If dry_run is not set, will return
 *                 false.
 */
bool PlayerController::action(PlayerAction action, int value, bool dry_run)
{
    /** If dry_run (parameter) is true, this macro tests if this action would
     *  trigger a state change in the specified variable (without actually
     *  doing it). If it will trigger a state change, the macro will
     *  immediatley return to the caller. If dry_run is false, it will only
     *  assign the new value to the variable (and not return to the user
     *  early). The do-while(0) helps using this macro e.g. in the 'then'
     *  clause of an if statement. */

    SoccerWorld *soccer_world = dynamic_cast<SoccerWorld*>(World::getWorld());

    float kart_y, kart_x, kart_z, ball_x, ball_y, ball_z, kart_vx, kart_vz;

#define SET_OR_TEST(var, value)                \
    do                                         \
    {                                          \
        if(dry_run)                            \
        {                                      \
            if (var != (value) ) return true;  \
        }                                      \
        else                                   \
        {                                      \
            var = value;                       \
        }                                      \
    } while(0)

    /** Basically the same as the above macro, but is uses getter/setter
     *  functions. The name of the setter/getter is set'name'(value) and
     *  get'name'(). */
#define SET_OR_TEST_GETTER(name, value)                           \
    do                                                            \
    {                                                             \
        if(dry_run)                                               \
        {                                                         \
            if (m_controls->get##name() != (value) ) return true; \
        }                                                         \
        else                                                      \
        {                                                         \
            m_controls->set##name(value);                         \
        }                                                         \
    } while(0)

    switch (action)
    {
    case PA_STEER_LEFT:
        SET_OR_TEST(m_steer_val_l, value);
        if (value)
        {
            SET_OR_TEST(m_steer_val, value);
            if (m_controls->getSkidControl() == KartControl::SC_NO_DIRECTION)
                SET_OR_TEST_GETTER(SkidControl, KartControl::SC_LEFT);
        }
        else
            SET_OR_TEST(m_steer_val, m_steer_val_r);
        break;
    case PA_STEER_RIGHT:
        SET_OR_TEST(m_steer_val_r, -value);
        if (value)
        {
            SET_OR_TEST(m_steer_val, -value);
            if (m_controls->getSkidControl() == KartControl::SC_NO_DIRECTION)
                SET_OR_TEST_GETTER(SkidControl, KartControl::SC_RIGHT);
        }
        else
            SET_OR_TEST(m_steer_val, m_steer_val_l);

        break;
    case PA_ACCEL:
    {
        uint16_t v16 = (uint16_t)value;
        SET_OR_TEST(m_prev_accel, v16);
        if (v16)
        {
            SET_OR_TEST_GETTER(Accel, v16 / 32768.0f);
            SET_OR_TEST_GETTER(Brake, false);
            SET_OR_TEST_GETTER(Nitro, m_prev_nitro);
        }
        else
        {
            SET_OR_TEST_GETTER(Accel, 0.0f);
            SET_OR_TEST_GETTER(Brake, m_prev_brake);
            SET_OR_TEST_GETTER(Nitro, false);
        }
        break;
    }
    case PA_BRAKE:
        SET_OR_TEST(m_prev_brake, value!=0);
        // let's consider below that to be a deadzone
        if(value > 32768/2)
        {
            SET_OR_TEST_GETTER(Brake, true);
            SET_OR_TEST_GETTER(Accel, 0.0f);
            SET_OR_TEST_GETTER(Nitro, false);
        }
        else
        {
            SET_OR_TEST_GETTER(Brake, false);
            SET_OR_TEST_GETTER(Accel, m_prev_accel/32768.0f);
            // Nitro still depends on whether we're accelerating
            SET_OR_TEST_GETTER(Nitro, m_prev_nitro && m_prev_accel);
        }
        break;
    case PA_NITRO:
        // This basically keeps track whether the button still is being pressed
        SET_OR_TEST(m_prev_nitro, value != 0 );
        // Enable nitro only when also accelerating
        SET_OR_TEST_GETTER(Nitro, ((value!=0) && m_controls->getAccel()) );

        kart_vx =  m_kart->getBody()->getLinearVelocity ().x();
        kart_vz = m_kart->getBody()->getLinearVelocity ().z();

        //get kart position
        kart_y = m_kart->getXYZ().getY(); //elevation
        kart_x = m_kart->getXYZ().getX();
        kart_z = m_kart->getXYZ().getZ();

        //get ball position
        ball_y = soccer_world->getBallPosition().getY(); //elevation
        ball_x = soccer_world->getBallPosition().getX();
        ball_z = soccer_world->getBallPosition().getZ();

        //press nitro while not accelerating and kart at rest to jump high
   //     if ((!m_controls->getAccel()) && (fabsf(kart_vx)<0.2f) && (fabsf(kart_vz)<0.2f) &&  (sqrt(pow(kart_x - ball_x, 2) + pow(kart_z - ball_z, 2)) < distance_threshold_static) && (ball_y > ball_height_min) && ((ball_y - kart_y) < 70))
      //  {

            //set the kart's linear velocity to jump
        //    if (kart_y < jump_limit )
        //    m_kart->getBody()->setLinearVelocity(Vec3(kart_vx, kart_y+value1, kart_vz));


      //  if (!m_kart->isOnGround() )
    //    {


            //get kart position
       //     const Vec3 kartPos = m_kart->getXYZ();

            //get ball position and velocity
     //       const Vec3 ballPos = soccer_world->getBallPosition();
       //     const Vec3 ballVel = soccer_world->getBallVelocity();

            //Set the time it takes for the kart to reach the ball
     //       const float g = -9.8f;
     //       const float t = value4_sta; //time for kart to intercept the ball

            //calculate the x and z velocities required for the kart to intercept the ball
    //        const float vx = (ballVel.getX()*t + ballPos.getX() - kartPos.getX()) / t;
    //        const float vz = (ballVel.getZ()*t + ballPos.getZ() - kartPos.getZ()) / t;
     //       const float vy = (ballVel.getY()*t + ballPos.getY() - kartPos.getY()) / t - g*t + value3_sta;

            //set the kart's linear velocity to intercept the ball
   //         m_kart->getBody()->setLinearVelocity(Vec3(vx, vy, vz));



    //    }
      //  }
//use else if, if above not commented
        if ((!m_controls->getAccel()) &&  (sqrt(pow(kart_x - ball_x, 2) + pow(kart_z - ball_z, 2)) < jump_distance_threshold_moving) && ((ball_y - kart_y) < jump_ball_height_max) && ((ball_y - kart_y) > jump_ball_height_min) && (kart_y < jump_karty_threshold))
        {

            //get kart elevation position
            kart_y = m_kart->getXYZ().getY();



            //set the kart's linear velocity to jump
            if (kart_y < jump_limit )
            m_kart->getBody()->setLinearVelocity(Vec3(kart_vx, kart_y+jump_value2, kart_vz));



        if (!m_kart->isOnGround()  )
        {




            //get kart position
            const Vec3 kartPos = m_kart->getXYZ();

            //get ball position and velocity
            const Vec3 ballPos = soccer_world->getBallPosition();
            const Vec3 ballVel = soccer_world->getBallVelocity();

            //Set the time it takes for the kart to reach the ball
            const float g = -9.8f;

            Vec3 ballToKart = kartPos - soccer_world->getBallPosition();
            float dotProduct = ballToKart.getX() * ballVel.getX() + ballToKart.getZ() * ballVel.getZ();
            bool ballVelocityTowardsKartPosition = (dotProduct > 0); //true if dotProduct > 0
            float kartBallDistance = sqrt(pow(ballPos.getX() - kartPos.getX(), 2) + pow(ballPos.getY() - kartPos.getY(), 2)+  pow(ballPos.getZ() - kartPos.getZ(), 2));
            float t;
            float yoffset;
            if ((ballVelocityTowardsKartPosition) && (ballVel.length() > jump_ball_vel_min))
            {
                t = jump_value4_sta;
                yoffset = jump_value3_sta;
            }
            else if (kartBallDistance < jump_distance_max_fastjumping)
            {
            t = 0.7; //time for kart to intercept the ball
            yoffset = -4;
            }
            else
            {
            t = jump_value4_mov; //time for kart to intercept the ball
            yoffset = jump_value3_mov;
            }
            //calculate the x, y and z velocities required for the kart to intercept the ball
            const float vx = (ballVel.getX()*t + ballPos.getX() - kartPos.getX()) / t;
            const float vz = (ballVel.getZ()*t + ballPos.getZ() - kartPos.getZ()) / t;
            const float vy = (ballVel.getY()*t + ballPos.getY() - kartPos.getY()) / t - g*t + yoffset;

            //set the kart's linear velocity to intercept the ball
            m_kart->getBody()->setLinearVelocity(Vec3(vx, vy, vz));



        }
        }

        break;
    case PA_RESCUE:
        SET_OR_TEST_GETTER(Rescue, value!=0);
        break;
    case PA_FIRE:
        SET_OR_TEST_GETTER(Fire, value!=0);
        break;
    case PA_LOOK_BACK:
        SET_OR_TEST_GETTER(LookBack, value!=0);
        break;
    case PA_DRIFT:
        if (value == 0)
        {
            SET_OR_TEST_GETTER(SkidControl, KartControl::SC_NONE);
        }
        else if (m_controls->getSkidControl() == KartControl::SC_NONE)
        {
            if (m_steer_val == 0)
            {
                SET_OR_TEST_GETTER(SkidControl, KartControl::SC_NO_DIRECTION);
            }
            else
            {
                SET_OR_TEST_GETTER(SkidControl, m_steer_val<0
                                                ? KartControl::SC_RIGHT
                                                : KartControl::SC_LEFT  );
            }
        }
        break;
    case PA_PAUSE_RACE:
        if (value != 0) StateManager::get()->escapePressed();
        break;
    default:
       break;
    }
    if (dry_run) return false;
    return true;
#undef SET_OR_TEST
#undef SET_OR_TEST_GETTER
}   // action

//-----------------------------------------------------------------------------
void PlayerController::actionFromNetwork(PlayerAction p_action, int value,
                                         int value_l, int value_r)
{
    m_steer_val_l = value_l;
    m_steer_val_r = value_r;
    PlayerController::action(p_action, value, /*dry_run*/false);
}   // actionFromNetwork

//-----------------------------------------------------------------------------
/** Handles steering for a player kart.
 */
void PlayerController::steer(int ticks, int steer_val)
{
    // Get the old value, compute the new steering value,
    // and set it at the end of this function
    float steer = m_controls->getSteer();
    if(stk_config->m_disable_steer_while_unskid &&
        m_controls->getSkidControl()==KartControl::SC_NONE &&
       m_kart->getSkidding()->getVisualSkidRotation()!=0)
    {
        steer = 0;
    }

    // Amount the steering is changed for digital devices.
    // If the steering is 'back to straight', a different steering
    // change speed is used.
    float dt = stk_config->ticks2Time(ticks);
    const float STEER_CHANGE = ( (steer_val<=0 && steer<0) ||
                                 (steer_val>=0 && steer>0)   )
                     ? dt/m_kart->getKartProperties()->getTurnTimeResetSteer()
                     : dt/m_kart->getTimeFullSteer(fabsf(steer));
    if (steer_val < 0)
    {
        steer += STEER_CHANGE;
        steer = std::min(steer, -steer_val/32767.0f);
    }
    else if(steer_val > 0)
    {
        steer -= STEER_CHANGE;
        steer = std::max(steer, -steer_val/32767.0f);
    }
    else
    {   // no key is pressed
        if(steer>0.0f)
        {
            steer -= STEER_CHANGE;
            if(steer<0.0f) steer=0.0f;
        }
        else
        {   // steer<=0.0f;
            steer += STEER_CHANGE;
            if(steer>0.0f) steer=0.0f;
        }   // if steer<=0.0f
    }   // no key is pressed
    m_controls->setSteer(std::min(1.0f, std::max(-1.0f, steer)) );

}   // steer

//-----------------------------------------------------------------------------
/** Callback when the skidding bonus is triggered. The player controller
 *  resets the current steering to 0, which makes the kart easier to control.
 */
void PlayerController::skidBonusTriggered()
{
    m_controls->setSteer(0);
}   // skidBonusTriggered

//-----------------------------------------------------------------------------
/** Updates the player kart, called once each timestep.
 */
void PlayerController::update(int ticks)
{
    steer(ticks, m_steer_val);

    if (World::getWorld()->isStartPhase())
    {
        if ((m_controls->getAccel() || m_controls->getBrake()||
            m_controls->getNitro()) && !NetworkConfig::get()->isNetworking())
        {
            // Only give penalty time in READY_PHASE.
            // Penalty time check makes sure it doesn't get rendered on every
            // update.
            if (m_penalty_ticks == 0 &&
                World::getWorld()->getPhase() == WorldStatus::READY_PHASE)
            {
                displayPenaltyWarning();
            }   // if penalty_time = 0
            m_controls->setBrake(false);
        }   // if key pressed

        return;
    }   // if isStartPhase

    if (m_penalty_ticks != 0 &&
        World::getWorld()->getTicksSinceStart() < m_penalty_ticks)
    {
        m_controls->setBrake(false);
        m_controls->setAccel(0.0f);
        return;
    }

    // Only accept rescue if there is no kart animation is already playing
    // (e.g. if an explosion happens, wait till the explosion is over before
    // starting any other animation).
    if ( m_controls->getRescue() && !m_kart->getKartAnimation() )
    {
        RescueAnimation::create(m_kart);
        m_controls->setRescue(false);
    }
}   // update

//-----------------------------------------------------------------------------
/** Called when a kart hits or uses a zipper.
 */
void PlayerController::handleZipper(bool play_sound)
{
    m_kart->showZipperFire();
}   // handleZipper

//-----------------------------------------------------------------------------
bool PlayerController::saveState(BareNetworkString *buffer) const
{
    // NOTE: when the size changes, the AIBaseController::saveState and
    // restore state MUST be adjusted!!
    int steer_abs = std::abs(m_steer_val);
    buffer->addUInt16((uint16_t)steer_abs).addUInt16(m_prev_accel)
        .addUInt8((m_prev_brake ? 1 : 0) | (m_prev_nitro ? 2 : 0));
    return m_steer_val < 0;
}   // copyToBuffer

//-----------------------------------------------------------------------------
void PlayerController::rewindTo(BareNetworkString *buffer)
{
    // NOTE: when the size changes, the AIBaseController::saveState and
    // restore state MUST be adjusted!!
    m_steer_val  = buffer->getUInt16();
    m_prev_accel = buffer->getUInt16();
    uint8_t c = buffer->getUInt8();
    m_prev_brake = (c & 1) != 0;
    m_prev_nitro = (c & 2) != 0;
}   // rewindTo

// ----------------------------------------------------------------------------
core::stringw PlayerController::getName(bool include_handicap_string) const
{
    core::stringw name = m_kart->getName();
    if (NetworkConfig::get()->isNetworking())
    {
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(
            m_kart->getWorldKartId());
        name = rki.getPlayerName();
        if (include_handicap_string && rki.getHandicap() == HANDICAP_MEDIUM)
        {
#ifdef SERVER_ONLY
            name += L" (handicapped)";
#else
            name = _("%s (handicapped)", name);
#endif
        }
    }
    return name;
}   // getName

// ----------------------------------------------------------------------------
void PlayerController::displayPenaltyWarning()
{
    m_penalty_ticks = stk_config->m_penalty_ticks;
}   // displayPenaltyWarning


