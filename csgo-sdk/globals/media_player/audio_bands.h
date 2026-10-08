#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace n_audio_bands
{
	constexpr std::size_t k_fft_size = 2048; // ~43 ms at 48 kHz, 23 Hz bins so the bass bands don't all share one bin
	constexpr std::size_t k_bands    = 64;
	constexpr float k_low_hz         = 50.f;
	constexpr float k_high_hz        = 16000.f;
	constexpr float k_floor_db       = -66.f;
	constexpr float k_ceiling_db     = -12.f;

	inline void fft( std::complex< float >* data, const std::size_t n )
	{
		for ( std::size_t i = 1, j = 0; i < n; i++ ) {
			std::size_t bit = n >> 1;
			for ( ; j & bit; bit >>= 1 )
				j ^= bit;
			j ^= bit;
			if ( i < j )
				std::swap( data[ i ], data[ j ] );
		}

		for ( std::size_t length = 2; length <= n; length <<= 1 ) {
			const float angle = -6.28318530718f / static_cast< float >( length );
			const std::complex< float > step( std::cos( angle ), std::sin( angle ) );

			for ( std::size_t start = 0; start < n; start += length ) {
				std::complex< float > w( 1.f, 0.f );
				for ( std::size_t k = 0; k < length / 2; k++ ) {
					const std::complex< float > even = data[ start + k ];
					const std::complex< float > odd  = data[ start + k + length / 2 ] * w;
					data[ start + k ]                = even + odd;
					data[ start + k + length / 2 ]   = even - odd;
					w *= step;
				}
			}
		}
	}

	inline std::size_t band_edge( const std::size_t band, const float sample_rate )
	{
		const float high = ( std::min )( k_high_hz, sample_rate * 0.5f );
		const float hz   = k_low_hz * std::pow( high / k_low_hz, static_cast< float >( band ) / static_cast< float >( k_bands ) );
		const auto bin   = static_cast< std::size_t >( hz * static_cast< float >( k_fft_size ) / sample_rate );
		return std::clamp< std::size_t >( bin, 1, k_fft_size / 2 - 1 );
	}

	/* samples = k_fft_size mono, oldest first, -1..1. out = k_bands levels 0..1. one caller thread ( static scratch ) */
	inline void compute( const float* samples, const float sample_rate, float* out )
	{
		static std::complex< float > data[ k_fft_size ];

		for ( std::size_t i = 0; i < k_fft_size; i++ ) {
			const float hann = 0.5f - 0.5f * std::cos( 6.28318530718f * static_cast< float >( i ) / static_cast< float >( k_fft_size - 1 ) );
			data[ i ]        = std::complex< float >( samples[ i ] * hann, 0.f );
		}

		fft( data, k_fft_size );

		for ( std::size_t band = 0; band < k_bands; band++ ) {
			const std::size_t first = band_edge( band, sample_rate );
			const std::size_t last  = ( std::max )( first + 1, band_edge( band + 1, sample_rate ) );

			float peak = 0.f;
			for ( std::size_t bin = first; bin < last && bin < k_fft_size / 2; bin++ )
				peak = ( std::max )( peak, std::abs( data[ bin ] ) );

			const float amplitude = peak * 2.f / static_cast< float >( k_fft_size );
			const float db        = 20.f * std::log10( amplitude + 1e-9f );
			out[ band ]           = std::clamp( ( db - k_floor_db ) / ( k_ceiling_db - k_floor_db ), 0.f, 1.f );
		}
	}

	inline float bar_level( const float* bands, const std::size_t bar, const std::size_t bars )
	{
		const std::size_t first = bar * k_bands / bars;
		const std::size_t last  = ( std::max )( first + 1, ( bar + 1 ) * k_bands / bars );

		float level = 0.f;
		for ( std::size_t band = first; band < last && band < k_bands; band++ )
			level = ( std::max )( level, bands[ band ] );
		return level;
	}
}
