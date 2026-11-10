#pragma once
#include <algorithm>
#include <cmath>
// Lumia aim math; Botox's existing settings are translated at this boundary.
namespace n_lumia_aim {
inline float calculate_fov(const c_angle& view_point,const c_angle& aim_angle) {
    c_vector forward_angles{},forward_aim_angles{};
    g_math.angle_vectors(view_point,&forward_aim_angles);
    g_math.angle_vectors(aim_angle,&forward_angles);
    const float length=forward_aim_angles.length()*forward_angles.length();
    if(!std::isfinite(length)||length<=0.f)return 180.f;
    const float dot=forward_aim_angles.dot_product(forward_angles)/length;
    if(!std::isfinite(dot))return 180.f;
    return rad2deg(acos(std::clamp(dot,-1.f,1.f)));
}
inline int smoothing_percent(float botox_value) {
    if(!std::isfinite(botox_value))return 0;
    return int(std::lround((std::clamp(botox_value,1.f,30.f)-1.f)*(100.f/29.f)));
}
inline c_angle smooth_angle( c_angle from, c_angle target, int percent, float interval )
{
	percent = std::clamp( percent, 0, 100 );
	if ( percent == 0 ) return target;
	if ( !std::isfinite( interval ) || interval <= 0.f ) interval = 1.f / 64.f;
	interval = std::min( interval, 0.1f );
	const float amount = percent / 100.f;
	const float tau = 0.8f * amount * amount;
	const float alpha = -std::expm1( -interval / tau );
	c_angle delta = target - from;
	delta.normalize(); // Shortest path across the -180/180 yaw boundary.
	c_angle result = from + delta * alpha;
	result.normalize();
	result.clamp();
	return result;
}
inline c_angle apply_rcs( c_angle angle, float rcs_x_pct, float rcs_y_pct )
{
	const auto scale = g_convars[ HASH_BT( "weapon_recoil_scale" ) ];
    const float recoil_scale = scale ? scale->get_float() : 2.f;
	const c_angle punch_component = g_ctx.m_local->get_punch( ) * recoil_scale;

	angle.m_x -= punch_component.m_x * ( rcs_x_pct / 100.f );
	angle.m_y -= punch_component.m_y * ( rcs_y_pct / 100.f );
	return angle;
}
}
