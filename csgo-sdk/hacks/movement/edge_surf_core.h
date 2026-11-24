#pragma once
/* edge surf core: one text for the dll ( edge_surf.cpp ) and the lab ( tools/wc_engine/es_lab.cpp ). W = the adapter: engine sims + traces.
   a pin = a tick END airborne with vz exactly -g dt / 2 at a ledge top. every pin is found by flying candidate moves in the engine */
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace n_es
{
	constexpr float k_eps       = 0.03125f; /* DIST_EPSILON */
	constexpr float k_min_frac  = 1.05e-4f; /* gamemovement MINIMUM_MOVE_FRACTION 1e-4: a smaller top hit is zeroed */
	constexpr float k_duck_crop = 0.34f;
	constexpr float k_wish_max  = 450.f;
	constexpr float k_ztol      = 0.06f; /* pin END over top + eps: vphysics tops read up to 1/32 out */
	constexpr float k_rad       = 57.2957795f;
	constexpr int k_cand        = 10;

	struct v3 {
		float x = 0.f, y = 0.f, z = 0.f;
	};
	inline v3 mk( const float x, const float y, const float z )
	{
		v3 r;
		r.x = x;
		r.y = y;
		r.z = z;
		return r;
	}

	struct st_t {
		v3 o{ }, v{ };
		bool ground = false, duck = false, walk = false;
	};
	/* one tick of input: wish toward world yaw ( forwardmove units, duck crop already undone ) + the duck bit */
	struct mv_t {
		float yaw = 0.f, wish = 0.f;
		bool duck = false;
	};
	struct hull_t {
		v3 mins{ }, maxs{ };
	};
	struct hit_t {
		float frac = 1.f, d = 0.f; /* d = hull_plane_dist: the face plane in world space */
		v3 n{ }, end{ };
		/* world = box rules ( brush, brush entity ), vp = vphysics only ( static prop, SOLID_VPHYSICS ), neither = an entity box ( bbox / obb ): unknown */
		bool start_solid = false, moves = true, ladder = false, disp = false, world = true, vp = false;
	};
	/* ledge: vertical face n . xy = d ( n out toward the hull, z 0 ), its top, along = xy . ( -n.y, n.x ). prop = a vphysics / entity hit read it,
	   brush = a world brush hit read it. both = a prop flush with a brush ( nuke trim in clip ): which one a sweep meets first is a float tie */
	struct edge_t {
		v3 n{ };
		float d = 0.f, top = 0.f;
		bool prop = false, brush = false;
	};
	/* only vphysics: over the top pins only ( a beside hull whose move ends under the top misses it ), no lift, no stand up, no hold beside */
	inline bool vphys( const edge_t& e ) { return e.prop && !e.brush; }

	inline float ulp( const float a )
	{
		int ex = 0;
		std::frexp( a, &ex );
		return a != 0.f ? std::ldexp( 1.f, ex - 24 ) : 1e-7f;
	}
	/* plane first, extent after: centre - plane is exact near the face, adding the 16u extent first rounds at the face coordinate's ulp ( 2x where
	   the hull centre and the face sit on two sides of a power of two ) */
	inline float gap( const edge_t& e, const v3& o, const hull_t& h )
	{
		return ( o.x * e.n.x + o.y * e.n.y - e.d ) + ( std::min( e.n.x * h.mins.x, e.n.x * h.maxs.x ) + std::min( e.n.y * h.mins.y, e.n.y * h.maxs.y ) );
	}
	inline float closing( const edge_t& e, const v3& v ) { return -( v.x * e.n.x + v.y * e.n.y ); }
	inline float along( const edge_t& e, const v3& p ) { return p.y * e.n.x - p.x * e.n.y; }
	/* a vphysics read stops 0..1/32 short of its plane ( hull_plane_dist assumes 1/32: nuke curb d 1080 / 1079.9689 ): one ledge */
	inline bool same( const edge_t& a, const edge_t& b )
	{
		return a.n.x * b.n.x + a.n.y * b.n.y > 0.99999f && fabsf( a.d - b.d ) < 0.04f && fabsf( a.top - b.top ) < 0.04f;
	}
	/* one ledge read twice: a brush read is exact, else the outer vphysics read ( every read sits at or under the real plane ) */
	inline void absorb( edge_t& o, const edge_t& e )
	{
		if ( e.brush && !o.brush ) {
			o.d   = e.d;
			o.top = e.top;
		} else if ( !o.brush && !e.brush ) {
			o.d   = std::max( o.d, e.d );
			o.top = std::max( o.top, e.top );
		}
		o.prop  = o.prop || e.prop;
		o.brush = o.brush || e.brush;
	}
	/* the gap's float grid: the coordinates the face normal reads, never under the hull half width's */
	inline float face_ulp( const edge_t& e, const v3& o )
	{
		return std::max( std::max( fabsf( e.n.x ) > 1e-3f ? ulp( o.x ) : 0.f, fabsf( e.n.y ) > 1e-3f ? ulp( o.y ) : 0.f ), 1.9073486e-6f );
	}
	/* the guide parks here until the tick before the crossing, which then picks the arrival gap by flying it */
	inline float standoff( const float u ) { return std::max( 32.f * u, 5e-4f ); }

	/* ticks until the feet cross top + eps ( 0 = this tick ), the box sweep's float steps ( cmodel.cpp IntersectRayWithBoxBrush on the hull
	   centre ). f0 = the top plane's enter fraction that tick, zc / vzc = that tick's START */
	inline bool crossing( const float top, float z, float vz, const float off, const float pin, const float dt, int& k, float& f0, float* zc = nullptr,
	                      float* vzc = nullptr )
	{
		for ( k = 0; k < 256; ++k ) {
			volatile float v1 = vz + pin;
			volatile float dz = dt * v1;
			volatile float z1 = z + dz;
			volatile float c  = z + off;
			volatile float a  = top - c;
			a                 = a + off;
			if ( a >= 0.f )
				return false;
			volatile float dlt = z1 - z;
			if ( !( dlt > a ) ) {
				volatile float ae  = a + k_eps;
				volatile float inv = 1.f / dlt;
				volatile float t   = ae * inv;
				f0                 = t;
				if ( zc )
					*zc = z;
				if ( vzc )
					*vzc = vz;
				return true;
			}
			z  = z1;
			vz = v1 + pin;
		}
		return false;
	}

	/* sv_air_max_wishspeed 30: a straight press never raises its own wish speed past it */
	static float es_stop_speed( const float r, const float a, const float dt )
	{
		const float ar = std::fabs( r ), k = std::floor( ( std::sqrt( 1.f + 8.f * ar / ( a * dt ) ) - 1.f ) * 0.5f );
		return ( r >= 0.f ? 1.f : -1.f ) * ( ar / ( ( k + 1.f ) * dt ) + k * a * 0.5f );
	}
	static float es_profile_step( const float r, const float w, const float ap, const float a, const float dt )
	{
		const float up = std::min( w + a, std::max( w, 30.f ) ), dn = std::max( w - a, std::min( w, -30.f ) );
		return std::clamp( es_stop_speed( r, ap, dt ), dn, up );
	}
	static bool es_in_window( const float r, const float w, const float park, const float a, const float dt )
	{
		return std::fabs( r ) / dt <= park && std::fabs( std::clamp( r / dt, -park, park ) - w ) <= a;
	}
	/* r_min = face plane: a plan that touches it puts the hull over the top = standing on the ledge */
	static int es_profile_ticks( float r, float w, const float ap, const float a, const float park, const float dt, const int n, const float r_min )
	{
		for ( int t = 0; t <= n; ++t ) {
			if ( es_in_window( r, w, park, a, dt ) )
				return t;
			w = es_profile_step( r, w, ap, a, dt );
			r -= w * dt;
			if ( r <= r_min )
				return -1;
		}
		return -1;
	}

	/* above the top: err = distance left to the aim ( + = toward the face ), r_min = the face plane on that scale. K = one press lands K's window,
	   D = delta strafe turn ( v = closing change ), S = straight stop-in-time profile, F = nothing fits. pure: air align flies it too */
	struct es_guide_t {
		char act = 'F';
		float v  = 0.f;
	};
	/* pull = undershoot: a press profile that gets there in time fires at any gap ( else only inside reach / owned / your speed reaches the face ) */
	static es_guide_t es_guide( const float err, const float gap, const float r_min, const float v_old, const float vxy, const int kc, const float authority,
	                            const float turn, const float reach, const bool owned, const bool staged, const float ipt, const bool pull = false )
	{
		constexpr float k_park_speed = 25.f;
		const float park    = std::min( k_park_speed, authority );
		const float v_park  = std::clamp( err / ipt, -park, park );
		const bool settles  = std::fabs( v_park - v_old ) <= authority;
		if ( std::fabs( err ) / ipt <= park && settles )
			return { 'K', v_park };
		if ( turn > 0.f && vxy > 30.f ) {
			const auto d_at = [ & ]( const float ww ) {
				return std::min( std::min( vxy * std::tan( turn / k_rad ), 30.f ), authority ) * std::sqrt( std::max( vxy * vxy - ww * ww, 0.f ) ) / vxy;
			};
			const float d = d_at( v_old );
			if ( d >= 1.f ) {
				const float ae = std::fabs( err );
				const int k    = static_cast< int >( std::floor( ( std::sqrt( 1.f + 8.f * ae / ( d * ipt ) ) - 1.f ) * 0.5f ) );
				const float vs = ( err >= 0.f ? 1.f : -1.f ) * ( ae / ( static_cast< float >( k + 1 ) * ipt ) + static_cast< float >( k ) * d * 0.5f );
				const auto d_reaches = [ & ] {
					float rr = err, ww = v_old;
					for ( int t = 0; t < kc; ++t ) {
						if ( es_in_window( rr, ww, park, authority, ipt ) )
							return true;
						ww = std::clamp( es_stop_speed( rr, d, ipt ), ww - d, ww + d );
						rr -= ww * ipt;
						if ( rr <= r_min )
							return false;
					}
					return false;
				};
				constexpr float k_d2 = 60.f;
				const float d2_cap   = std::min( k_d2, 0.24f * vxy );
				const auto d2_target = [ & ]( const float rr, const float ww ) { return std::min( es_stop_speed( rr, 0.75f * authority, ipt ), std::max( ww, d2_cap ) ); };
				const auto d2_reaches = [ & ] {
					float rr = err, ww = v_old;
					for ( int t = 0; t <= kc - 3 && rr > r_min; ++t ) {
						if ( es_in_window( rr, ww, park, authority, ipt ) )
							return true;
						const float g = d2_target( rr, ww ), dd = d_at( ww );
						ww            = g > ww + 0.5f * dd ? std::min( ww + dd, g ) : es_profile_step( rr, ww, 0.75f * authority, authority, ipt );
						rr -= ww * ipt;
					}
					return false;
				};
				const bool d1 = static_cast< float >( k + 1 ) + std::fabs( vs - v_old ) / d <= static_cast< float >( kc - 1 ) && d_reaches( );
				const bool d2 = !d1 && err > 0.f && kc >= 3 && d2_target( err, v_old ) > v_old + 0.5f * d && d2_reaches( );
				if ( d1 || d2 )
					return { 'D', d1 ? std::clamp( vs, v_old - d, v_old + d ) - v_old : std::min( d2_target( err, v_old ) - v_old, d ) };
			}
		}
		/* reach drift: staged = the slowest press that reaches the standoff 4 ticks before the crossing ( brake, creep, brake, correct ), else park speed */
		const float k_drift = static_cast< float >( staged ? std::max( kc - 4, 1 ) : kc );
		const float v_drift = staged ? std::clamp( err / ( k_drift * ipt ), -park, park ) : v_park;
		if ( gap <= reach && std::fabs( err ) / ( k_drift * ipt ) <= park && std::fabs( v_drift - v_old ) <= authority )
			return { 'K', v_drift };
		if ( ( pull || gap <= reach || owned || ( v_old > 0.f && v_old * ipt * static_cast< float >( kc ) >= err ) ) && kc >= 3 &&
		     es_profile_ticks( err, v_old, 0.75f * authority, authority, park, ipt, kc - 3, r_min ) >= 0 )
			return { 'S', es_profile_step( err, v_old, 0.75f * authority, authority, ipt ) };
		return { };
	}

	/* AirAccelerate inverse: the wish along the face normal that ends this tick closing at c ( c0 = closing now ). VectorNormalize divides by len + FLT_EPSILON */
	template < class W >
	mv_t press( const W& w, const edge_t& e, const float c0, const float c, const bool duck )
	{
		float wv = c >= c0 ? std::max( ( c - c0 ) / w.k_acc, c ) : -std::max( ( c0 - c ) / w.k_acc, -c );
		wv += wv >= 0.f ? FLT_EPSILON : -FLT_EPSILON;
		mv_t m;
		m.duck = duck;
		m.wish = std::min( fabsf( wv ) / ( duck ? k_duck_crop : 1.f ), k_wish_max );
		m.yaw  = atan2f( -e.n.y, -e.n.x ) * k_rad + ( wv >= 0.f ? 0.f : 180.f );
		return m;
	}
	/* closing c plus an along change a ( diagonal: the along speed's low bits pick its rounding ). one AirAccelerate onto that velocity: wish dir = the
	   change, wish speed = max( change / accel, speed along it after ). a against your travel only: adding speed past the 30 wish cap is not one press */
	template < class W >
	mv_t press_along( const W& w, const edge_t& e, const v3& v0, const float c, const float a, const bool duck )
	{
		const float c0 = closing( e, v0 ), tx = -e.n.y, ty = e.n.x;
		const float dx = -e.n.x * ( c - c0 ) + tx * a, dy = -e.n.y * ( c - c0 ) + ty * a, dl = sqrtf( dx * dx + dy * dy );
		if ( !( dl > 0.f ) || a == 0.f )
			return press( w, e, c0, c, duck );
		const float ux = dx / dl, uy = dy / dl;
		mv_t m;
		m.duck = duck;
		m.wish = std::min( ( std::max( dl / w.k_acc, v0.x * ux + v0.y * uy + dl ) + FLT_EPSILON ) / ( duck ? k_duck_crop : 1.f ), k_wish_max );
		m.yaw  = atan2f( uy, ux ) * k_rad;
		return m;
	}
	/* delta strafe: wish square to the velocity moves the closing speed by dv, speed kept */
	template < class W >
	mv_t turn( const W& w, const edge_t& e, const v3& v, const float dv, const bool duck )
	{
		const float vxy = std::max( sqrtf( v.x * v.x + v.y * v.y ), 1e-3f );
		float px = -v.y / vxy, py = v.x / vxy, into = -( px * e.n.x + py * e.n.y );
		if ( ( into < 0.f ) != ( dv < 0.f ) ) {
			px   = -px;
			py   = -py;
			into = -into;
		}
		const float add = std::min( fabsf( dv ) / std::max( fabsf( into ), 1e-3f ), 30.f );
		mv_t m;
		m.duck = duck;
		m.wish = std::min( std::max( add / w.k_acc, add ) / ( duck ? k_duck_crop : 1.f ), k_wish_max );
		m.yaw  = atan2f( py, px ) * k_rad;
		return m;
	}

	enum { k_miss = 0, k_pin = 1, k_over = 2 };
	/* sim END vs this ledge: pin, over ( grounded / hull over the top = lands ), miss ( beside it, no pin ). TryPlayerMove's stuck test zeroes the
	   whole velocity: that also ends on vz = -g dt / 2 with the hull frozen = no pin ( xy exactly 0 ) */
	template < class W >
	int verdict( const W& w, const edge_t& e, const hull_t& h, const st_t& r, const float vxy0 )
	{
		if ( !r.walk )
			return k_over;
		/* grounded under the top = fell past it onto a floor beside the ledge ( a low floor inside one fall step ): a miss, not a landing on it */
		if ( r.ground )
			return r.o.z < e.top - 0.5f ? k_miss : k_over;
		if ( fabsf( r.v.z - w.pin ) < 1e-3f && fabsf( r.o.z - ( e.top + k_eps ) ) < k_ztol && ( r.v.x != 0.f || r.v.y != 0.f || vxy0 == 0.f ) )
			return k_pin;
		return gap( e, r.o, h ) < 0.f ? k_over : k_miss;
	}
	inline float speed_xy( const v3& v ) { return sqrtf( v.x * v.x + v.y * v.y ); }

	/* the crossing tick from state s ( from( ) puts the engine there ): coast first ( a hold off the float grid needs no press ), then the closing speed
	   between a miss ( low ) and an over ( high ) is bisected. the pin sits on that boundary: beside the face it is a sub ulp window ( the full move
	   crosses the plane, bump 1 + bump 2 round back off it ), over the top it is the out speed that leaves after the top hit. tweaks = along speed
	   draws on a diagonal ( j * 3e-3 u/s slower each ) when the plain search finds none */
	template < class W, class F >
	bool find_pin( W& w, const edge_t& e, const hull_t& h, const st_t& s, const F& from, const float u, mv_t& out, const int tweaks = 0 )
	{
		const float c0     = closing( e, s.v );
		const float a      = w.k_acc * w.max_speed * ( s.duck ? k_duck_crop : 1.f );
		const float lim_lo = c0 - a, lim_hi = std::max( c0, std::min( c0 + a, 30.f ) );
		const float vxy0   = speed_xy( s.v );
		if ( w.sims_left( ) <= 0 )
			return false;
		mv_t coast;
		coast.duck = s.duck;
		from( );
		if ( verdict( w, e, h, w.sim( coast ), vxy0 ) == k_pin ) {
			out = coast;
			return true;
		}
		const float cs  = std::clamp( gap( e, s.o, h ) / w.dt, lim_lo, lim_hi );
		const float dir = along( e, s.v ) >= 0.f ? 1.f : -1.f;
		for ( int j = 0; j <= tweaks; ++j ) {
			const float al = -3e-3f * static_cast< float >( j ) * dir;
			mv_t mv;
			const auto run = [ & ]( const float c ) {
				mv = press_along( w, e, s.v, c, al, s.duck );
				from( );
				return verdict( w, e, h, w.sim( mv ), vxy0 );
			};
			if ( w.sims_left( ) <= 0 )
				return false;
			const int k0 = run( cs );
			if ( k0 == k_pin ) {
				out = mv;
				return true;
			}
			float lo = cs, hi = cs, st = u / w.dt;
			bool bracket = false, dead = false;
			for ( int i = 0; i < 18 && !bracket && !dead; ++i, st *= 2.f ) {
				if ( w.sims_left( ) <= 0 )
					return false;
				const float c = k0 == k_miss ? std::min( lo + st, lim_hi ) : std::max( hi - st, lim_lo );
				const int r   = run( c );
				if ( r == k_pin ) {
					out = mv;
					return true;
				}
				if ( k0 == k_miss ) {
					if ( r == k_over )
						hi = c, bracket = true;
					else if ( c >= lim_hi )
						dead = true;
					else
						lo = c;
				} else {
					if ( r == k_miss )
						lo = c, bracket = true;
					else if ( c <= lim_lo )
						dead = true;
					else
						hi = c;
				}
			}
			for ( int i = 0; bracket && i < 28 && ( hi - lo ) * w.dt > u * 4e-3f; ++i ) {
				if ( w.sims_left( ) <= 0 )
					return false;
				const float mid = 0.5f * ( lo + hi );
				if ( !( mid > lo && mid < hi ) )
					break;
				const int r = run( mid );
				if ( r == k_pin ) {
					out = mv;
					return true;
				}
				( r == k_miss ? lo : hi ) = mid;
			}
		}
		return false;
	}

	/* arrival gaps worth flying first, beside the face. a box face pins when the crossing's full step rounds onto the plane ( m ulps ) while bump 1
	   ( f0 of it ) + bump 2 round back off it: round( m f0 ) + round( ( 1 - f0 ) ( m - 1/2 ) ) <= m - 1, i.e. frac( m f0 ) in ( f0 / 2, 1 / 2 ).
	   f0 near 1 needs m ~ 1 / ( 2 ( 1 - f0 ) ). only the ORDER: every gap is flown, non box faces ( diagonal, props ) take the generic tail */
	inline int arrival_gaps( const float f0, const float u, const bool axial, float* out )
	{
		int n = 0;
		/* a diagonal plane rounds both coordinates: no ulp grid on it, a spread of gaps under the eps band = a spread of rounding draws */
		if ( !axial ) {
			for ( const float g : { 4e-3f, 2e-3f, 6e-3f, 1e-3f, 3e-3f, 5e-3f, 8e-3f, 2.f * u } )
				out[ n++ ] = g;
			return n;
		}
		const float mg = 0.15f * 0.5f * ( 1.f - f0 );
		for ( int m = 1; m <= 2048 && n < 4 && static_cast< float >( m ) * u < 0.45f * k_eps; ++m ) {
			const float x = static_cast< float >( m ) * f0, fr = x - floorf( x );
			if ( fr > 0.5f * f0 + mg && fr < 0.5f - mg )
				out[ n++ ] = static_cast< float >( m ) * u;
		}
		for ( const float g : { u, 2.f * u, 3.f * u, 4e-3f } )
			if ( n < 8 && std::none_of( out, out + n, [ & ]( const float o ) { return fabsf( o - g ) < 0.5f * u; } ) )
				out[ n++ ] = g;
		return n;
	}

	/* tick before the crossing: fly each arrival ( beside at a few ulps, or over the top, then out ), the crossing searched from its END.
	   first one that pins wins: p1 now, p2 next tick. f0 = the crossing's top fraction ( vertical only, no press moves it ) */
	template < class W >
	bool arrive( W& w, const edge_t& e, const hull_t& h, const st_t& s, const float off, const float u, const float f0x, mv_t& p1, mv_t& p2 )
	{
		const float g = gap( e, s.o, h ), c0 = closing( e, s.v );
		float gts[ 16 ];
		const bool axial = fabsf( e.n.x ) > 0.9999f || fabsf( e.n.y ) > 0.9999f;
		int ng           = 0;
		/* a prop pins over the top then out: those first. the box beside gaps where a brush read it, else two beside draws */
		if ( e.prop )
			for ( const float o : { -0.12f, -0.05f, -0.25f, -0.4f } )
				gts[ ng++ ] = o;
		if ( e.brush )
			ng += arrival_gaps( f0x, u, axial, gts + ng );
		else
			for ( const float o : { 2.f * u, 4e-3f } )
				gts[ ng++ ] = o;
		if ( !e.prop )
			for ( const float o : { -0.12f, -0.05f, -0.25f } )
				gts[ ng++ ] = o;
		for ( int i = 0; i < ng; ++i ) {
			const float gt = gts[ i ];
			if ( gt > 0.f && gt >= 0.9f * k_eps )
				continue;
			if ( w.sims_left( ) < 12 )
				return false;
			const mv_t a = press( w, e, c0, ( g - gt ) / w.dt, s.duck );
			w.reset( );
			const st_t e1 = w.sim( a );
			int k         = -1;
			float f0      = 0.f;
			if ( !e1.walk || e1.ground || !crossing( e.top, e1.o.z, e1.v.z, off, w.pin, w.dt, k, f0 ) || k != 0 || f0 < k_min_frac )
				continue;
			const float g1 = gap( e, e1.o, h );
			/* over the top the tick before must END 2u+ over it: CategorizePosition's probe grounds anything closer */
			if ( gt > 0.f ? !( g1 > 0.f && g1 < k_eps ) : !( g1 < 0.f && e1.o.z > e.top + 2.f ) )
				continue;
			if ( !w.save( 0 ) )
				return false;
			mv_t b;
			const bool ok = find_pin( w, e, h, e1, [ & ] { w.load( 0 ); }, face_ulp( e, e1.o ), b );
			if ( w.debug ) {
				char buf[ 160 ];
				snprintf( buf, sizeof( buf ), "ESURF: arrive gt=%.3e got=%.3e f0=%.4f close=%.4e pin=%d", gt, g1, f0, closing( e, e1.v ), ok ? 1 : 0 );
				w.note( 'a', buf );
			}
			if ( ok ) {
				p1 = a;
				p2 = b;
				return true;
			}
		}
		return false;
	}

	/* highest flat top 0.5u inside the face under the hull at p: axial = a strip over the hull's side, diagonal = the hull corner that meets it */
	template < class W >
	bool top_at( W& w, const edge_t& e, const hull_t& h, const v3& p, const float z_hi, const float z_lo, float& top )
	{
		const bool ax = fabsf( e.n.x ) > 0.9999f, ay = fabsf( e.n.y ) > 0.9999f;
		const v3 corner = mk( e.n.x > 0.f ? h.mins.x : h.maxs.x, e.n.y > 0.f ? h.mins.y : h.maxs.y, 0.f );
		const float s   = ax || ay ? along( e, p ) : along( e, mk( p.x + corner.x, p.y + corner.y, 0.f ) );
		const v3 q      = mk( e.n.x * ( e.d - 0.5f ) - e.n.y * s, e.n.y * ( e.d - 0.5f ) + e.n.x * s, z_hi );
		const float wy = ax ? h.maxs.y - 0.5f : 0.25f, wx = ay ? h.maxs.x - 0.5f : 0.25f;
		hit_t t;
		w.trace( q, mk( q.x, q.y, z_lo ), mk( -wx, -wy, 0.f ), mk( wx, wy, 0.f ), t );
		const auto flat = [ & ] { return !t.start_solid && t.frac < 1.f && t.n.z >= ( t.disp ? 0.99999f : 0.999f ); };
		if ( !flat( ) ) {
			t = hit_t{ };
			w.trace( q, mk( q.x, q.y, z_lo ), mk( 0.f, 0.f, 0.f ), mk( 0.f, 0.f, 0.f ), t );
			if ( !flat( ) )
				return false;
		}
		top = t.n.z >= 0.99999f ? t.d / t.n.z : t.end.z - k_eps;
		return true;
	}

	/* the hull beside the face at the arrival ( along s, feet top + eps ): CategorizePosition's 2u ground probe ( gamemovement.cpp:4171 ) hitting a
	   floor = a landing, never a pin ( low curbs ). a steep first hit tries the 4 quadrant boxes like the engine ( :4061 ). 0.1 out: closer reads the top */
	template < class W >
	bool floor_near( W& w, const edge_t& e, const hull_t& h, const float s )
	{
		const float r = e.d + 0.1f - ( std::min( e.n.x * h.mins.x, e.n.x * h.maxs.x ) + std::min( e.n.y * h.mins.y, e.n.y * h.maxs.y ) );
		const v3 p    = mk( e.n.x * r - e.n.y * s, e.n.y * r + e.n.x * s, e.top + k_eps );
		const v3 to   = mk( p.x, p.y, p.z - 2.f );
		const auto ground = [ & ]( const v3& lo, const v3& hi, bool& steep ) {
			hit_t t;
			w.trace( p, to, lo, hi, t );
			steep = !t.start_solid && t.frac < 1.f && t.n.z < 0.7f;
			return t.start_solid || ( t.frac < 1.f && t.n.z >= 0.7f );
		};
		const float x0 = h.mins.x, x1 = h.maxs.x, y0 = h.mins.y, y1 = h.maxs.y;
		bool steep = false, q = false;
		if ( ground( mk( x0, y0, 0.f ), mk( x1, y1, 0.f ), steep ) )
			return true;
		return steep && ( ground( mk( x0, y0, 0.f ), mk( 0.f, 0.f, 0.f ), q ) || ground( mk( 0.f, 0.f, 0.f ), mk( x1, y1, 0.f ), q ) ||
		                  ground( mk( x0, 0.f, 0.f ), mk( 0.f, y1, 0.f ), q ) || ground( mk( 0.f, y0, 0.f ), mk( x1, 0.f, 0.f ), q ) );
	}

	/* the face the hull meets at the arrival p: a flat hull swept in from 2u out, 1/8 under the top ( thin lips ). a plane read off one brush runs on
	   past its end ( nuke -224: brushes 854 / 1192 0.16 deg apart, seam on 854's axial bevel: the arrival met 1192, every gap off 854's plane, all
	   pin=0 ). 1 = out differs: another plane with this top is the one there, or the same plane read the other kind ( a prop flush with a brush ).
	   -1 = no face there: the ledge ends before the arrival ( nuke curb 2960 ends x -569, hull at -600, a joist's top under it read as its top ) */
	template < class W >
	int face_at( W& w, const edge_t& f, const hull_t& h, const v3& p, edge_t& out )
	{
		const float ext = std::min( f.n.x * h.mins.x, f.n.x * h.maxs.x ) + std::min( f.n.y * h.mins.y, f.n.y * h.maxs.y );
		const float s = along( f, p ), r0 = f.d - ext + 2.f, r1 = f.d - ext - 1.f, z = f.top - 0.125f;
		hit_t t;
		w.trace( mk( f.n.x * r0 - f.n.y * s, f.n.y * r0 + f.n.x * s, z ), mk( f.n.x * r1 - f.n.y * s, f.n.y * r1 + f.n.x * s, z ), mk( h.mins.x, h.mins.y, 0.f ),
		         mk( h.maxs.x, h.maxs.y, 0.f ), t );
		if ( !t.start_solid && t.frac >= 1.f )
			return -1;
		if ( t.start_solid || fabsf( t.n.z ) > 1e-4f || !t.moves || t.ladder || t.disp )
			return 0;
		const float l = sqrtf( t.n.x * t.n.x + t.n.y * t.n.y );
		edge_t e;
		e.n = mk( t.n.x / l, t.n.y / l, 0.f );
		/* near parallel only ( 5 deg ): a corner's turned face ( nuke 1192 21 deg ) is no plane for the hull reaching past its end ( lab: 7 stood ) */
		if ( e.n.x * f.n.x + e.n.y * f.n.y < 0.9962f )
			return 0;
		e.d     = ( t.d - t.n.z * f.top ) / l;
		e.top   = f.top;
		e.prop  = !t.world;
		e.brush = !t.vp;
		if ( same( e, f ) ) {
			out = f;
			absorb( out, e );
			return out.prop != f.prop || out.brush != f.brush || out.d != f.d || out.top != f.top ? 1 : 0;
		}
		if ( !top_at( w, e, h, t.end, f.top + 1.f, f.top - 1.f, e.top ) || fabsf( e.top - f.top ) >= k_ztol )
			return 0;
		e.d = ( t.d - t.n.z * e.top ) / l;
		out = e;
		return 1;
	}

	/* ledges under the feet within range: 8 yaws + the way you fly, a hull curtain from the feet down the fall. the curtain ends above what lies
	   under the hull ( a floor inside it = every sweep starts solid = blind ). a hit that is no ledge is swept over: the curtain lifts 0.5 above it */
	template < class W >
	int scan( W& w, const st_t& s, const hull_t& h, const float range, const bool riding, edge_t* out, const int max_out )
	{
		const float gz  = -2.f * w.pin / w.dt;
		constexpr float k_t = 0.75f;
		float dz = std::clamp( -( s.v.z + w.pin ) * k_t + 0.5f * gz * k_t * k_t, 4.f, 256.f );
		hit_t f;
		w.trace( s.o, mk( s.o.x, s.o.y, s.o.z - dz ), mk( h.mins.x, h.mins.y, 0.f ), mk( h.maxs.x, h.maxs.y, 0.f ), f );
		if ( !f.start_solid && f.frac < 1.f )
			dz = std::max( f.frac * dz - 0.5f, 0.5f );
		if ( riding )
			dz = std::min( dz, 2.f );
		v3 dirs[ 9 ];
		int nd = 0;
		for ( int i = 0; i < 8; ++i ) {
			const float a = static_cast< float >( i < 4 ? i * 90 : ( i - 4 ) * 90 + 45 ) / k_rad;
			dirs[ nd++ ]  = mk( cosf( a ), sinf( a ), 0.f );
		}
		if ( const float vxy = sqrtf( s.v.x * s.v.x + s.v.y * s.v.y ); vxy >= 1.f )
			dirs[ nd++ ] = mk( s.v.x / vxy, s.v.y / vxy, 0.f );
		int n = 0;
		/* a vertical face hit: its ledge ( top under the hull, no floor 2u beside it ) joins the list. true = a ledge */
		const auto face = [ & ]( const hit_t& t, const float z_lo ) {
			/* a displacement's vertical hit = its triangle box side, no wall ( nuke disp 441: x 1560 top -368 mound, every arrival pin=0 ). a disp top
			   over a brush face still counts: the face hit is the brush */
			if ( fabsf( t.n.z ) > 1e-4f || !t.moves || t.ladder || t.disp )
				return false;
			const float l = sqrtf( t.n.x * t.n.x + t.n.y * t.n.y );
			edge_t e;
			e.n = mk( t.n.x / l, t.n.y / l, 0.f );
			e.d = t.d / l;
			/* an entity box ( neither ) reads both: every arrival kind is flown, the sims pick */
			e.prop  = !t.world;
			e.brush = !t.vp;
			bool ok = top_at( w, e, h, s.o, s.o.z + 1.f, z_lo, e.top ) || top_at( w, e, h, t.end, s.o.z + 1.f, z_lo, e.top );
			if ( ok ) {
				/* a face a hair off vertical ( vphysics ): its plane at the top's height. a floor 2u beside the top = nothing to eb off ( displacement
				   triangle box sides on terrain ): never a candidate, no guide, no hold */
				e.d = ( t.d - t.n.z * e.top ) / l;
				ok  = e.top < s.o.z && !floor_near( w, e, h, along( e, s.o ) );
			}
			if ( !ok )
				return false;
			edge_t* const o = std::find_if( out, out + n, [ & ]( const edge_t& x ) { return same( x, e ); } );
			if ( o == out + n )
				out[ n++ ] = e;
			else
				absorb( *o, e );
			return true;
		};
		/* the top under the hull ( you would land on it ): the curtain ends above it = blind to its own faces. sweep in from outside, 1u under that
		   top, toward the feet: the faces hit are edges you can still leave over ( log 11-21: off the curb at 125 u/s, found beside = too late ) */
		if ( !riding && !f.start_solid && f.frac < 1.f && f.n.z >= 0.7f ) {
			const float zp = f.end.z - 1.f;
			for ( int i = 0; i < nd && n < max_out; ++i ) {
				hit_t t;
				w.trace( mk( s.o.x + dirs[ i ].x * range, s.o.y + dirs[ i ].y * range, zp ), mk( s.o.x, s.o.y, zp ), mk( h.mins.x, h.mins.y, 0.f ),
				         mk( h.maxs.x, h.maxs.y, 0.f ), t );
				if ( !t.start_solid && t.frac < 1.f && t.n.x * dirs[ i ].x + t.n.y * dirs[ i ].y > 0.5f )
					face( t, zp - 0.5f );
			}
		}
		for ( int i = 0; i < nd && n < max_out; ++i ) {
			float bz = dz;
			for ( int pass = 0; pass < 4; ++pass ) {
				hit_t t;
				w.trace( s.o, mk( s.o.x + dirs[ i ].x * range, s.o.y + dirs[ i ].y * range, s.o.z ), mk( h.mins.x, h.mins.y, -bz ), mk( h.maxs.x, h.maxs.y, 0.f ), t );
				if ( t.start_solid || t.frac >= 1.f )
					break;
				if ( face( t, s.o.z - bz - 0.1f ) )
					break;
				if ( pass == 3 )
					break;
				const v3 p = mk( t.end.x + dirs[ i ].x * 0.25f, t.end.y + dirs[ i ].y * 0.25f, t.end.z );
				hit_t up;
				w.trace( p, mk( p.x, p.y, p.z - bz ), mk( h.mins.x, h.mins.y, 0.f ), mk( h.maxs.x, h.maxs.y, 0.f ), up );
				const float next = up.frac * bz - 0.5f;
				if ( up.start_solid || up.frac >= 1.f || next < 0.5f )
					break;
				bz = next;
			}
		}
		return n;
	}

	struct in_t {
		float range = 96.f, reach = 6.f, turn = 0.f;
		bool takeover = false; /* edgebug's option: only a pin START we never sent ( an eb end ) or our own chain */
		bool eb_guide = false; /* edgebug's guide: one pin then the drop ( no ride, no lift ), never holds off or brakes ( H / V ) */
	};
	struct out_t {
		bool own = false, pin = false;
		mv_t mv{ };
		char act = 'N';
	};
	struct bad_t {
		edge_t e{ };
		bool duck = false;
		int fails = 0;
	};
	struct mem_t {
		int cmd = -1;
		bool own = false, sent_pin = false, next_ok = false;
		mv_t next{ };
		edge_t face{ };
		bad_t bad[ 8 ]{ };
		int n_bad = 0;
		/* act Q stood you up: your held duck stays off until the feet pass that top */
		int up_cmd    = -1;
		float up_top  = 0.f;
		/* an owned approach handed back ( Y ): its let go rules still hold it this fall */
		edge_t guard{ };
		bool guard_ok = false;
		/* every ledge read this fall: a prop / brush read sticks, the exact brush plane wins */
		edge_t seen[ 8 ]{ };
		int n_seen = 0;
		edge_t cand[ k_cand ]{ };
		int n_cand = 0;
		bool pick_ok = false, arrive_ok = false;
		edge_t pick{ };
		v3 arrive{ };
		char act = 'N';
	};

	inline bad_t* bad_of( mem_t& m, const edge_t& e, const bool duck )
	{
		for ( int i = 0; i < std::min( m.n_bad, 8 ); ++i )
			if ( m.bad[ i ].duck == duck && same( m.bad[ i ].e, e ) )
				return &m.bad[ i ];
		return nullptr;
	}
	inline void fail( mem_t& m, const edge_t& e, const bool duck )
	{
		bad_t* b = bad_of( m, e, duck );
		if ( !b ) {
			b  = &m.bad[ m.n_bad++ % 8 ];
			*b = { e, duck, 0 };
		}
		++b->fails;
	}
	/* grounded / key up: misses and guards are about one fall, a new jump starts clean */
	inline void new_fall( mem_t& m )
	{
		m.n_bad    = 0;
		m.n_seen   = 0;
		m.guard_ok = false;
		m.own      = false;
	}

	/* one cmd. START is the engine's, the move it returns is flown as is ( its sim proved it ) */
	template < class W >
	out_t tick( W& w, mem_t& m, const int cmd_no, const in_t& in )
	{
		out_t r;
		const bool chain = m.cmd == cmd_no - 1;
		const bool own0 = chain && m.own, sent0 = chain && m.sent_pin, next0_ok = chain && m.next_ok;
		const mv_t next0   = m.next;
		const edge_t face0 = m.face;
		m.cmd              = cmd_no;
		m.own = m.sent_pin = m.next_ok = false;
		m.n_cand                       = 0;
		m.pick_ok = m.arrive_ok = false;
		m.act                   = 'N';
		const st_t s = w.start( );
		if ( s.ground )
			new_fall( m );
		if ( !s.walk || s.ground || s.v.z > w.pin + 1e-3f )
			return r;
		const bool riding0 = fabsf( s.v.z - w.pin ) < 1e-3f;
		if ( in.takeover && !own0 && !riding0 )
			return r;
		const int sims0 = w.used( );
		const hull_t h  = w.hull( s.duck );
		const float off = h.maxs.z * 0.5f;
		edge_t c[ k_cand + 1 ];
		int nc = scan( w, s, h, riding0 ? 2.05f : std::max( in.range, in.reach ), riding0, c, k_cand );
		/* each ledge as the hull meets it on its crossing tick ( along your travel: presses only move the closing ). raw = as read: an owned plan,
		   a guard or a miss count swapped onto the real plane still finds it */
		edge_t raw[ k_cand + 1 ];
		bool gone[ k_cand + 1 ]{ };
		const auto reread = [ & ]( const int i ) {
			raw[ i ]  = c[ i ];
			int k     = -1;
			float f0  = 0.f;
			edge_t e;
			if ( riding0 || !crossing( c[ i ].top, s.o.z, s.v.z, off, w.pin, w.dt, k, f0 ) )
				return;
			const float ta = ( static_cast< float >( k ) + f0 ) * w.dt;
			const int fa   = face_at( w, c[ i ], h, mk( s.o.x + s.v.x * ta, s.o.y + s.v.y * ta, s.o.z ), e );
			if ( fa > 0 )
				c[ i ] = e;
			gone[ i ] = fa < 0;
		};
		for ( int i = 0; i < nc; ++i )
			reread( i );
		/* the owned face once the sweeps cannot see it ( the hull over its top ), checked against the re-read planes ( no twin ), then re-read too */
		if ( own0 && s.o.z > face0.top && std::none_of( c, c + nc, [ & ]( const edge_t& e ) { return same( e, face0 ); } ) ) {
			c[ nc ] = face0;
			reread( nc++ );
		}
		for ( int i = 0; i < nc; ++i ) {
			edge_t* const sn = m.seen + std::min( m.n_seen, 8 );
			edge_t* const o  = std::find_if( m.seen, sn, [ & ]( const edge_t& x ) { return same( x, c[ i ] ); } );
			if ( o == sn )
				m.seen[ m.n_seen++ % 8 ] = c[ i ];
			else {
				absorb( *o, c[ i ] );
				c[ i ] = *o;
			}
		}
		for ( int i = 0; i < nc && m.n_cand < k_cand; ++i )
			m.cand[ m.n_cand++ ] = c[ i ];

		const float vxy  = sqrtf( s.v.x * s.v.x + s.v.y * s.v.y );
		const float auth = w.k_acc * w.max_speed * ( s.duck ? k_duck_crop : 1.f );
		struct ev_t {
			int k = -1, score = -1;
			float f0 = 0.f, g = 0.f, cl = 0.f, u = 0.f, hs = 0.f, v = 0.f, danger = FLT_MAX, qv = 0.f, hk = 0.f, loss = 0.f;
			bool riding = false, mine = false, fly = false, upq = false, flip = false, under = true, alt = false;
			char act = 'F', qact = 'F';
		};
		const hull_t hst = w.hull( false );
		const float a_st = w.k_acc * w.max_speed;
		ev_t ev[ k_cand + 1 ];
		bool ok[ k_cand + 1 ]{ };
		for ( int i = 0; i < nc; ++i ) {
			const edge_t& f = c[ i ];
			ev_t& e         = ev[ i ];
			e.g      = gap( f, s.o, h );
			e.cl     = closing( f, s.v );
			e.u      = face_ulp( f, s.o );
			e.hs     = standoff( e.u );
			e.mine   = own0 && ( same( f, face0 ) || same( raw[ i ], face0 ) );
			/* speed this surf costs: presses only move the closing, the pin keeps the along speed */
			e.loss   = vxy - fabsf( s.v.y * f.n.x - s.v.x * f.n.y );
			e.riding = riding0 && fabsf( s.o.z - ( f.top + k_eps ) ) < k_ztol;
			float zc = 0.f;
			if ( !e.riding && !crossing( f.top, s.o.z, s.v.z, off, w.pin, w.dt, e.k, e.f0, &zc ) )
				continue;
			e.hk = zc - f.top;
			if ( e.riding )
				e.score = 16 + ( e.mine ? 8 : 0 );
			else {
				/* still under the hull on the crossing tick ( else the ledge ends before you get there ), no floor 2u under the arrival */
				const float ta = ( static_cast< float >( e.k ) + e.f0 ) * w.dt;
				const v3 pc    = mk( s.o.x + s.v.x * ta, s.o.y + s.v.y * ta, s.o.z );
				float t2       = 0.f;
				const bool top_ok = top_at( w, f, h, pc, s.o.z + 1.f, f.top - 1.f, t2 ) && fabsf( t2 - f.top ) < k_ztol && !floor_near( w, f, h, along( f, pc ) );
				const bool under  = !gone[ i ] && top_ok;
				if ( e.k >= 2 ) {
					/* over the top: the same plan mirrored ( aim = the standoff outside, no face on that side ), it brakes your run out while you are
					   still over the top. the press check keeps those ticks 2u+ up */
					es_guide_t gd = e.g > 0.f ? es_guide( e.g - e.hs, e.g, -e.hs, e.cl, vxy, e.k, auth, in.turn, in.reach, e.mine, true, w.dt, true )
					                          : es_guide( e.hs - e.g, 0.f, -1e9f, -e.cl, vxy, e.k, auth, in.turn, in.reach, e.mine, true, w.dt, true );
					if ( e.g <= 0.f )
						gd.v = -gd.v;
					e.act = gd.act;
					e.v   = gd.v;
				} else
					e.act = e.g <= std::max( e.cl, 30.f ) * static_cast< float >( e.k + 1 ) * w.dt + 1e-3f ? 'K' : 'F';
				/* ducked: stand up early ( air unduck = feet -9, no crop, never starts the re-duck timer ) = a standing pin, the air duck lift, a
				   second pin. planned on the standing crossing + authority. the lift needs the duck back after that pin: 0.4 s since the last
				   FinishDuck, duck speed for this release + that press */
				int ks   = -1;
				float fs = 0.f;
				if ( !vphys( f ) && s.duck && e.k >= 4 && s.o.z - f.top > 9.5f && w.duck_speed >= 5.5f &&
				     crossing( f.top, s.o.z - 9.f, s.v.z, hst.maxs.z * 0.5f, w.pin, w.dt, ks, fs ) && ks >= 3 && fs >= k_min_frac &&
				     w.now + static_cast< float >( ks + 2 ) * w.dt >= w.last_duck + w.duck_gap ) {
					const es_guide_t gq = es_guide( e.g - e.hs, e.g, -e.hs, e.cl, vxy, ks, a_st, in.turn, in.reach, e.mine, true, w.dt );
					/* the standing arrival comes earlier along: its own top + face ( the ducked one may be past the ledge's end ) */
					const float tq      = ( static_cast< float >( ks ) + fs ) * w.dt;
					const v3 pq         = mk( s.o.x + s.v.x * tq, s.o.y + s.v.y * tq, s.o.z );
					float t3            = 0.f;
					edge_t fq;
					e.upq  = gq.act != 'F' && top_at( w, f, hst, pq, s.o.z + 1.f, f.top - 1.f, t3 ) && fabsf( t3 - f.top ) < k_ztol &&
					        !floor_near( w, f, hst, along( f, pq ) ) && face_at( w, f, hst, pq, fq ) >= 0;
					e.qact = gq.act;
					e.qv   = gq.v;
				}
				/* a prop pins over the top then out ( vphysics, log 11-20 ): the crossing must START 2u+ over the top, else CategorizePosition grounds the
				   tick before. a ground jump beside a curb always crosses at the same height: an air duck / unduck ( feet +9 / -9 ) moves that phase.
				   a brush read too with a box phase: no flip ( the box beside pin needs this stance ) */
				bool flip_ok = false;
				if ( f.prop && !( f.brush && e.f0 >= k_min_frac ) && e.k >= 3 && e.hk <= 2.05f ) {
					const hull_t hf = w.hull( !s.duck );
					int kf          = -1;
					float ff = 0.f, zf = 0.f;
					e.flip = crossing( f.top, s.o.z + ( s.duck ? -9.f : 9.f ), s.v.z, hf.maxs.z * 0.5f, w.pin, w.dt, kf, ff, &zf ) && kf >= 2 && zf - f.top > 2.05f;
					/* this stance's arrival past the end: the flipped one crosses on another tick, its own top + face */
					if ( e.flip && gone[ i ] ) {
						const float tf = ( static_cast< float >( kf ) + ff ) * w.dt;
						const v3 pf    = mk( s.o.x + s.v.x * tf, s.o.y + s.v.y * tf, s.o.z );
						float t4       = 0.f;
						edge_t ef;
						flip_ok = top_at( w, f, hf, pf, s.o.z + 1.f, f.top - 1.f, t4 ) && fabsf( t4 - f.top ) < k_ztol && face_at( w, f, hf, pf, ef ) >= 0;
						e.flip  = flip_ok;
					}
				}
				/* the ledge ends before you get there ( no stance change reaches it ): no owned bonus, out of the pick */
				e.under = under;
				e.alt   = e.upq || flip_ok;
				if ( gone[ i ] && !e.alt )
					e.mine = false;
				/* a box crossing whose feet start inside the top's 1/32 band hits it at fraction 0 ( MINIMUM_MOVE_FRACTION ): no arrival pin. read both
				   ways: either kind may pin, the sims pick */
				const bool phase = ( f.brush && e.f0 >= k_min_frac ) || ( f.prop && ( e.hk > 2.05f || e.flip ) );
				const bad_t* b   = bad_of( m, f, s.duck );
				if ( !b )
					b = bad_of( m, raw[ i ], s.duck );
				e.fly            = ( ( ( under || flip_ok ) && e.act != 'F' && phase ) || e.upq ) && ( !b || b->fails < 2 || e.mine );
				e.score        = ( e.mine ? 8 : 0 ) + ( e.fly ? 4 : 0 );
				if ( e.g > 0.f && e.cl > 0.f && e.g / e.cl < static_cast< float >( e.k + 1 ) * w.dt )
					e.danger = e.g / e.cl;
			}
			ok[ i ] = true;
		}
		/* owned / flyable, then the ledge you land on unless braked ( beside closing ), then the least speed lost: within 2 u/s of the class's
		   least = a tie ( one band, no chain ), then the nearest. a top you are over ( you land on it first ) and a ride stay in the band: k decides */
		/* a ledge that ends before you get there sits out ( picked only when nothing else is there: hands off, why=e ) */
		bool live = false;
		for ( int i = 0; i < nc; ++i )
			if ( ok[ i ] && !( gone[ i ] && !ev[ i ].alt ) )
				live = true;
		if ( live )
			for ( int i = 0; i < nc; ++i )
				ok[ i ] = ok[ i ] && !( gone[ i ] && !ev[ i ].alt );
		int best = -1;
		for ( int i = 0; i < nc; ++i )
			if ( ok[ i ] && ( best < 0 || ev[ i ].score > ev[ best ].score || ( ev[ i ].score == ev[ best ].score && ev[ i ].danger < ev[ best ].danger ) ) )
				best = i;
		if ( best >= 0 ) {
			const int top_cls = best;
			const auto cls    = [ & ]( const int i ) { return ok[ i ] && ev[ i ].score == ev[ top_cls ].score && ev[ i ].danger == ev[ top_cls ].danger; };
			float lmin        = FLT_MAX;
			for ( int i = 0; i < nc; ++i )
				if ( cls( i ) )
					lmin = std::min( lmin, ev[ i ].loss );
			best = -1;
			for ( int i = 0; i < nc; ++i )
				if ( cls( i ) && ( ev[ i ].loss <= lmin + 2.f || ev[ i ].riding || ev[ i ].g <= 0.f ) &&
				     ( best < 0 || ev[ i ].k < ev[ best ].k || ( ev[ i ].k == ev[ best ].k && ev[ i ].g < ev[ best ].g ) ) )
					best = i;
		}
		/* a pinned START on no ledge top here = another trick's ride ( texture bug / pixel surf ): never ours */
		if ( best < 0 || ( riding0 && !ev[ best ].riding ) )
			return r;
		const edge_t f = c[ best ];
		const ev_t& e  = ev[ best ];
		m.pick         = f;
		m.pick_ok      = true;
		if ( !e.riding ) {
			const float ta = ( static_cast< float >( e.k ) + e.f0 ) * w.dt;
			m.arrive       = mk( s.o.x + s.v.x * ta, s.o.y + s.v.y * ta, f.top );
			m.arrive_ok    = true;
		}

		char why = '-';
		const auto log = [ & ]( const char act ) {
			if ( !w.debug )
				return;
			char buf[ 384 ];
			snprintf( buf, sizeof( buf ),
			          "ESURF: act=%c why=%c top=%.4f n=%.4f %.4f d=%.4f prop=%d hk=%.2f gap=%.3e kc=%d f0=%.4f close=%.4f sims=%d vz=%.2f xy=%.1f duck=%d own=%d pos=%.2f %.2f %.3f loss=%.1f n_c=%d",
			          act, why, f.top, f.n.x, f.n.y, f.d, f.prop ? ( f.brush ? 2 : 1 ) : 0, e.hk, e.g, e.k, e.f0, e.cl, w.used( ) - sims0, s.v.z, vxy, s.duck ? 1 : 0, m.own ? 1 : 0, s.o.x, s.o.y,
			          s.o.z, e.loss, nc );
			w.note( act, buf );
		};
		const auto own = [ & ]( const char act, const mv_t& mv, const bool pin ) {
			r.own      = true;
			r.pin      = pin;
			r.mv       = mv;
			r.act      = act;
			m.own      = true;
			m.face     = f;
			m.sent_pin = pin;
			m.act      = act;
			if ( pin )
				if ( bad_t* b = bad_of( m, f, s.duck ) )
					b->fails = 0;
			log( act );
			return r;
		};
		/* no plan: hands off, unless an owned / in reach approach could still close onto the ledge ( over the top: out, beside: the closing that
		   never reaches the face before the crossing, held while your keys could get there: a press adds 30 u/s, more when turning your speed into it ) */
		const bool guarded = m.guard_ok && ( same( f, m.guard ) || same( raw[ best ], m.guard ) );
		const auto hands_off = [ & ]( ) {
			if ( e.mine ) {
				m.guard    = f;
				m.guard_ok = true;
			}
			m.act = 'Y';
			log( 'Y' );
			return r;
		};
		const auto let_go = [ & ]( const char w_ ) {
			why = w_;
			/* eb guide: never holds off */
			if ( in.eb_guide )
				return hands_off( );
			/* in reach = beside: a ledge you are over with no plan was never ours, you land on it */
			if ( !( e.mine || guarded || ( e.g > 0.f && e.g <= in.reach ) ) || e.riding || s.o.z <= f.top )
				return hands_off( );
			/* over the top: out, flown ( a slow fall lands before it leaves = try the full brake ) */
			if ( e.g <= 0.f ) {
				mv_t out = press( w, f, e.cl, std::min( -30.f, e.cl ), s.duck );
				for ( const float c : { std::min( -30.f, e.cl ), e.cl - auth } ) {
					out = press( w, f, e.cl, c, s.duck );
					if ( w.sims_left( ) <= 0 )
						break;
					w.reset( );
					if ( !w.sim( out ).ground )
						break;
				}
				return own( 'H', out, false );
			}
			/* beside a prop: hands off, its pins are edgebug shaped ( log 11-20: H holds there blocked the eb plans that pinned the curb ) */
			if ( vphys( f ) )
				return hands_off( );
			/* diagonal: closing 0 is not parked, the along rounding walks the plane distance ~1e-4 a tick: hold 2e-3 off */
			const bool axial  = fabsf( f.n.x ) > 0.9999f || fabsf( f.n.y ) > 0.9999f;
			const float v_ok  = std::max( 0.f, ( e.g - ( axial ? e.u : 2e-3f ) ) / ( static_cast< float >( e.k + 1 ) * w.dt ) );
			const float v_1   = std::max( e.cl, 0.f ) + std::max( 30.f, std::min( w.k_acc * w.max_speed, 30.f + vxy ) );
			const float brake = std::max( auth, 1.f );
			if ( e.cl <= v_ok && e.g > v_1 * w.dt * ( 2.f + v_1 / ( 2.f * brake ) ) + 0.05f )
				return hands_off( );
			return own( 'H', press( w, f, e.cl, std::min( e.cl, v_ok ), s.duck ), false );
		};

		if ( e.riding ) {
			mv_t mv;
			if ( !in.eb_guide && find_pin( w, f, h, s, [ & ] { w.reset( ); }, e.u, mv ) )
				return own( 'C', mv, true );
			/* standing: an air duck lifts the feet 9 ( FinishDuck ), the fall back is a second arrival. the sim says if the engine lets the duck in.
			   never on a prop: the 9u fall back steps < 2u, its over the top pin needs more */
			if ( !in.eb_guide && !s.duck && !vphys( f ) && w.sims_left( ) > 0 ) {
				const mv_t up = press( w, f, e.cl, 0.f, true );
				w.reset( );
				const st_t e1   = w.sim( up );
				const hull_t hd = w.hull( true );
				int k1          = -1;
				float f1        = 0.f;
				if ( e1.walk && !e1.ground && e1.duck && e1.o.z > s.o.z + 8.f && crossing( f.top, e1.o.z, e1.v.z, hd.maxs.z * 0.5f, w.pin, w.dt, k1, f1 ) && k1 >= 2 ) {
					const float g1 = gap( f, e1.o, hd );
					const es_guide_t gd = es_guide( g1 - e.hs, g1, -e.hs, closing( f, e1.v ), vxy, k1, w.k_acc * w.max_speed * k_duck_crop, in.turn, in.reach, true, true, w.dt );
					if ( g1 > 0.f && gd.act != 'F' )
						return own( 'U', up, false );
				}
			}
			/* an eb end we cannot continue stays yours */
			if ( !sent0 && !own0 ) {
				why = 'n';
				log( 'Y' );
				return r;
			}
			/* drop beside it: closing 0, unless that freezes ( a 1 ulp gap where hull centre + 16 crosses a power of two reads touching ) */
			const auto fell = [ & ]( const st_t& q ) { return q.walk && !q.ground && q.o.z < s.o.z - 1e-3f; };
			/* eb guide: a coast that falls keeps your speed ( a leaving eb ), else the drop */
			if ( in.eb_guide && w.sims_left( ) > 0 ) {
				mv_t cm;
				cm.duck = s.duck;
				w.reset( );
				if ( fell( w.sim( cm ) ) ) {
					why = 'c';
					own( 'E', cm, false );
					m.own = false;
					return r;
				}
			}
			mv_t dm = press( w, f, e.cl, 0.f, s.duck );
			st_t q  = s;
			if ( w.sims_left( ) > 0 ) {
				w.reset( );
				q = w.sim( dm );
			}
			for ( int j = 1; j <= 3 && !fell( q ) && w.sims_left( ) > 0; ++j ) {
				dm = press( w, f, e.cl, -static_cast< float >( j ) * e.u / w.dt, s.duck );
				w.reset( );
				q = w.sim( dm );
				why = 'f';
			}
			own( 'E', dm, false );
			m.own = false;
			return r;
		}
		/* no face where the hull gets there ( and no stance change that does ): nothing to hold off from either */
		if ( gone[ best ] && !e.alt ) {
			why = 'e';
			return hands_off( );
		}
		if ( !e.fly && !e.mine )
			return let_go( 'n' );

		if ( e.k == 0 ) {
			mv_t mv;
			if ( next0_ok && w.sims_left( ) > 0 ) {
				w.reset( );
				if ( verdict( w, f, h, w.sim( next0 ), vxy ) == k_pin )
					return own( 'P', next0, true );
			}
			const bool axial = fabsf( f.n.x ) > 0.9999f || fabsf( f.n.y ) > 0.9999f;
			if ( e.f0 >= k_min_frac && find_pin( w, f, h, s, [ & ] { w.reset( ); }, e.u, mv, axial ? 0 : 12 ) )
				return own( 'P', mv, true );
			if ( e.mine )
				fail( m, f, s.duck );
			return let_go( 'p' );
		}
		if ( e.k == 1 ) {
			mv_t a, b;
			if ( arrive( w, f, h, s, off, e.u, e.f0, a, b ) ) {
				m.next = b;
				own( 'A', a, false );
				m.next_ok = true;
				return r;
			}
			if ( !e.mine )
				return let_go( 'a' );
			why = 'a';
			return own( 'K', press( w, f, e.cl, std::clamp( ( e.g - e.hs ) / w.dt, -auth, auth ), s.duck ), false );
		}

		/* act T: prop crossing too low over the top, the other stance crosses 2u+ over it: this tick's guide press with the duck flipped, flown */
		if ( e.flip && w.sims_left( ) > 1 ) {
			const bool d  = !s.duck;
			const mv_t q  = e.act == 'D' ? turn( w, f, s.v, e.v, d ) : press( w, f, e.cl, e.act == 'F' ? 0.f : e.v, d );
			w.reset( );
			const st_t e1 = w.sim( q );
			if ( e1.walk && !e1.ground && e1.duck == d && gap( f, e1.o, w.hull( d ) ) > 0.f )
				return own( 'T', q, false );
		}
		/* act Q: the standing plan's press with the duck let go, flown: it must END standing, airborne, beside the face, the standing guide still
		   holding a plan from there */
		if ( e.upq && w.sims_left( ) > 1 ) {
			const mv_t q = e.qact == 'D' ? turn( w, f, s.v, e.qv, false ) : press( w, f, e.cl, e.qv, false );
			w.reset( );
			const st_t e1  = w.sim( q );
			int k1         = -1;
			float f1       = 0.f;
			const float g1 = gap( f, e1.o, hst );
			const bool up_ok = e1.walk && !e1.ground && !e1.duck && g1 > 0.f && crossing( f.top, e1.o.z, e1.v.z, hst.maxs.z * 0.5f, w.pin, w.dt, k1, f1 ) && k1 >= 2;
			const es_guide_t gd =
			    up_ok ? es_guide( g1 - e.hs, g1, -e.hs, closing( f, e1.v ), vxy, k1, a_st, in.turn, in.reach, true, true, w.dt ) : es_guide_t{ };
			if ( w.debug ) {
				char buf[ 160 ];
				snprintf( buf, sizeof( buf ), "ESURF: stand up ok=%d act=%c kc=%d -> %d gap=%.3e close=%.3f", up_ok ? 1 : 0, gd.act, e.k, k1, g1, closing( f, e1.v ) );
				w.note( 'q', buf );
			}
			if ( gd.act != 'F' ) {
				m.up_cmd = cmd_no;
				m.up_top = f.top;
				return own( 'Q', q, false );
			}
		}
		/* T / Q refused: this stance's own arrival has no ledge ( gone, no top, floor ), never guide onto it */
		if ( !e.under ) {
			why = 'u';
			return gone[ best ] ? hands_off( ) : let_go( 'u' );
		}
		if ( e.act == 'F' )
			return let_go( 'g' );
		mv_t mv = e.act == 'D' ? turn( w, f, s.v, e.v, s.duck ) : press( w, f, e.cl, e.v, s.duck );
		/* every guide press flown once: it must END beside the face, above the top, never through half the standoff faster than a creep. from over
		   the top it may END still over it 2u+ up ( closer = CategorizePosition lands you ) */
		const auto ok_end = [ & ]( const st_t& q ) {
			if ( !q.walk || q.ground )
				return false;
			const float gq = gap( f, q.o, h );
			if ( gq <= 0.f )
				return e.g <= 0.f && q.o.z > f.top + 2.05f;
			return q.o.z > f.top + k_eps && ( gq >= 0.5f * e.hs || closing( f, q.v ) * w.dt <= 0.5f * e.hs );
		};
		if ( w.sims_left( ) <= 0 )
			return let_go( 's' );
		w.reset( );
		if ( ok_end( w.sim( mv ) ) )
			return own( e.act, mv, false );
		mv = press( w, f, e.cl, 0.f, s.duck );
		w.reset( );
		if ( !in.eb_guide && w.sims_left( ) > 0 && ok_end( w.sim( mv ) ) ) {
			why = e.act;
			return own( 'V', mv, false );
		}
		return let_go( 'v' );
	}
}
