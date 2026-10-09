#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace n_tb::wallstrafe
{
	constexpr float k_speed_tolerance = 0.05f;
	inline bool retains_speed( float before, float after )
	{
		return std::isfinite( before ) && std::isfinite( after ) && after + k_speed_tolerance >= before;
	}

	struct wish_t {
		float into = 0.f, along = 0.f, gain = 0.f;
	};

	// Rank acceleration retained along a flat wall. Engine prediction validates
	// the shortlisted inputs against actual collisions, stance and server rules.
	inline std::array< wish_t, 3 > candidates( float into_velocity, float along_velocity, float wishspeed, float acceleration )
	{
		std::array< wish_t, 3 > result{ };
		if ( !std::isfinite( into_velocity ) || !std::isfinite( along_velocity ) ||
		     !std::isfinite( wishspeed ) || !std::isfinite( acceleration ) || wishspeed <= 0.f || acceleration <= 0.f )
			return result;
		constexpr float radians = 0.017453292519943295f;
		const auto at = [ & ]( float degrees ) {
			wish_t w{ std::cos( degrees * radians ), std::sin( degrees * radians ), 0.f };
			const float add = ( std::min )( wishspeed, 30.f ) - into_velocity * w.into - along_velocity * w.along;
			w.gain = ( std::min )( acceleration * wishspeed, ( std::max )( 0.f, add ) ) * w.along;
			return w;
		};
		float angle = 0.f;
		for ( int i = 0; i <= 360; ++i ) {
			const float a = static_cast< float >( i ) * 0.25f;
			const wish_t w = at( a );
			if ( w.gain > result[ 0 ].gain ) {
				angle = a;
				result[ 0 ] = w;
			}
		}
		if ( result[ 0 ].gain <= 0.f )
			return result;
		result[ 1 ] = at( ( std::max )( 0.f, angle - 0.5f ) );
		result[ 2 ] = at( ( std::min )( 90.f, angle + 0.5f ) );
		return result;
	}

	// Add along-wall acceleration while keeping the precision correction's
	// normal acceleration. The caller still validates the resulting collision.
	inline wish_t aligned_wish( float into_velocity, float along_velocity, float normal_delta, float wishspeed, float acceleration )
	{
		wish_t best{ };
		if ( !std::isfinite( into_velocity ) || !std::isfinite( along_velocity ) || !std::isfinite( normal_delta ) ||
		     !std::isfinite( wishspeed ) || !std::isfinite( acceleration ) || wishspeed <= 0.f || acceleration <= 0.f ||
		     std::fabs( normal_delta ) < 1e-5f )
			return best;
		const double sign = normal_delta < 0.f ? -1.0 : 1.0;
		constexpr double radians = 0.017453292519943295;
		for ( int i = 1; i < 360; ++i ) {
			const double angle = i * 0.25 * radians;
			const double normal = sign * std::cos( angle ), along = std::sin( angle );
			const double add = normal_delta / normal;
			const double dot = into_velocity * normal + along_velocity * along;
			if ( add + dot > ( std::min )( static_cast< double >( wishspeed ), 30.0 ) )
				continue;
			const double speed = ( std::max )( add / acceleration, add + dot );
			const double gain = add * along;
			if ( speed > wishspeed || gain <= best.gain )
				continue;
			best = { static_cast< float >( normal * speed ), static_cast< float >( along * speed ), static_cast< float >( gain ) };
		}
		return best;
	}

	inline float direction( float input_along, float velocity_along )
	{
		const float desired = std::fabs( input_along ) > 1.f ? input_along : velocity_along;
		return desired < 0.f ? -1.f : 1.f;
	}

	// Remember only movement, so preserving a predicted command never restores
	// stale shooting buttons or changes the player's camera/aim angles.
	struct owned_move_t {
		int command_number = -1, buttons = 0;
		float yaw = 0.f, forward = 0.f, side = 0.f, up = 0.f;

		void clear( ) { command_number = -1; }

		template< typename Cmd > void capture( const Cmd& cmd )
		{
			command_number = cmd.m_command_number;
			buttons = cmd.m_buttons;
			yaw = cmd.m_view_point.m_y;
			forward = cmd.m_forward_move;
			side = cmd.m_side_move;
			up = cmd.m_up_move;
		}

		template< typename Cmd > bool apply( Cmd& cmd, int movement_mask ) const
		{
			if ( command_number < 0 || cmd.m_command_number != command_number || !std::isfinite( cmd.m_view_point.m_y ) )
				return false;
			const float delta = ( cmd.m_view_point.m_y - yaw ) * 0.017453292519943295f;
			const float c = std::cos( delta ), s = std::sin( delta );
			cmd.m_forward_move = c * forward - s * side;
			cmd.m_side_move = s * forward + c * side;
			cmd.m_up_move = up;
			cmd.m_buttons = ( cmd.m_buttons & ~movement_mask ) | ( buttons & movement_mask );
			return true;
		}
	};
}
