#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/* COD4 menu expressions as the dumped .menu text writes them: `when(...)` bodies, `exp rect Y (...)`,
   `exp text (...)`. numbers, "strings", calls, ! - unary, * / %, + -, < > <= >=, == !=, &&, ||.
   pure, tools/cod4_hud_test.cpp runs every expression of every menu through it */
namespace n_cod4_expr
{
	struct value_t {
		bool m_is_string = false;
		double m_number  = 0.0;
		std::string m_string{ };

		static value_t num( double v ) { return { false, v, { } }; }
		static value_t str( std::string s ) { return { true, 0.0, std::move( s ) }; }

		double number( ) const { return m_is_string ? std::atof( m_string.c_str( ) ) : m_number; }
		bool truth( ) const { return m_is_string ? !m_string.empty( ) : m_number != 0.0; }
		std::string text( ) const
		{
			if ( m_is_string )
				return m_string;
			if ( m_number == std::floor( m_number ) && std::fabs( m_number ) < 1e15 )
				return std::to_string( static_cast< long long >( m_number ) );
			char b[ 32 ];
			snprintf( b, sizeof( b ), "%g", m_number );
			return b;
		}
	};

	// the hud supplies the game state behind each function name
	struct context_t {
		virtual ~context_t( ) = default;
		virtual value_t call( std::string_view name, const std::vector< value_t >& args ) = 0;
	};

	struct node_t {
		enum kind_t { number, string, call, unary, binary } m_kind = number;
		double m_number = 0.0;
		std::string m_text{ }; // string literal, function name, operator
		std::vector< std::unique_ptr< node_t > > m_kids{ };
	};

	class parser_t
	{
		std::string_view m_src;
		size_t m_pos = 0;
		bool m_ok    = true;

		void skip( )
		{
			while ( m_pos < m_src.size( ) && ( m_src[ m_pos ] == ' ' || m_src[ m_pos ] == '\t' || m_src[ m_pos ] == '\n' || m_src[ m_pos ] == '\r' ) )
				++m_pos;
		}

		bool eat( std::string_view op )
		{
			skip( );
			if ( m_src.substr( m_pos, op.size( ) ) == op ) {
				m_pos += op.size( );
				return true;
			}
			return false;
		}

		bool peek( std::string_view op )
		{
			skip( );
			return m_src.substr( m_pos, op.size( ) ) == op;
		}

		static std::unique_ptr< node_t > bin( std::string op, std::unique_ptr< node_t > a, std::unique_ptr< node_t > b )
		{
			auto n    = std::make_unique< node_t >( );
			n->m_kind = node_t::binary;
			n->m_text = std::move( op );
			n->m_kids.push_back( std::move( a ) );
			n->m_kids.push_back( std::move( b ) );
			return n;
		}

		std::unique_ptr< node_t > primary( )
		{
			skip( );
			if ( m_pos >= m_src.size( ) ) {
				m_ok = false;
				return std::make_unique< node_t >( );
			}

			const char c = m_src[ m_pos ];
			if ( c == '(' ) {
				++m_pos;
				auto e = expr( );
				if ( !eat( ")" ) )
					m_ok = false;
				return e;
			}
			if ( c == '!' || c == '-' ) {
				++m_pos;
				auto n    = std::make_unique< node_t >( );
				n->m_kind = node_t::unary;
				n->m_text = std::string( 1, c );
				n->m_kids.push_back( primary( ) );
				return n;
			}
			if ( c == '"' ) {
				const size_t end = m_src.find( '"', m_pos + 1 );
				if ( end == std::string_view::npos ) {
					m_ok = false;
					return std::make_unique< node_t >( );
				}
				auto n    = std::make_unique< node_t >( );
				n->m_kind = node_t::string;
				n->m_text = std::string( m_src.substr( m_pos + 1, end - m_pos - 1 ) );
				m_pos     = end + 1;
				return n;
			}
			if ( ( c >= '0' && c <= '9' ) || c == '.' ) {
				size_t end = m_pos;
				while ( end < m_src.size( ) && ( ( m_src[ end ] >= '0' && m_src[ end ] <= '9' ) || m_src[ end ] == '.' ) )
					++end;
				auto n      = std::make_unique< node_t >( );
				n->m_number = std::atof( std::string( m_src.substr( m_pos, end - m_pos ) ).c_str( ) );
				m_pos       = end;
				return n;
			}
			if ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_' ) {
				size_t end = m_pos;
				while ( end < m_src.size( ) &&
				        ( ( m_src[ end ] >= 'a' && m_src[ end ] <= 'z' ) || ( m_src[ end ] >= 'A' && m_src[ end ] <= 'Z' ) ||
				          ( m_src[ end ] >= '0' && m_src[ end ] <= '9' ) || m_src[ end ] == '_' ) )
					++end;
				auto n    = std::make_unique< node_t >( );
				n->m_kind = node_t::call;
				n->m_text = std::string( m_src.substr( m_pos, end - m_pos ) );
				m_pos     = end;
				if ( !eat( "(" ) ) {
					m_ok = false;
					return n;
				}
				if ( !eat( ")" ) ) {
					do
						n->m_kids.push_back( expr( ) );
					while ( m_ok && eat( "," ) );
					if ( !eat( ")" ) )
						m_ok = false;
				}
				return n;
			}
			m_ok = false;
			++m_pos;
			return std::make_unique< node_t >( );
		}

