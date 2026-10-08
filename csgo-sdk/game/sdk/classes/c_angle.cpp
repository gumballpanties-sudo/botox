#include "c_angle.h"
#include "../../../globals/math/math.h"
#include "c_vector.h"

c_vector c_angle::to_vector( )
{
	return c_vector( m_x, m_y, m_z );
}

c_angle c_angle::forward( )
{
	c_angle return_forward{ };
	float sp{ }, sy{ }, cp{ }, cy{ };

	g_math.sin_cos( deg2rad( m_x ), &sp, &cp );
	g_math.sin_cos( deg2rad( m_y ), &sy, &cy );

	return_forward.m_x = cp * cy;
	return_forward.m_y = cp * sy;
	return_forward.m_z = -sp;

	return return_forward;
}

c_angle c_angle::right( )
{
	c_angle return_right{ };
	float sp{ }, sy{ }, sr{ }, cp{ }, cy{ }, cr{ };

	g_math.sin_cos( deg2rad( m_x ), &sp, &cp );
	g_math.sin_cos( deg2rad( m_y ), &sy, &cy );
	g_math.sin_cos( deg2rad( m_z ), &sr, &cr );

	return_right.m_x = -1 * sr * sp * cy + -1 * cr * -sy;
	return_right.m_y = -1 * sr * sp * sy + -1 * cr * cy;
	return_right.m_z = -1 * sr * cp;

	return return_right;
}

c_angle c_angle::up( )
{
	c_angle return_up{ };
	float sp{ }, sy{ }, sr{ }, cp{ }, cy{ }, cr{ };

	g_math.sin_cos( deg2rad( m_x ), &sp, &cp );
	g_math.sin_cos( deg2rad( m_y ), &sy, &cy );
	g_math.sin_cos( deg2rad( m_z ), &sr, &cr );

	return_up.m_x = cr * sp * cy + -sr * -sy;
	return_up.m_y = cr * sp * sy + -sr * cy;
	return_up.m_z = cr * cp;

	return return_up;
}
