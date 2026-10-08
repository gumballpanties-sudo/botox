#pragma once
#include <array>

#include <cmath>

struct ImVec4;

struct c_unsigned_char_color {
	unsigned char r, g, b, a;
	constexpr c_unsigned_char_color( ) : r( 0 ), g( 0 ), b( 0 ), a( 255 ) { }

	constexpr c_unsigned_char_color( unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha = 255 )
		: r{ red }, g{ green }, b{ blue }, a{ alpha }
	{
	}

	constexpr static c_unsigned_char_color console_text_color( )
	{
		return c_unsigned_char_color( 180, 180, 180, 255 );
	}
};

enum e_color_type {
	color_type_r = 0,
	color_type_g = 1,
	color_type_b = 2,
	color_type_a = 3
};

class c_color
{
public:
	c_color( ) = default;

	constexpr c_color( std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255 ) : m_colors( { r, g, b, a } ) { }

	constexpr c_color( int r, int g, int b, int a = 255 )
		: m_colors( { static_cast< std::uint8_t >( r ), static_cast< std::uint8_t >( g ), static_cast< std::uint8_t >( b ),
	                  static_cast< std::uint8_t >( a ) } )
	{
	}

	constexpr c_color( float r, float g, float b, float a = 1.0f )
		: m_colors( { static_cast< std::uint8_t >( r * 255.f ), static_cast< std::uint8_t >( g * 255.f ), static_cast< std::uint8_t >( b * 255.f ),
	                  static_cast< std::uint8_t >( a * 255.f ) } )
	{
	}

	template< std::size_t N >
	[[nodiscard]] std::uint8_t get( ) const
	{
		static_assert( N >= e_color_type::color_type_r && N <= e_color_type::color_type_a, "given index is out of range" );
		return this->m_colors[ N ];
	}

	[[nodiscard]] unsigned int get_u32( const float alpha_multiplier = 1.0f ) const;

	[[nodiscard]] ImVec4 get_vec4( const float alpha_multiplier = 1.0f ) const;

	static c_color from_hsb( float hue, float saturation, float brightness, float alpha = 1.0f )
	{
		const float h = std::fmodf( hue, 1.0f ) / ( 60.0f / 360.0f );
		const int i   = static_cast< int >( h );
		const float f = h - static_cast< float >( i );
		const float p = brightness * ( 1.0f - saturation );
		const float q = brightness * ( 1.0f - saturation * f );
		const float t = brightness * ( 1.0f - saturation * ( 1.0f - f ) );

		float r = 0.0f, g = 0.0f, b = 0.0f;

		switch ( i ) {
		case 0:
			r = brightness, g = t, b = p;
			break;
		case 1:
			r = q, g = brightness, b = p;
			break;
		case 2:
			r = p, g = brightness, b = t;
			break;
		case 3:
			r = p, g = q, b = brightness;
			break;
		case 4:
			r = t, g = p, b = brightness;
			break;
		case 5:
		default:
			r = brightness, g = p, b = q;
			break;
		}

		return c_color( r, g, b, alpha );
	}

	std::uint8_t& operator[]( const std::size_t i )
	{
		return this->m_colors[ i ];
	}

	const std::uint8_t& operator[]( const std::size_t i ) const
	{
		return this->m_colors[ i ];
	}

	bool operator==( const c_color& second_color ) const
	{
		return this->m_colors == second_color.m_colors;
	}

	bool operator!=( const c_color& second_color ) const
	{
		return !( operator==( second_color ) );
	}

	c_color& operator=( const c_color& new_color )
	{
		this->m_colors[ e_color_type::color_type_r ] = new_color.m_colors[ e_color_type::color_type_r ];
		this->m_colors[ e_color_type::color_type_g ] = new_color.m_colors[ e_color_type::color_type_g ];
		this->m_colors[ e_color_type::color_type_b ] = new_color.m_colors[ e_color_type::color_type_b ];
		this->m_colors[ e_color_type::color_type_a ] = new_color.m_colors[ e_color_type::color_type_a ];
		return *this;
	}

	template< std::size_t N >
	[[nodiscard]] float base( ) const
	{
		static_assert( N >= e_color_type::color_type_r && N <= e_color_type::color_type_a, "given index is out of range" );
		return this->m_colors[ N ] / 255.f;
	}

	std::array< std::uint8_t, 4U >& get_colors( )
	{
		return m_colors;
	}

private:
	std::array< std::uint8_t, 4U > m_colors = { };
};