		std::unique_ptr< node_t > mul( )
		{
			auto a = primary( );
			for ( ;; ) {
				if ( eat( "*" ) )
					a = bin( "*", std::move( a ), primary( ) );
				else if ( eat( "/" ) )
					a = bin( "/", std::move( a ), primary( ) );
				else if ( eat( "%" ) )
					a = bin( "%", std::move( a ), primary( ) );
				else
					return a;
			}
		}

		std::unique_ptr< node_t > add( )
		{
			auto a = mul( );
			for ( ;; ) {
				if ( eat( "+" ) )
					a = bin( "+", std::move( a ), mul( ) );
				else if ( eat( "-" ) )
					a = bin( "-", std::move( a ), mul( ) );
				else
					return a;
			}
		}

		std::unique_ptr< node_t > rel( )
		{
			auto a = add( );
			for ( ;; ) {
				if ( eat( "<=" ) )
					a = bin( "<=", std::move( a ), add( ) );
				else if ( eat( ">=" ) )
					a = bin( ">=", std::move( a ), add( ) );
				else if ( eat( "<" ) )
					a = bin( "<", std::move( a ), add( ) );
				else if ( eat( ">" ) )
					a = bin( ">", std::move( a ), add( ) );
				else
					return a;
			}
		}

		std::unique_ptr< node_t > eq( )
		{
			auto a = rel( );
			for ( ;; ) {
				if ( eat( "==" ) )
					a = bin( "==", std::move( a ), rel( ) );
				else if ( eat( "!=" ) )
					a = bin( "!=", std::move( a ), rel( ) );
				else
					return a;
			}
		}

		std::unique_ptr< node_t > land( )
		{
			auto a = eq( );
			while ( eat( "&&" ) )
				a = bin( "&&", std::move( a ), eq( ) );
			return a;
		}

	public:
		std::unique_ptr< node_t > expr( )
		{
			auto a = land( );
			while ( eat( "||" ) )
				a = bin( "||", std::move( a ), land( ) );
			return a;
		}

		// nullptr = syntax error
		static std::unique_ptr< node_t > parse( std::string_view src )
		{
			parser_t p{ };
			p.m_src = src;
			auto n  = p.expr( );
			p.skip( );
			while ( p.m_pos < src.size( ) && src[ p.m_pos ] == ';' )
				++p.m_pos;
			p.skip( );
			if ( !p.m_ok || p.m_pos != src.size( ) )
				return nullptr;
			return n;
		}
	};

	inline value_t eval( const node_t& n, context_t& ctx )
	{
		switch ( n.m_kind ) {
		case node_t::number: return value_t::num( n.m_number );
		case node_t::string: return value_t::str( n.m_text );
		case node_t::call: {
			std::vector< value_t > args{ };
			args.reserve( n.m_kids.size( ) );
			for ( const auto& k : n.m_kids )
				args.push_back( eval( *k, ctx ) );
			return ctx.call( n.m_text, args );
		}
		case node_t::unary: {
			const value_t v = eval( *n.m_kids[ 0 ], ctx );
			return value_t::num( n.m_text == "!" ? ( v.truth( ) ? 0.0 : 1.0 ) : -v.number( ) );
		}
		case node_t::binary: break;
		}

		const std::string& op = n.m_text;
		if ( op == "&&" )
			return value_t::num( eval( *n.m_kids[ 0 ], ctx ).truth( ) && eval( *n.m_kids[ 1 ], ctx ).truth( ) ? 1.0 : 0.0 );
		if ( op == "||" )
			return value_t::num( eval( *n.m_kids[ 0 ], ctx ).truth( ) || eval( *n.m_kids[ 1 ], ctx ).truth( ) ? 1.0 : 0.0 );

		const value_t a = eval( *n.m_kids[ 0 ], ctx ), b = eval( *n.m_kids[ 1 ], ctx );
		const bool strings = a.m_is_string || b.m_is_string;

		if ( op == "+" )
			return strings ? value_t::str( a.text( ) + b.text( ) ) : value_t::num( a.m_number + b.m_number );
		if ( op == "==" )
			return value_t::num( ( strings ? a.text( ) == b.text( ) : a.number( ) == b.number( ) ) ? 1.0 : 0.0 );
		if ( op == "!=" )
			return value_t::num( ( strings ? a.text( ) != b.text( ) : a.number( ) != b.number( ) ) ? 1.0 : 0.0 );

		const double x = a.number( ), y = b.number( );
		if ( op == "-" ) return value_t::num( x - y );
		if ( op == "*" ) return value_t::num( x * y );
		// ponytail: x / 0 = 0 (a 0 ui_scorelimit would give inf widths), the game's behaviour there is untested
		if ( op == "/" ) return value_t::num( y != 0.0 ? x / y : 0.0 );
		if ( op == "%" ) return value_t::num( static_cast< long long >( y ) ? static_cast< double >( static_cast< long long >( x ) % static_cast< long long >( y ) ) : 0.0 );
		if ( op == "<" ) return value_t::num( x < y ? 1.0 : 0.0 );
		if ( op == ">" ) return value_t::num( x > y ? 1.0 : 0.0 );
		if ( op == "<=" ) return value_t::num( x <= y ? 1.0 : 0.0 );
		if ( op == ">=" ) return value_t::num( x >= y ? 1.0 : 0.0 );
		return value_t::num( 0.0 );
	}
}
